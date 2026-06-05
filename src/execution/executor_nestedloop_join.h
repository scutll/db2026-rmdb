/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

// 执行多表查询/连接查询->做笛卡尔乘积（两表的记录两两拼接）然后过滤出符合条件的记录
// 这个executor的职责是将两张表的记录合起来，并按照join条件过滤
// 在此之前两个executor已经做了单表过滤操作
class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        left_->beginTuple();

        if(left_->is_end()){
            isend = true;
            return;
        }

        right_->beginTuple();
        isend = false;

        find_next_valid_tuple();
    }

    void nextTuple() override {
        if(isend){
            return;
        }

        right_->nextTuple();
        find_next_valid_tuple();
    }

    std::unique_ptr<RmRecord> Next() override {
        if(isend){
            return nullptr;
        }

        auto left_rec = left_->Next();
        auto right_rec = right_->Next();

        return make_join_record(left_rec.get(), right_rec.get());
    }

    bool is_end() const override
    {
        return isend;
    }

    Rid &rid() override { return _abstract_rid; }

    size_t tupleLen() const override
    {
        return len_;
    }

    const std::vector<ColMeta> &cols() const override
    {
        return cols_;
    }

    std::string getType() override
    {
        return "NestedLoopJoinExecutor";
    }

    ColMeta get_col_offset(const TabCol &target) override
    {
        auto pos = get_col(cols_, target);
        return *pos;
    }

    void find_next_valid_tuple(){
        while(!left_->is_end()){
            while(!right_->is_end()){
                auto l_rec = left_->Next();
                auto r_rec = right_->Next();

                auto join_rec = make_join_record(l_rec.get(), r_rec.get());

                if(eval_conds(fed_conds_, join_rec.get())){
                    return;
                }

                right_->nextTuple();
            }

            left_->nextTuple();

            if(left_->is_end()){
                break;
            }

            right_->beginTuple();
        }

        isend = true;
    }
   private:
    std::unique_ptr<RmRecord> make_join_record(const RmRecord* left_rec, const RmRecord* right_rec){
        auto rec = std::make_unique<RmRecord>(len_);

        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());

        return rec;
    }

    bool eval_conds(const std::vector<Condition> &conds, const RmRecord *rec)
    {
        for (auto &cond : conds)
        {
            if (!eval_cond(cond, rec))
            {
                return false;
            }
        }
        return true;
    }

    bool eval_cond(const Condition &cond, const RmRecord *rec)
    {
        auto lhs_col = get_col(cols_, cond.lhs_col);
        const char *lhs = rec->data + lhs_col->offset;

        int cmp = 0;

        if (cond.is_rhs_val)
        {
            cmp = compare_value(*lhs_col, lhs, *lhs_col, cond.rhs_val.raw->data);
        }
        else
        {
            auto rhs_col = get_col(cols_, cond.rhs_col);
            const char *rhs = rec->data + rhs_col->offset;
            cmp = compare_value(*lhs_col, lhs, *rhs_col, rhs);
        }

        return check_compare_result(cmp, cond.op);
    }

    int compare_value(const ColMeta &lhs_col, const char *lhs,
                      const ColMeta &rhs_col, const char *rhs)
    {
        if (lhs_col.type == TYPE_INT)
        {
            int l = *reinterpret_cast<const int *>(lhs);
            int r = *reinterpret_cast<const int *>(rhs);

            if (l < r)
                return -1;
            if (l > r)
                return 1;
            return 0;
        }

        if (lhs_col.type == TYPE_FLOAT)
        {
            float l = *reinterpret_cast<const float *>(lhs);
            float r = *reinterpret_cast<const float *>(rhs);

            if (l < r)
                return -1;
            if (l > r)
                return 1;
            return 0;
        }

        if (lhs_col.type == TYPE_STRING)
        {
            std::string l(lhs, lhs_col.len);
            std::string r(rhs, rhs_col.len);

            l.resize(strlen(l.c_str()));
            r.resize(strlen(r.c_str()));

            if (l < r)
                return -1;
            if (l > r)
                return 1;
            return 0;
        }

        throw InternalError("Unsupported column type in NestedLoopJoinExecutor");
    }

    bool check_compare_result(int cmp, CompOp op)
    {
        switch (op)
        {
        case OP_EQ:
            return cmp == 0;
        case OP_NE:
            return cmp != 0;
        case OP_LT:
            return cmp < 0;
        case OP_GT:
            return cmp > 0;
        case OP_LE:
            return cmp <= 0;
        case OP_GE:
            return cmp >= 0;
        default:
            throw InternalError("Unsupported comparison operator");
        }
    }
};