#include "cross_tu_checks/UnusedMemberCheck.hpp"

#include "CrossTuCheckFixture.hpp"

#include <gtest/gtest.h>

using namespace cross_tu_checks_test;
using cross_tu_checks::UnusedMemberCheck;

using UnusedMemberCheckTest = CrossTuCheckTest<UnusedMemberCheck>;

TEST_F(UnusedMemberCheckTest, FlagsMemberNeverReferenced) {
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  int used = 0;
  int unused;
};
int get(const Foo &foo) { return foo.used; }
)cpp"),
            Names{"Foo::unused"});
}

TEST_F(UnusedMemberCheckTest, InClassInitializerIsNotAReference) {
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  double value = 0.0;
};
)cpp"),
            Names{"Foo::value"});
}

TEST_F(UnusedMemberCheckTest, AWriteIsAReference) {
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int value;
};
void set(Foo &foo) { foo.value = 1; }
)cpp")
                  .empty());
}

TEST_F(UnusedMemberCheckTest, ConstructorInitializerIsAReference) {
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  Foo() : value(1) {}
  int value;
};
)cpp")
                  .empty());
}

TEST_F(UnusedMemberCheckTest, AggregateInitReferencesEveryMember) {
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int a;
  int b;
};
Foo make() { return Foo{1, 2}; }
)cpp")
                  .empty());
}

TEST_F(UnusedMemberCheckTest, DesignatedInitReferencesEveryMember) {
  // TODO: this should probably NOT do that, but I don't think I care enough
  // A designated initializer is still an InitListExpr of type Foo, so the
  // aggregate rule marks every member used, not just the designated ones.
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int a;
  int b;
};
Foo make() { return Foo{.a = 1}; }
)cpp")
                  .empty());
}

TEST_F(UnusedMemberCheckTest, StructuredBindingReferencesEveryMember) {
  EXPECT_TRUE(flagged(R"cpp(
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

TEST_F(UnusedMemberCheckTest, OffsetofIsAReference) {
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int a;
};
unsigned long offset() { return __builtin_offsetof(Foo, a); }
)cpp")
                  .empty());
}

TEST_F(UnusedMemberCheckTest, AnonymousUnionMemberIsReferencedThroughParent) {
  EXPECT_EQ(flagged(R"cpp(
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

TEST_F(UnusedMemberCheckTest, DependentAccessSuppressesByName) {
  // t.value can't be resolved in an uninstantiated template, so any member
  // named "value" is conservatively treated as used.
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  int value;
  int other;
};
template <typename T> int get(const T &t) { return t.value; }
)cpp"),
            Names{"Foo::other"});
}

TEST_F(UnusedMemberCheckTest, IgnoresMembersOutsideSourceRoot) {
  EXPECT_EQ(flagged({{"/fixture/src/main.cc", R"cpp(
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

TEST_F(UnusedMemberCheckTest, ReferenceInAnotherTUCounts) {
  const clang::tooling::FileContentMappings headers = {
      {"/fixture/src/Foo.hh", R"cpp(
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
  EXPECT_EQ(flagged({useA}, headers), Names{"Foo::b"});
  EXPECT_EQ(flagged({useB}, headers), Names{"Foo::a"});
  // ...but together they reference both: the field is matched across TUs.
  EXPECT_TRUE(flagged({useA, useB}, headers).empty());
}
