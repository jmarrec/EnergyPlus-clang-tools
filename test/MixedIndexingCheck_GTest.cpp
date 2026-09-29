#include "mixed_indexing_check/MixedIndexingCheck.hpp"

#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

namespace {

// Wraps a snippet of `demo()` body code with the ObjexxFCL-ish/std container
// aliases the check's operator()-vs-operator[] distinction cares about.
std::string wrap(llvm::StringRef Body) {
  return (R"cpp(
#include <vector>

struct Array1D {
  int &operator()(int i) { static int dummy; return dummy; }
};

void demo(std::vector<int> &v, Array1D &a) {
)cpp" + Body.str() +
          "}\n");
}

// Parses `Code`, finds the `demo` FunctionDecl, and returns the names
// findFlaggedVars() reports for it.
std::vector<std::string> flaggedVars(
    llvm::StringRef Code,
    const std::vector<mixed_indexing_check::MemberExclusion> &Exclusions =
        mixed_indexing_check::defaultExclusions()) {
  std::unique_ptr<ASTUnit> Unit =
      buildASTFromCodeWithArgs(Code, {"-std=c++20"});
  if (!Unit) {
    return {};
  }

  auto Matches =
      match(functionDecl(hasName("demo")).bind("func"), Unit->getASTContext());
  if (Matches.empty()) {
    return {};
  }
  const auto *Func = Matches.front().getNodeAs<FunctionDecl>("func");

  std::vector<std::string> Names;
  for (const mixed_indexing_check::FlaggedVar &FV :
       mixed_indexing_check::findFlaggedVars(*Func, Exclusions)) {
    Names.push_back(FV.Name);
  }
  std::sort(Names.begin(), Names.end());
  return Names;
}

} // namespace

TEST(MixedIndexingCheck, FlagsVariableUsedBothWays) {
  EXPECT_EQ(flaggedVars(wrap(R"(  int i = 0;
  a(i);
  (void)v[i];
)")),
            std::vector<std::string>{"i"});
}

TEST(MixedIndexingCheck, IgnoresVariableUsedOnlyAsSubscript) {
  EXPECT_TRUE(flaggedVars(wrap(R"(  int i = 0;
  (void)v[i];
)"))
                  .empty());
}

TEST(MixedIndexingCheck, IgnoresVariableUsedOnlyAsCall) {
  EXPECT_TRUE(flaggedVars(wrap(R"(  int i = 0;
  a(i);
)"))
                  .empty());
}

TEST(MixedIndexingCheck, IgnoresAdditiveAdjustedIndex) {
  // `i + 1`/`i - 1` is a deliberate convention-mismatch adjustment, not
  // co-mingled use.
  EXPECT_TRUE(flaggedVars(wrap(R"(  int i = 0;
  a(i + 1);
  (void)v[i];
)"))
                  .empty());
}

TEST(MixedIndexingCheck, IgnoresAssociativeContainerSubscript) {
  EXPECT_TRUE(flaggedVars(R"cpp(
#include <map>
struct Array1D {
  int &operator()(int i) { static int dummy; return dummy; }
};
void demo(std::map<int, int> &m, Array1D &a) {
  int i = 0;
  a(i);
  (void)m[i];
}
)cpp")
                  .empty());
}

TEST(MixedIndexingCheck, RespectsMemberExclusion) {
  std::string Code = R"cpp(
#include <vector>
struct Array1D {
  int &operator()(int i) { static int dummy; return dummy; }
};
struct Widget {
  std::vector<int> compPointer;
  Array1D a;
};
void demo(Widget &w) {
  int i = 0;
  w.a(i);
  (void)w.compPointer[i];
}
)cpp";

  // Default exclusions don't cover Widget::compPointer -> flagged.
  EXPECT_EQ(flaggedVars(Code), std::vector<std::string>{"i"});

  // Excluding it drops the subscript use entirely -> no longer flagged.
  EXPECT_TRUE(flaggedVars(Code, {{"Widget", "compPointer"}}).empty());
}
