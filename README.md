# EnergyPlus-fmt-clang-refactoring

Small Clang LibTooling-based refactoring tools that either check or rewrites EnergyPlus C++ Source code.

## path_format_fixer

It finds calls to EnergyPlus::format/fmt::format/std::format that pass a std::filesystem::path::string()/generic_string() argument, drops the redundant .string()/.generic_string() call (inserting a `{:g}` format spec when needed for generic_string), and normalizes the call itself onto std::format

There is a gtest suite covering the AST-matching and rewrite logic.

## Building


### Without tests

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_COMPILER=clang-20 -DBUILD_TESTING:BOOL=OFF
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
  -s compiler.cppstd=20 -s build_type=Release
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
