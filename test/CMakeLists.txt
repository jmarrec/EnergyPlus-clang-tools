add_executable(clang_tools_test
    RecurringStaticMessageCheck_GTest.cpp
    MixedIndexingCheck_GTest.cpp
)
target_link_libraries(clang_tools_test
    PRIVATE
        recurring_static_message_check_lib
        mixed_indexing_check_lib
        llvm_clang_includes
        GTest::gtest_main
)

gtest_discover_tests(clang_tools_test)
