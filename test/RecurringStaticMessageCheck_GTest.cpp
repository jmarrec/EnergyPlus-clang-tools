#include "recurring_static_message_check/RecurringStaticMessageCheck.hpp"

#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

namespace {

// Wraps a snippet of `demo()` body code, with a stub
// `ShowRecurringWarningErrorAtEnd` matching the real signature's shape closely
// enough for the matcher: a leading `state` argument, then the message at
// index 1.
std::string wrap(llvm::StringRef Body) {
  return (R"cpp(
#include <string>
struct EnergyPlusState {};
void ShowRecurringWarningErrorAtEnd(EnergyPlusState &state, const std::string &msg, int *idx) {}
void demo(EnergyPlusState &state, const std::string &name) {
)cpp" + Body.str() +
          "}\n");
}

// Runs recurring_static_message_check on in-memory `Code` and returns whether
// the check fired for any call.
bool checkTriggers(llvm::StringRef Code,
                   const std::vector<std::string> &RecurringFunctionNames = {
                       "ShowRecurringWarningErrorAtEnd"}) {
  recurring_static_message_check::Callback Callback;

  MatchFinder Finder;
  Finder.addMatcher(
      recurring_static_message_check::makeMatcher(RecurringFunctionNames),
      &Callback);

  std::vector<std::string> Args = {"-std=c++20"};
  runToolOnCodeWithArgs(newFrontendActionFactory(&Finder)->create(), Code, Args,
                        "input.cc");

  return Callback.foundAny();
}

} // namespace

TEST(RecurringStaticMessageCheck, FlagsPureStringLiteralMessage) {
  EXPECT_TRUE(checkTriggers(wrap(R"(  int idx = 0;
  ShowRecurringWarningErrorAtEnd(state, "Something went wrong", &idx);
)")));
}

TEST(RecurringStaticMessageCheck, FlagsConcatenatedStringLiterals) {
  EXPECT_TRUE(checkTriggers(wrap(R"(  int idx = 0;
  ShowRecurringWarningErrorAtEnd(state, std::string("Prefix: ") + "suffix", &idx);
)")));
}

TEST(RecurringStaticMessageCheck, IgnoresMessageWithIdentifierReference) {
  EXPECT_FALSE(checkTriggers(wrap(R"(  int idx = 0;
  ShowRecurringWarningErrorAtEnd(state, "Bad name: " + name, &idx);
)")));
}

TEST(RecurringStaticMessageCheck, IgnoresUnrelatedCalls) {
  EXPECT_FALSE(checkTriggers(wrap("  (void)name;\n")));
}

TEST(RecurringStaticMessageCheck, RespectsCustomFunctionNameList) {
  std::string Code = wrap(R"(  int idx = 0;
  ShowRecurringWarningErrorAtEnd(state, "Something went wrong", &idx);
)");

  // Default list matches `ShowRecurringWarningErrorAtEnd` -> triggers.
  EXPECT_TRUE(checkTriggers(Code));

  // A list that doesn't include it -> no match.
  EXPECT_FALSE(checkTriggers(Code, {"SomeOtherFunction"}));
}
