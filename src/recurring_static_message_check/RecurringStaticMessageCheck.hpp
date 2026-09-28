#pragma once

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

#include <vector>

namespace recurring_static_message_check {

// Builds the AST matcher that finds calls to any of `RecurringFunctionNames`
// (eg ShowRecurringWarningErrorAtEnd) whose message argument (parameter index 1,
// after `state`) is built exclusively from string literals -- no identifier
// (variable/member) reference anywhere in it, however that reference could have
// gotten in (`+` concatenation, a std::format()/fmt::format() argument, etc).
clang::ast_matchers::StatementMatcher
makeMatcher(const std::vector<std::string> &RecurringFunctionNames);

class Callback : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  void run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

  bool foundAny() const { return FoundAny; }

private:
  bool FoundAny = false;
};

} // namespace recurring_static_message_check
