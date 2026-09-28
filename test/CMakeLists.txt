add_executable(recurring_static_message_check_test
    RecurringStaticMessageCheck_GTest.cpp
)
target_link_libraries(recurring_static_message_check_test
    PRIVATE
        recurring_static_message_check_lib
        llvm_clang_includes
        gtest::gtest
)

gtest_discover_tests(recurring_static_message_check_test)
