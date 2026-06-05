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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

    //判断记录是否满足一个where条件
    bool eval_cond(const Condition &cond, const RmRecord* rec){
        auto lhs_col = get_col(cols_, cond.lhs_col);
        const char *lhs = rec->data + lhs_col->offset;

        int cmp = 0;

        //right hand side是常量
        if(cond.is_rhs_val){
            cmp = compare_value(*lhs_col, lhs, *lhs_col, cond.rhs_val.raw->data);
        }else{
            //rhs是列值
            auto rhs_col = get_col(cols_, cond.rhs_col);
            const char *rhs = rec->data + rhs_col->offset;
            cmp = compare_value(*lhs_col, lhs, *rhs_col, rhs);
        }

        return check_compare_result(cmp, cond.op);
    }

    //判断满足所有where
    bool eval_conds(const std::vector<Condition> &conds, const RmRecord *rec){
        for (auto &cond : conds)
        {
            if (!eval_cond(cond, rec))
            {
                return false;
            }
        }
        return true;
    }

    //按字段类型比较
    //lhs >rhs ->1, lhs == rhs -> 0, lhs < rhs -> -1
    int compare_value(const ColMeta &lhs_col, const char *lhs, const ColMeta &rhs_col, const char *rhs){
        if(lhs_col.type == TYPE_INT){
            int l = *reinterpret_cast<const int *>(lhs);
            int r = *reinterpret_cast<const int *>(rhs);
            if(l < r)
                return -1;
            if(l > r)
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

        throw InternalError("Unsupported column type in SeqScanExecutor");
    }

    //把结构套进比较符
    bool check_compare_result(int cmp, CompOp op){
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

public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    // 开始扫描,定位到第一条可用 tuple
    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        while(!scan_->is_end()){
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if(eval_conds(fed_conds_, rec.get())){
                return;
            }
            scan_->next();
        }
    }

    //移动到下一条可用 tuple
    void nextTuple() override {
        if(scan_ == nullptr || scan_->is_end()){
            return;
        }

        scan_->next();

        while(!scan_->is_end()){
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if(eval_conds(fed_conds_, rec.get())){
                return;
            }
            scan_->next();
        }
    }

    bool is_end() const override{
        return scan_ == nullptr || scan_->is_end();
    }


    // 取出当前tuple的数据
    std::unique_ptr<RmRecord> Next() override {
        if(is_end()){
            return nullptr;
        }

        return fh_->get_record(rid_, context_);
    }

    size_t tupleLen() const override{
        return len_;
    }

    const std::vector<ColMeta>& cols() const override{
        return cols_;
    }

    std::string getType() override
    {
        return "SeqScanExecutor";
    }

    ColMeta get_col_offset(const TabCol &target) override {
        auto pos = get_col(cols_, target);
        return *pos;
    }

    //当前tuple在表文件里的位置
    Rid &rid() override { return rid_; }
};