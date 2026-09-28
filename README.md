# EnergyPlus-fmt-clang-refactoring

Small Clang LibTooling-based refactoring tools that either check or rewrites EnergyPlus C++ Source code.

## [path_format_fixer](src/path_format_fixer)

It finds calls to EnergyPlus::format/fmt::format/std::format that pass a std::filesystem::path::string()/generic_string() argument, drops the redundant .string()/.generic_string() call (inserting a `{:g}` format spec when needed for generic_string), and normalizes the call itself onto std::format

There is a gtest suite covering the AST-matching and rewrite logic.

## [recurring_static_message_check](src/recurring_static_message_check)

Flags calls to the recurring-error functions (eg `ShowRecurringWarningErrorAtEnd`) whose message argument is built exclusively from string literals, with no identifier (variable/member) reference anywhere in it -- however that reference could have snuck in (`+` concatenation, a `std::format`/`fmt::format` argument, etc). The check function names to look for are passed on the command line and may be repeated.

Also shipped as a clang-tidy check, `energyplus-recurring-static-message`, via [EnergyPlusTidyModule](src/tidy_module).

## [mixed_indexing_check](src/mixed_indexing_check)

Finds variables/members that get indexed both 0-based and 1-based within the same scope -- a common source of off-by-one bugs in EnergyPlus's mix of legacy 1-indexed and modern 0-indexed containers. Deliberately 1-indexed members (eg a `std::vector` resized to `N + 1` so `operator[]` can be used with a 1-based index) can be excluded via `--exclude-member Type::member`.

Also shipped as a clang-tidy check, `energyplus-mixed-indexing`, via [EnergyPlusTidyModule](src/tidy_module).

## [find_unused_members](src/find_unused_members)

A dead-data-member finder built on `AllTUsToolExecutor` for real per-TU thread-pool parallelism (unlike the single-threaded `ClangTool` loop the other tools above use).

## [EnergyPlusTidyModule](src/tidy_module)

A clang-tidy plugin module (`clang-tidy --load=<this>.so -checks='-*,energyplus-*' ...`) bundling the `energyplus-recurring-static-message` and `energyplus-mixed-indexing` checks, reusing the AST-matching logic from `recurring_static_message_check` and `mixed_indexing_check` above.

## Building


### Without tests

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_COMPILER=clang-20 \
  -DBUILD_TESTING:BOOL=OFF \
  -DCPACK_BINARY_TGZ:BOOL=ON -DCPACK_BINARY_STGZ:BOOL=OFF -DCPACK_BINARY_TZ:BOOL=OFF
```

### With tests

Using **conan >= 2.0**:

```bash
pip install conan
```

Install conan dependencies and create toolchain file. Needed only to have `gtest` and `fmt` for the tests.

```bash
cat ~/.conan2/profiles/clang
```

```
[settings]
arch=x86_64
build_type=Release
compiler=clang
compiler.cppstd=20
compiler.libcxx=libc++
compiler.version=20
os=Linux
```


```shell
export CC=/usr/bin/clang-20
export CXX=/usr/bin/clang++-20
conan install . --output-folder=./build --build=missing -c tools.cmake.cmaketoolchain:generator=Ninja \
  -s compiler.cppstd=20 -s build_type=Release \
  --profile:all clang \
  -c tools.build:cxxflags="['-Wno-deprecated-literal-operator', '-DFMT_CONSTEVAL=']"
```

Build using conan-presets

```shell
cmake --preset conan-release -DBUILD_TESTING:BOOL=ON
cmake --build --preset conan-release
```

On macoS with homebrew clang 21, the profile I used was

```bash
$ cat ~/.conan2/profiles/llvm-21
```

```
{% set clang_v = 21 %}
{% set llvm_prefix = "/opt/homebrew/opt/llvm@" ~ clang_v %}
{% set clang = llvm_prefix ~ "/bin/clang" %}

[settings]
arch=armv8
build_type=Release
compiler=clang
compiler.cppstd=26
compiler.libcxx=libc++
compiler.version={{ clang_v }}
os=Macos

[buildenv]
CC={{ clang }}
CXX={{ clang + '++' }}
CPPFLAGS=-I-{{ llvm_prefix }}/include
LDFLAGS=-L{{ llvm_prefix }}/lib

[conf]
tools.build:compiler_executables={'c': '{{ clang }}', 'cpp': '{{ clang + '++' }}' }
tools.cmake.cmaketoolchain:extra_variables={'Clang_DIR': '{{ llvm_prefix }}/lib/cmake/clang', 'LLVM_DIR': '{{ llvm_prefix }}/lib/cmake/llvm'}
```
