cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Do not enable compiler specific extensions, for eg on GCC use -std=c++1z (=c++17) and not -std=gnu++17
set(CMAKE_CXX_EXTENSIONS OFF)

project(EnergyPlus-clang-tools
  VERSION 0.0.1
  LANGUAGES CXX C
)

###############################################################################
#                         C M A K E    C O N T R O L                          #
###############################################################################

# High level project configuration
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/Products")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/Products")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/Products")

set(LIBRARY_SEARCH_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/Release" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/Debug")

find_package(Clang REQUIRED CONFIG)
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")

# Link the combined clang-cpp shared lib rather than the individual static clangXXX
# archives: on LLVM 23.1.0 (homebrew), libclangDriver.a is missing a definition
# (clang::driver::tools::AMDGPU::setArchNameInTriple, referenced from ToolChain.cpp.o in
# that same archive) that IS present in libclang-cpp.dylib -- a real gap between
# Homebrew's static and shared library outputs for this release, not something fixable
# from here. libclang-cpp.dylib is also what the real clang++/clang-tidy binaries link
# against, so it's the more representative thing to build tools against anyway.
set(CLANG_TOOLING_LIBS clang-cpp LLVM)

add_library(llvm_clang_includes INTERFACE)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(llvm_clang_includes INTERFACE -fno-rtti)
endif()
# Note: deliberately NOT target_include_directories()/-I/-isystem here.
# /usr/lib/llvm-20/include ships its own cxxabi.h (meant to pair with
# libc++abi); when it's found ahead of libstdc++'s, any TU that ends up
# pulling in both (e.g. gtest's <gtest/internal/gtest-type-util.h> includes
# <cxxabi.h> for demangling, while libstdc++'s <exception> drags in
# bits/cxxabi_init_exception.h) fails with conflicting declarations of
# __cxa_init_primary_exception. -idirafter searches these dirs only after
# the compiler's builtin system include paths, so libstdc++'s cxxabi.h is
# found first while llvm/... and clang/... headers still resolve correctly.
foreach(dir IN LISTS LLVM_INCLUDE_DIRS CLANG_INCLUDE_DIRS)
  target_compile_options(llvm_clang_includes INTERFACE -idirafter${dir})
endforeach()

