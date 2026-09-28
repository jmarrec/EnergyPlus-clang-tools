#pragma once

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/ADT/SmallPtrSet.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace path_format_fixer {

// A `{...}` replacement field found in a format string's source spelling.
struct Field {
  size_t Offset;     // byte offset of '{' within the literal's source spelling
  size_t Length;     // length including both braces
  std::string Inner; // text between '{' and '}'
  int ArgIndex;      // 0-based variadic-argument index this field refers to
};

// Scans `Text` (the raw source spelling of a format-string literal, including
// quotes) for `{...}` replacement fields, skipping `{{`/`}}` brace escapes.
// Nested replacement fields inside a format spec are not handled (such fields
// are returned with ArgIndex == -1, and parsing aborts with std::nullopt if
// they're malformed).
//
// Returns, for each field in order, the field text/location plus which
// (0-based) variadic argument it refers to: either the explicit leading index
// (`{2:...}`) or, for auto-numbered fields (`{}`/`{:...}`), the next index in
// sequence. Mixing the two styles is invalid std::format anyway, so treating
// them uniformly here is fine.
std::optional<std::vector<Field>> scanReplacementFields(llvm::StringRef Text);

// Builds the AST matcher that finds calls to any of `FormatFunctionNames`
// (e.g. `EnergyPlus::format`, `std::format`, `fmt::format`) passing a
// `std::filesystem::path::string()` / `generic_string()` call as one of the
// variadic arguments.
clang::ast_matchers::StatementMatcher
makeMatcher(const std::vector<std::string> &FormatFunctionNames);

class PathFormatCallback : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  explicit PathFormatCallback(std::map<std::string, clang::tooling::Replacements> &FileToReplaces)
      : FileToReplaces(FileToReplaces) {}

  void run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

private:
  void diag(const clang::SourceManager &SM, clang::SourceLocation Loc, const std::string &Msg);
  void addReplacement(const clang::SourceManager &SM, clang::CharSourceRange Range,
                      llvm::StringRef NewText);

  std::map<std::string, clang::tooling::Replacements> &FileToReplaces;
  // Calls already rewritten onto `std::format`, so multiple matches against
  // the same call (one per rewritten path argument) don't add the same
  // callee replacement twice.
  llvm::SmallPtrSet<const clang::CallExpr *, 8> RenamedCalls;
};

} // namespace path_format_fixer
