#include "cross_tu_checks/WriteOnlyMemberCheck.hpp"

#include "CrossTuCheckFixture.hpp"

#include <gtest/gtest.h>

using namespace cross_tu_checks_test;
using cross_tu_checks::WriteOnlyMemberCheck;

using WriteOnlyMemberCheckTest = CrossTuCheckTest<WriteOnlyMemberCheck>;

TEST_F(WriteOnlyMemberCheckTest, FlagsMemberAssignedButNeverRead) {
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  int readToo;
  int writeOnly;
};
int update(Foo &foo) {
  foo.readToo = 1;
  foo.writeOnly = 2;
  return foo.readToo;
}
)cpp"),
            Names{"Foo::writeOnly"});
}

TEST_F(WriteOnlyMemberCheckTest, InClassInitializerIsAWrite) {
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  double value = 0.0;
};
)cpp"),
            Names{"Foo::value"});
}

TEST_F(WriteOnlyMemberCheckTest, ConstructorInitializerIsAWrite) {
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  Foo() : value(1) {}
  int value;
};
)cpp"),
            Names{"Foo::value"});
}

TEST_F(WriteOnlyMemberCheckTest, IgnoresMemberNeverTouched) {
  // No write at all: that's energyplus-unused-member's job, not this check's.
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int value;
};
)cpp")
                  .empty());
}

TEST_F(WriteOnlyMemberCheckTest, IgnoresMemberReadButNeverWritten) {
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int value;
};
int get(const Foo &foo) { return foo.value; }
)cpp")
                  .empty());
}

TEST_F(WriteOnlyMemberCheckTest, CompoundAssignmentAndIncrementAreReads) {
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int sum = 0;
  int count = 0;
};
void add(Foo &foo, int value) {
  foo.sum += value;
  ++foo.count;
}
)cpp")
                  .empty());
}

TEST_F(WriteOnlyMemberCheckTest, UnevaluatedOperandIsNotARead) {
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  int sized = 0;
  int noexcepted = 0;
};
unsigned long size(const Foo &foo) { return sizeof(foo.sized); }
bool isNoexcept(const Foo &foo) { return noexcept(foo.noexcepted); }
)cpp"),
            (Names{"Foo::noexcepted", "Foo::sized"}));
}

TEST_F(WriteOnlyMemberCheckTest, TakingTheAddressIsARead) {
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int value = 0;
};
int *address(Foo &foo) { return &foo.value; }
)cpp")
                  .empty());
}

TEST_F(WriteOnlyMemberCheckTest, PassingByReferenceIsARead) {
  // Even a non-const reference: the callee might read it.
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int byConstRef = 0;
  int byRef = 0;
};
void read(const int &value);
void mutate(int &value);
void use(Foo &foo) {
  read(foo.byConstRef);
  mutate(foo.byRef);
}
)cpp")
                  .empty());
}

TEST_F(WriteOnlyMemberCheckTest, UserDefinedAssignmentOperatorIsAWrite) {
  EXPECT_EQ(flagged(R"cpp(
struct Value {
  Value &operator=(int) { return *this; }
};
struct Foo {
  Value value;
};
void set(Foo &foo) { foo.value = 1; }
)cpp"),
            Names{"Foo::value"});
}

TEST_F(WriteOnlyMemberCheckTest, DefaultedCopyIsARead) {
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int copied = 0;
  int assigned = 0;
};
struct Bar {
  int assigned = 0;
};
Foo copy(const Foo &foo) { return foo; }
void assign(Bar &to, const Bar &from) { to = from; }
)cpp")
                  .empty());
}

TEST_F(WriteOnlyMemberCheckTest, AggregateInitIsAWrite) {
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  int a;
  int b;
};
int get() {
  Foo foo{1, 2};
  return foo.a;
}
)cpp"),
            Names{"Foo::b"});
}

TEST_F(WriteOnlyMemberCheckTest, ValueInitializationIsAWrite) {
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  int value;
};
Foo make() { return Foo{}; }
)cpp"),
            Names{"Foo::value"});
}

TEST_F(WriteOnlyMemberCheckTest, StructuredBindingIsARead) {
  EXPECT_TRUE(flagged(R"cpp(
struct Foo {
  int a = 0;
  int b = 0;
};
int sum(const Foo &foo) {
  auto [a, b] = foo;
  return a + b;
}
)cpp")
                  .empty());
}

TEST_F(WriteOnlyMemberCheckTest, DependentReadSuppressesByName) {
  // t.value can't be resolved in an uninstantiated template, so any member
  // named "value" is conservatively treated as read.
  EXPECT_EQ(flagged(R"cpp(
struct Foo {
  int value = 0;
  int other = 0;
};
template <typename T> int get(const T &t) { return t.value; }
)cpp"),
            Names{"Foo::other"});
}

TEST_F(WriteOnlyMemberCheckTest, IgnoresMembersOutsideSourceRoot) {
  EXPECT_EQ(flagged({{"/fixture/src/main.cc", R"cpp(
#include "../third_party/External.hh"
struct Foo {
  int value = 0;
};
)cpp"}},
                    {{"/fixture/third_party/External.hh", R"cpp(
#pragma once
struct External {
  int value = 0;
};
)cpp"}}),
            Names{"Foo::value"});
}

TEST_F(WriteOnlyMemberCheckTest, ReadInAnotherTUCounts) {
  const clang::tooling::FileContentMappings headers = {{"/fixture/src/Foo.hh",
                                                        R"cpp(
#pragma once
struct Foo {
  int value;
};
)cpp"}};

  const TranslationUnit writer{"/fixture/src/writer.cc", R"cpp(
#include "Foo.hh"
void set(Foo &foo) { foo.value = 1; }
)cpp"};
  const TranslationUnit reader{"/fixture/src/reader.cc", R"cpp(
#include "Foo.hh"
int get(const Foo &foo) { return foo.value; }
)cpp"};

  // The writer alone sees a write-only member...
  EXPECT_EQ(flagged({writer}, headers), Names{"Foo::value"});
  // ...but the read in the other TU counts: the field is matched across TUs.
  EXPECT_TRUE(flagged({writer, reader}, headers).empty());
}
