#include "cross_tu_checks/UnusedMemberCheck.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::tooling;
using cross_tu_checks::CrossTuCheck;
using cross_tu_checks::Finding;
using cross_tu_checks::UnusedMemberCheck;

namespace {

// Virtual (in-memory) files live under here; only fields declared below
// sourceRoot are considered, just like --source-root on a real build.
constexpr auto *sourceRoot = "/fixture/src";

// Hands each TU's AST to the check, as cross_tu_checks' CrossTuAction does.
class CheckAction final : public ASTFrontendAction {
public:
  explicit CheckAction(CrossTuCheck &check) : check_(check) {}

  std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &compiler,
                    llvm::StringRef /*inFile*/) override {
    return check_.createConsumer(compiler);
  }

private:
  CrossTuCheck &check_;
};

struct TranslationUnit {
  std::string path;
  std::string code;
};

// Runs every TU through a single UnusedMemberCheck (headers are shared by all
// TUs) and returns the sorted qualified names of the members it reports.
std::vector<std::string>
unusedMembers(const std::vector<TranslationUnit> &tus,
              const FileContentMappings &headers = {}) {
  UnusedMemberCheck check{sourceRoot};
  for (const TranslationUnit &tu : tus) {
    EXPECT_TRUE(runToolOnCodeWithArgs(
        std::make_unique<CheckAction>(check), tu.code, {"-std=c++20"}, tu.path,
        "unused-member-test", std::make_shared<PCHContainerOperations>(),
        headers))
        << tu.path << " failed to compile";
  }

  std::vector<std::string> names;
  for (const Finding &finding : check.findings()) {
    names.push_back(finding.field.qualifiedName);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> unusedMembers(const std::string &code) {
  return unusedMembers({{"/fixture/src/main.cc", code}});
}

using Names = std::vector<std::string>;

} // namespace

TEST(UnusedMemberCheck, FlagsMemberNeverReferenced) {
  EXPECT_EQ(unusedMembers(R"cpp(
struct Foo {
  int used = 0;
  int unused;
};
int get(const Foo &foo) { return foo.used; }
)cpp"),
            Names{"Foo::unused"});
}

TEST(UnusedMemberCheck, InClassInitializerIsNotAReference) {
  EXPECT_EQ(unusedMembers(R"cpp(
struct Foo {
  double value = 0.0;
};
)cpp"),
            Names{"Foo::value"});
}

TEST(UnusedMemberCheck, AWriteIsAReference) {
  EXPECT_TRUE(unusedMembers(R"cpp(
struct Foo {
  int value;
};
void set(Foo &foo) { foo.value = 1; }
)cpp")
                  .empty());
}

TEST(UnusedMemberCheck, ConstructorInitializerIsAReference) {
  EXPECT_TRUE(unusedMembers(R"cpp(
struct Foo {
  Foo() : value(1) {}
  int value;
};
)cpp")
                  .empty());
}

TEST(UnusedMemberCheck, AggregateInitReferencesEveryMember) {
  EXPECT_TRUE(unusedMembers(R"cpp(
struct Foo {
  int a;
  int b;
};
Foo make() { return Foo{1, 2}; }
)cpp")
                  .empty());
}

TEST(UnusedMemberCheck, DesignatedInitReferencesEveryMember) {
  // TODO: this should probably NOT do that, but I don't think I care enough
  // A designated initializer is still an InitListExpr of type Foo, so the
  // aggregate rule marks every member used, not just the designated ones.
  EXPECT_TRUE(unusedMembers(R"cpp(
struct Foo {
  int a;
  int b;
};
Foo make() { return Foo{.a = 1}; }
)cpp")
                  .empty());
}

TEST(UnusedMemberCheck, StructuredBindingReferencesEveryMember) {
  EXPECT_TRUE(unusedMembers(R"cpp(
struct Foo {
  int a;
  int b;
};
int sum(const Foo &foo) {
  auto [a, b] = foo;
  return a + b;
}
)cpp")
                  .empty());
}

TEST(UnusedMemberCheck, OffsetofIsAReference) {
  EXPECT_TRUE(unusedMembers(R"cpp(
struct Foo {
  int a;
};
unsigned long offset() { return __builtin_offsetof(Foo, a); }
)cpp")
                  .empty());
}

TEST(UnusedMemberCheck, AnonymousUnionMemberIsReferencedThroughParent) {
  EXPECT_EQ(unusedMembers(R"cpp(
struct Foo {
  union {
    int asInt;
    float asFloat;
  };
};
int get(const Foo &foo) { return foo.asInt; }
)cpp"),
            Names{"Foo::(anonymous union)::asFloat"});
}

TEST(UnusedMemberCheck, DependentAccessSuppressesByName) {
  // t.value can't be resolved in an uninstantiated template, so any member
  // named "value" is conservatively treated as used.
  EXPECT_EQ(unusedMembers(R"cpp(
struct Foo {
  int value;
  int other;
};
template <typename T> int get(const T &t) { return t.value; }
)cpp"),
            Names{"Foo::other"});
}

TEST(UnusedMemberCheck, IgnoresMembersOutsideSourceRoot) {
  EXPECT_EQ(unusedMembers({{"/fixture/src/main.cc", R"cpp(
#include "../third_party/External.hh"
struct Foo {
  int unused;
};
)cpp"}},
                          {{"/fixture/third_party/External.hh", R"cpp(
#pragma once
struct External {
  int unused;
};
)cpp"}}),
            Names{"Foo::unused"});
}

TEST(UnusedMemberCheck, ReferenceInAnotherTUCounts) {
  const FileContentMappings headers = {{"/fixture/src/Foo.hh", R"cpp(
#pragma once
struct Foo {
  int a;
  int b;
};
)cpp"}};

  const TranslationUnit useA{.path = "/fixture/src/a.cc", .code = R"cpp(
#include "Foo.hh"
int getA(const Foo &foo) { return foo.a; }
)cpp"};
  const TranslationUnit useB{.path = "/fixture/src/b.cc", .code = R"cpp(
#include "Foo.hh"
int getB(const Foo &foo) { return foo.b; }
)cpp"};

  // Each TU alone leaves the other member unreferenced...
  EXPECT_EQ(unusedMembers({useA}, headers), Names{"Foo::b"});
  EXPECT_EQ(unusedMembers({useB}, headers), Names{"Foo::a"});
  // ...but together they reference both: the field is matched across TUs.
  EXPECT_TRUE(unusedMembers({useA, useB}, headers).empty());
}
