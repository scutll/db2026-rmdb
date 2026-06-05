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


//进行完整记录的裁剪，得到指定列、顺序的输出记录
class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;        // 投影节点的儿子节点
    std::vector<ColMeta> cols_;                     // 需要投影的字段
    size_t len_;                                    // 字段总长度
    std::vector<size_t> sel_idxs_;                  // 输出列->原始列位置的映射           

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);

        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        //计算sel_cols中每个列在prev_cols的位置并放入sel_idxs_
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override {
        prev_->beginTuple();
    }

    void nextTuple() override {
        prev_->nextTuple();
    }

    bool is_end() const override{
        return prev_->is_end();
    }

    size_t tupleLen() const override
    {
        return len_;
    }

    const std::vector<ColMeta> &cols() const override
    {
        return cols_;
    }

    //拿到完整记录，然后进行裁剪，只拷贝选中的列
    std::unique_ptr<RmRecord> Next() override {
        auto prev_rec = prev_->Next();
        if(prev_rec == nullptr){
            return nullptr;
        }

        auto rec = std::make_unique<RmRecord>(len_);
        auto &prev_cols = prev_->cols();

        for (size_t i = 0; i < sel_idxs_.size(); i++){
            const ColMeta &prev_col = prev_cols[sel_idxs_[i]];
            const ColMeta &out_col = cols_[i];

            memcpy(rec->data + out_col.offset, prev_rec->data + prev_col.offset, out_col.len);
        }

        return rec;
    }

    std::string getType() override
    {
        return "ProjectionExecutor";
    }

    //获取target列在cols_的iter
    ColMeta get_col_offset(const TabCol &target) override
    {
        auto pos = get_col(cols_, target);
        return *pos;
    }

    Rid &rid() override { return _abstract_rid; }
};