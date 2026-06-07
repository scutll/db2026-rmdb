add_executable(ix_test
    ix_test.cpp
)

target_link_libraries(ix_test
    index
    storage
    lru_replacer
    record
    system
    pthread
)