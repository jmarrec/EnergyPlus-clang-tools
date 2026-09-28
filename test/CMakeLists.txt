add_executable(path_format_fixer_test
    Fixture.cpp
    Fixture.hpp
    PathFormatFixerTest.cpp
)
target_link_libraries(path_format_fixer_test
    PRIVATE
        path_format_fixer_lib
        llvm_clang_includes
        gtest::gtest
        fmt::fmt
)

# clang::tooling::runToolOnCodeWithArgs() (used by runOnCode() in Fixture.cpp)
# builds a from-scratch compiler invocation that doesn't inherit any of our
# build flags, so on macOS it can resolve <filesystem>/<format> against the
# active SDK's bundled libc++ -- which can be newer than what CMAKE_CXX_COMPILER
# (Homebrew's clang) handles, e.g. tripping over `__has_include(<picolibc.h>)`.
# Point it at the compiler's own resource dir + libc++ headers so the in-memory
# compile is self-consistent with the compiler that's actually running it.
if(APPLE)
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE PATH_FORMAT_FIXER_TEST_CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  get_filename_component(_clang_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_clang_libcxx_dir "${_clang_bin_dir}/../include/c++/v1" ABSOLUTE)
  if(EXISTS "${_clang_libcxx_dir}")
    target_compile_definitions(path_format_fixer_test PRIVATE
      "PATH_FORMAT_FIXER_TEST_CLANG_RESOURCE_DIR=\"${PATH_FORMAT_FIXER_TEST_CLANG_RESOURCE_DIR}\""
      "PATH_FORMAT_FIXER_TEST_CLANG_LIBCXX_DIR=\"${_clang_libcxx_dir}\""
    )
  endif()
  unset(_clang_bin_dir)
  unset(_clang_libcxx_dir)
endif()

# The wrapped test snippets `#include <fmt/core.h>`, but
# runToolOnCodeWithArgs()'s from-scratch invocation has no idea where Conan put
# fmt's headers (they're not on any default search path). Tell it explicitly.
list(GET fmt_INCLUDE_DIRS 0 _fmt_include_dir)
target_compile_definitions(path_format_fixer_test PRIVATE
  "PATH_FORMAT_FIXER_TEST_FMT_INCLUDE_DIR=\"${_fmt_include_dir}\""
)
unset(_fmt_include_dir)

gtest_discover_tests(path_format_fixer_test)

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
