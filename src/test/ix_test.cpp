#include <cassert>
#include <iostream>
#include <vector>

#include "storage/disk_manager.h"
#include "storage/buffer_pool_manager.h"
#include "index/ix_manager.h"

int main()
{
    std::string db_name = "ix_test_db";

    DiskManager disk_manager;
    BufferPoolManager bpm(BUFFER_POOL_SIZE, &disk_manager);
    IxManager ix_manager(&disk_manager, &bpm);

    std::string index_name = "test";
    std::vector<ColMeta> cols;
    cols.push_back(ColMeta{
        .tab_name = "test",
        .name = "id",
        .type = TYPE_INT,
        .len = sizeof(int),
        .offset = 0,
        .index = true});

    if (disk_manager.is_file("test_id.idx"))
    {
        disk_manager.destroy_file("test_id.idx");
    }

    ix_manager.create_index(index_name, cols);
    auto ih = ix_manager.open_index(index_name, cols);

    // 1. 插入大量 key，触发 split
    for (int i = 1; i <= 1000; i++)
    {
        Rid rid{.page_no = i, .slot_no = i + 10000};

        ih->insert_entry(reinterpret_cast<const char *>(&i), rid, nullptr);

        std::vector<Rid> result;
        bool ok = ih->get_value(reinterpret_cast<const char *>(&i), &result, nullptr);

        if (!ok)
        {
            std::cerr << "lookup failed immediately after insert, key = " << i << std::endl;
            return 1;
        }

        if (result.size() != 1 || result[0].page_no != i || result[0].slot_no != i + 10000)
        {
            std::cerr << "wrong rid after insert, key = " << i
                      << ", result.size = " << result.size()
                      << ", page_no = " << (result.empty() ? -1 : result[0].page_no)
                      << ", slot_no = " << (result.empty() ? -1 : result[0].slot_no)
                      << std::endl;
            return 1;
        }
    }

    std::cout << "insert + point lookup passed" << std::endl;

    // 2. 点查
    for (int i = 1; i <= 1000; i++)
    {
        std::vector<Rid> result;
        bool ok = ih->get_value(reinterpret_cast<char *>(&i), &result, nullptr);

        // assert(ok);
        assert(result.size() == 1);
        assert(result[0].page_no == i);
        assert(result[0].slot_no == i + 10000);
    }

    // 3. 删除一部分 key，触发 redistribute / coalesce
    for (int i = 1; i <= 500; i++)
    {
        bool ok = ih->delete_entry(reinterpret_cast<char *>(&i), nullptr);
        assert(ok);
    }

    // 4. 确认已删除的查不到
    for (int i = 1; i <= 500; i++)
    {
        std::vector<Rid> result;
        bool ok = ih->get_value(reinterpret_cast<char *>(&i), &result, nullptr);
        assert(!ok);
    }

    // 5. 确认未删除的还能查到
    for (int i = 501; i <= 1000; i++)
    {
        std::vector<Rid> result;
        bool ok = ih->get_value(reinterpret_cast<char *>(&i), &result, nullptr);

        assert(ok);
        assert(result.size() == 1);
        assert(result[0].page_no == i);
        assert(result[0].slot_no == i + 10000);
    }

    ix_manager.close_index(ih.get());

    std::cout << "IxIndex basic test passed!" << std::endl;
    return 0;
}