#include "cross_tu_checks/CrossTuCommon.hpp"

#include "clang/AST/Decl.h"
#include "clang/Basic/SourceManager.h"
// Moved out of clang/Index in LLVM 23 (same clang::index namespace).
#if __has_include("clang/UnifiedSymbolResolution/USRGeneration.h")
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#else
#include "clang/Index/USRGeneration.h"
#endif
#include "llvm/ADT/SmallString.h"

#include <algorithm>
#include <iterator>
#include <string_view>

namespace cross_tu_checks {

fs::path normalized(const fs::path &path, const fs::path &base) {
  std::error_code error;
  fs::path absolutePath = path;
  if (absolutePath.is_relative()) {
    absolutePath =
        (base.empty() ? fs::current_path(error) : base) / absolutePath;
  }
  auto canonicalPath = fs::weakly_canonical(absolutePath, error);
  return error ? absolutePath.lexically_normal() : canonicalPath;
}

fs::path normalizedArgument(const fs::path &path,
                            const fs::path &repositoryRoot) {
  if (path.is_absolute()) {
    return normalized(path);
  }

  const fs::path workingDirectoryPath = normalized(path);
  std::error_code error;
  if (fs::exists(workingDirectoryPath, error)) {
    return workingDirectoryPath;
  }
  return normalized(path, repositoryRoot);
}

bool isWithin(const fs::path &path, const fs::path &root) {
  auto pathIt = path.begin();
  auto rootIt = root.begin();
  for (; rootIt != root.end(); ++rootIt, ++pathIt) {
    if (pathIt == path.end() || *pathIt != *rootIt) {
      return false;
    }
  }
  return true;
}

std::string usrFor(const clang::Decl *declaration) {
  llvm::SmallString<256> buffer;
  if (!declaration || clang::index::generateUSRForDecl(
                          declaration->getCanonicalDecl(), buffer)) {
    return {};
  }
  return std::string(buffer);
}

std::optional<FieldInfo>
describeField(const clang::FieldDecl *field,
              const clang::SourceManager &sourceManager,
              const fs::path &sourceRoot) {
  if (field->isImplicit()) {
    return std::nullopt;
  }

  const clang::SourceLocation location =
      sourceManager.getExpansionLoc(field->getLocation());
  if (location.isInvalid()) {
    return std::nullopt;
  }
  const llvm::StringRef filename = sourceManager.getFilename(location);
  if (filename.empty()) {
    return std::nullopt;
  }
  const fs::path file = normalized(filename.str());
  if (!isWithin(file, sourceRoot)) {
    return std::nullopt;
  }

  std::string usr = usrFor(field);
  if (usr.empty()) {
    return std::nullopt;
  }

  const clang::PresumedLoc presumed = sourceManager.getPresumedLoc(location);
  return FieldInfo{std::move(usr),
                   field->getNameAsString(),
                   field->getQualifiedNameAsString(),
                   field->getType().getAsString(),
                   file,
                   presumed.isValid() ? presumed.getLine() : 0,
                   presumed.isValid() ? presumed.getColumn() : 0,
                   sourceManager.getFileOffset(location)};
}

bool locationLess(const FieldInfo &left, const FieldInfo &right) {
  if (left.file != right.file) {
    return left.file < right.file;
  }
  if (left.line != right.line) {
    return left.line < right.line;
  }
  return left.column < right.column;
}

void sortByLocation(std::vector<FieldInfo> &fields) {
  std::sort(fields.begin(), fields.end(), locationLess);
}

namespace {

bool isPchPath(std::string_view argument) {
  return argument.find("cmake_pch") != std::string_view::npos;
}

// CMake's precompiled header comes in as GCC's `-include <cmake_pch.hxx>`, or
// for Clang as `-Xclang -include-pch -Xclang <.pch>` plus
// `-Xclang -include -Xclang <cmake_pch.hxx>`.
void stripPch(std::vector<std::string> &arguments) {
  for (auto it = arguments.begin(); it != arguments.end();) {
    const auto remaining = std::distance(it, arguments.end());
    if (*it == "-Winvalid-pch") {
      it = arguments.erase(it);
    } else if (remaining >= 4 && *it == "-Xclang" &&
               (it[1] == "-include-pch" || it[1] == "-include") &&
               it[2] == "-Xclang" && isPchPath(it[3])) {
      it = arguments.erase(it, it + 4);
    } else if (remaining >= 2 && (*it == "-include" || *it == "-include-pch") &&
               isPchPath(it[1])) {
      it = arguments.erase(it, it + 2);
    } else {
      ++it;
    }
  }
}

} // namespace

FilteredCompilationDatabase::FilteredCompilationDatabase(
    const clang::tooling::CompilationDatabase &database,
    const std::vector<fs::path> &scanRoots, bool keepPch) {
  for (const auto &command : database.getAllCompileCommands()) {
    const fs::path file = normalized(command.Filename, command.Directory);
    if (!std::any_of(
            scanRoots.begin(), scanRoots.end(),
            [&](const fs::path &root) { return isWithin(file, root); })) {
      continue;
    }
    // CMake's PCH support adds a synthetic TU that compiles cmake_pch.hxx
    // itself (to generate the .pch/.gch); it has no real declarations to
    // report and would just waste a parse.
    if (file.filename().string().rfind("cmake_pch", 0) == 0) {
      continue;
    }

    auto clangCommand = command;
    auto &arguments = clangCommand.CommandLine;
    // A compile database generated for GCC pairs -Werror with GCC-only -Wno-*
    // flags Clang doesn't recognize (e.g. -Wno-dangling-reference), turning
    // "unknown warning option" into a hard error; the same -Werror turns
    // "precompiled header ...cmake_pch.hxx.gch was ignored because it is not a
    // clang PCH file" into one too. Dropping it downgrades both back to
    // warnings -- the GCC PCH then just gets ignored and cmake_pch.hxx reparsed
    // as a normal header, slower but correct.
    arguments.erase(std::remove(arguments.begin(), arguments.end(), "-Werror"),
                    arguments.end());
    if (!keepPch) {
      stripPch(arguments);
    }
    commands_.push_back(clangCommand);
    commandsByFile_.emplace(file.string(), std::move(clangCommand));
  }
}

std::vector<clang::tooling::CompileCommand>
FilteredCompilationDatabase::getCompileCommands(
    llvm::StringRef filePath) const {
  std::vector<clang::tooling::CompileCommand> result;
  const std::string key = normalized(filePath.str()).string();
  const auto range = commandsByFile_.equal_range(key);
  for (auto it = range.first; it != range.second; ++it) {
    result.push_back(it->second);
  }
  return result;
}

std::vector<std::string> FilteredCompilationDatabase::getAllFiles() const {
  std::vector<std::string> files;
  files.reserve(commands_.size());
  for (const auto &command : commands_) {
    files.push_back(normalized(command.Filename, command.Directory).string());
  }
  return files;
}

std::vector<clang::tooling::CompileCommand>
FilteredCompilationDatabase::getAllCompileCommands() const {
  return commands_;
}

} // namespace cross_tu_checks
