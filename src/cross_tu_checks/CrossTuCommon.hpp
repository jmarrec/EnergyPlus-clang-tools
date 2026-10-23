#pragma once

#include "clang/Tooling/CompilationDatabase.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace clang {
class Decl;
class FieldDecl;
class SourceManager;
} // namespace clang

namespace cross_tu_checks {

namespace fs = std::filesystem;

fs::path normalized(const fs::path &path, const fs::path &base = {});

// Explicit relative paths are resolved from the working directory first, and
// from the repository root when that doesn't exist (so `src/EnergyPlus` works
// from either place).
fs::path normalizedArgument(const fs::path &path,
                            const fs::path &repositoryRoot);

bool isWithin(const fs::path &path, const fs::path &root);

// USR of the canonical declaration, used to match a field across TUs. Empty
// if Clang can't generate one.
std::string usrFor(const clang::Decl *declaration);

struct FieldInfo {
  std::string usr;
  std::string name;
  std::string qualifiedName;
  std::string type;
  fs::path file;
  unsigned line = 0;
  unsigned column = 0;
  unsigned fileOffset = 0;
};

// The reportable description of a field, or nullopt if it's implicit, declared
// outside sourceRoot, or has no USR.
std::optional<FieldInfo>
describeField(const clang::FieldDecl *field,
              const clang::SourceManager &sourceManager,
              const fs::path &sourceRoot);

// Orders by file, then line, then column.
bool locationLess(const FieldInfo &left, const FieldInfo &right);

void sortByLocation(std::vector<FieldInfo> &fields);

class FilteredCompilationDatabase final
    : public clang::tooling::CompilationDatabase {
public:
  // Keeps only the commands for TUs under one of scanRoots. With keepPch false,
  // the CMake precompiled header is stripped so every TU parses its headers
  // itself: needed when the PCH was built by a different Clang than the one
  // this tool links against (a PCH only loads in the version that wrote it).
  FilteredCompilationDatabase(
      const clang::tooling::CompilationDatabase &database,
      const std::vector<fs::path> &scanRoots, bool keepPch);

  std::vector<clang::tooling::CompileCommand>
  getCompileCommands(llvm::StringRef filePath) const override;

  std::vector<std::string> getAllFiles() const override;

  std::vector<clang::tooling::CompileCommand>
  getAllCompileCommands() const override;

  std::size_t size() const { return commands_.size(); }

private:
  std::vector<clang::tooling::CompileCommand> commands_;
  std::unordered_multimap<std::string, clang::tooling::CompileCommand>
      commandsByFile_;
};

} // namespace cross_tu_checks
