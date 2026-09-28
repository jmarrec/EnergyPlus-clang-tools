#pragma once

#include "clang/AST/Decl.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceLocation.h"

#include <string>
#include <vector>

namespace mixed_indexing_check {

// A `Type::member` container access to leave out of the cross-referencing entirely --
// e.g. a std::vector deliberately resize(N + 1)'d so operator[] on it can be used with a
// 1-based index (index 0 left as an unused dummy slot), which is a real, deliberate
// EnergyPlus idiom, not a 0-indexed container. Matched against the FieldDecl's name and
// its immediately declaring RecordDecl's name (not any base class), so it only matches
// where the member is actually declared, not every derived type that inherits it.
struct MemberExclusion {
  std::string TypeName;
  std::string MemberName;
};

// Known deliberately-padded-for-1-based-operator[] members, shared by the standalone
// ClangTool driver (mixed_indexing_check.cpp, which layers `--exclude-member` on top) and
// the clang-tidy plugin (EnergyPlusTidyModule.cpp). Extend as more turn up.
std::vector<MemberExclusion> defaultExclusions();

// Builds the matcher that finds every function with a body -- the caller then walks each
// body itself (RecursiveASTVisitor, inside findFlaggedVars) rather than matching a single
// expression, since this check is inherently cross-referencing: it needs to see every
// index-position use of every variable in the function before it can tell whether any one
// variable was used both ways. That stateful aggregation isn't expressible as a single
// clang-query matcher.
clang::ast_matchers::DeclarationMatcher makeMatcher();

// One variable flagged in `Func`: `Name` (for the "%0 is used both..." message), where it's
// declared, and the first use of each kind (for the two "used here as..." notes).
struct FlaggedVar {
  std::string Name;
  clang::SourceLocation DeclLoc;
  clang::SourceLocation FirstCallLoc;
  clang::SourceLocation FirstSubscriptLoc;
};

// The pure computation, shared by both consumers below: Callback (the standalone ClangTool
// driver, which reports via the raw DiagnosticsEngine) and MixedIndexingTidyCheck (the
// clang-tidy plugin, EnergyPlusTidyModule.cpp, which must report via ClangTidyCheck::diag()
// instead -- clang-tidy's ClangTidyDiagnosticConsumer maps every diagnostic's ID back to a
// check name for --checks=/NOLINT filtering, and crashes on one that was never registered
// that way, which a raw DiagnosticsEngine::Report()/getCustomDiagID() bypasses).
std::vector<FlaggedVar> findFlaggedVars(const clang::FunctionDecl &Func, const std::vector<MemberExclusion> &Exclusions);

// Standalone-ClangTool-only: calls findFlaggedVars() and reports via the raw
// DiagnosticsEngine (safe there -- no ClangTidyDiagnosticConsumer in the way).
class Callback : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  explicit Callback(std::vector<MemberExclusion> Exclusions) : Exclusions(std::move(Exclusions)) {}

  void run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

private:
  std::vector<MemberExclusion> Exclusions;
};

} // namespace mixed_indexing_check
