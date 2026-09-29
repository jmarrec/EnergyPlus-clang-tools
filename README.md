# EnergyPlus-fmt-clang-refactoring

Small Clang LibTooling-based refactoring tools that either check or rewrites EnergyPlus C++ Source code.

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

On MacOS:

```
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING:BOOL=OFF \
  -DCPACK_BINARY_TGZ:BOOL=ON -DCPACK_BINARY_STGZ:BOOL=OFF -DCPACK_BINARY_TZ:BOOL=OFF \
  -DCMAKE_PREFIX_PATH:PATH=/opt/homebrew/opt/llvm \
  -DCMAKE_C_COMPILER:PATH=/opt/homebrew/opt/llvm/bin/clang-23 \
  -DCMAKE_CXX_COMPILER:PATH=/opt/homebrew/opt/llvm/bin/clang++ \
  ..
```

### With tests

`googletest` is fetched automatically via CMake's `FetchContent` when `BUILD_TESTING:BOOL=ON` -- no separate dependency install step needed.

## Releasing

The version lives in `CMakeLists.txt` and is bumped with [bump2version](https://github.com/c4urself/bump2version) (config in `.bumpversion.cfg`), which also commits and tags:

```bash
bump2version patch  # or minor / major
git push --follow-tags
```