add_library(path_format_fixer_lib STATIC
  src/path_format_fixer/PathFormatFixer.cpp
  src/path_format_fixer/PathFormatFixer.hpp
)
target_include_directories(path_format_fixer_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(path_format_fixer_lib PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(path_format_fixer_lib
  PRIVATE
    ${CLANG_TOOLING_LIBS}
    llvm_clang_includes
)

add_executable(path_format_fixer src/path_format_fixer/path_format_fixer.cpp)
target_compile_definitions(path_format_fixer PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(path_format_fixer PRIVATE path_format_fixer_lib llvm_clang_includes)

add_library(recurring_static_message_check_lib STATIC
  src/recurring_static_message_check/RecurringStaticMessageCheck.cpp
  src/recurring_static_message_check/RecurringStaticMessageCheck.hpp
)
target_include_directories(recurring_static_message_check_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(recurring_static_message_check_lib PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(recurring_static_message_check_lib
  PRIVATE
    ${CLANG_TOOLING_LIBS}
    llvm_clang_includes
)

add_executable(recurring_static_message_check src/recurring_static_message_check/recurring_static_message_check.cpp)
target_compile_definitions(recurring_static_message_check PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(recurring_static_message_check PRIVATE recurring_static_message_check_lib llvm_clang_includes)

add_library(mixed_indexing_check_lib STATIC
  src/mixed_indexing_check/MixedIndexingCheck.cpp
  src/mixed_indexing_check/MixedIndexingCheck.hpp
)
target_include_directories(mixed_indexing_check_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Also statically linked into EnergyPlusTidyModule (a MODULE/shared-object target below),
# which requires every object going into it to be position-independent -- without this,
# linking that .so fails with "can not be used when making a shared object; recompile with
# -fPIC" on this lib's relocations.
set_target_properties(mixed_indexing_check_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(mixed_indexing_check_lib PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(mixed_indexing_check_lib
  PRIVATE
    ${CLANG_TOOLING_LIBS}
    llvm_clang_includes
)

add_executable(mixed_indexing_check src/mixed_indexing_check/mixed_indexing_check.cpp)
target_compile_definitions(mixed_indexing_check PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(mixed_indexing_check PRIVATE mixed_indexing_check_lib llvm_clang_includes)

# find_unused_members: a shared/reviewed dead-data-member finder using
# AllTUsToolExecutor (real per-TU-thread-pool parallelism, unlike
# dead_member_check's single-threaded ClangTool loop above). Single-file, no
# _lib split like the others -- nothing else links against its internals.
add_executable(find_unused_members src/find_unused_members/FindUnusedMembers.cpp)
target_compile_definitions(find_unused_members PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(find_unused_members PRIVATE ${CLANG_TOOLING_LIBS} llvm_clang_includes)

# See test/CMakeLists.txt's identical PATH_FORMAT_FIXER_TEST_CLANG_RESOURCE_DIR: ClangTool
# invokes the frontend library directly (not the `clang` driver binary), so it doesn't
# infer a resource-dir (builtin headers like stdarg.h) from this executable's own install
# location the way the driver would -- ask the actual compiler for its resource dir.
execute_process(
  COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
  OUTPUT_VARIABLE TOOLING_CLANG_RESOURCE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
foreach(tgt IN ITEMS recurring_static_message_check mixed_indexing_check)
  target_compile_definitions(${tgt} PRIVATE "CLANG_RESOURCE_DIR=\"${TOOLING_CLANG_RESOURCE_DIR}\"")
endforeach()
unset(TOOLING_CLANG_RESOURCE_DIR)

# A clang-tidy plugin: `clang-tidy --load=<this>.dylib -checks='-*,energyplus-*' ...`.
# See EnergyPlusTidyModule.cpp's header comment for why this deliberately does NOT link
# the clang/clangTidy static libs (only their headers, via llvm_clang_includes) -- it's
# dlopen()'d into a clang-tidy process that already has all of those linked in.
# mixed_indexing_check_lib IS linked in (unlike the clang/clangTidy libs): it's our own
# code with no ManagedStatic/cl::opt/registry globals to duplicate.
add_library(EnergyPlusTidyModule MODULE src/tidy_module/EnergyPlusTidyModule.cpp)
target_compile_definitions(EnergyPlusTidyModule PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(EnergyPlusTidyModule PRIVATE mixed_indexing_check_lib llvm_clang_includes)
set_target_properties(EnergyPlusTidyModule PROPERTIES PREFIX "")
if(APPLE)
  target_link_options(EnergyPlusTidyModule PRIVATE -undefined dynamic_lookup)
endif()

option(BUILD_TESTING "Build testing targets" OFF)
if(BUILD_TESTING)
  # Conan drops its generated Find/*Config.cmake modules in the build dir;
  # make sure they're found ahead of CMake's bundled FindGTest.cmake.
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_BINARY_DIR}")
  list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}")

  find_package(fmt REQUIRED)
  find_package(GTest REQUIRED)

  enable_testing()
  include(GoogleTest)

  add_subdirectory(test)
endif()

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

# eg 'Linux-Ubuntu26.04-x86_64' instead of CPack's bare default 'Linux'. lsb_release
# isn't guaranteed to be installed everywhere, so fall back to just OS-arch if missing.
set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(UNIX AND NOT APPLE)
  find_program(LSB_RELEASE_EXEC lsb_release)
  if(LSB_RELEASE_EXEC)
    execute_process(COMMAND "${LSB_RELEASE_EXEC}" -is OUTPUT_VARIABLE LSB_RELEASE_ID OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${LSB_RELEASE_EXEC}" -rs OUTPUT_VARIABLE LSB_RELEASE_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${LSB_RELEASE_ID}${LSB_RELEASE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endif()

message(STATUS "Installer name is set to '${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}'")

set(LIB_DESTINATION_DIR "lib")
if(WIN32)
  # No Rpaths on Windows
  set(LIB_DESTINATION_DIR "bin")
endif()

install(TARGETS
  path_format_fixer
  recurring_static_message_check
  mixed_indexing_check
  find_unused_members
  EnergyPlusTidyModule
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION ${LIB_DESTINATION_DIR}
)

include(CPack)
