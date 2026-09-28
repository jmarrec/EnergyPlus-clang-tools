#include "path_format_fixer/PathFormatFixer.hpp"
#include "Fixture.hpp"

#include <gtest/gtest.h>

using path_format_fixer::Field;
using path_format_fixer::scanReplacementFields;

namespace {

// Wraps a snippet of `demo()` body code in the boilerplate the matcher needs:
// a `fs::path` alias and a stub `EnergyPlus::format`.
std::string wrap(llvm::StringRef Body) {
  return (R"cpp(
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <fmt/core.h>

namespace fs = std::filesystem;
namespace EnergyPlus {
  template <typename... Args>
  std::string format(std::string_view format_str, Args &&...args) { return std::string(format_str); }
} // namespace EnergyPlus
void demo(const fs::path &filePath, const fs::path &other) {
)cpp" + Body.str()
          + "}\n");
}

}  // namespace

TEST(ScanReplacementFields, AutoNumbered) {
  auto Fields = scanReplacementFields(R"("{}, {}")");
  ASSERT_TRUE(Fields.has_value());
  ASSERT_EQ(Fields->size(), 2u);
  EXPECT_EQ((*Fields)[0].ArgIndex, 0);
  EXPECT_EQ((*Fields)[0].Inner, "");
  EXPECT_EQ((*Fields)[1].ArgIndex, 1);
}

TEST(ScanReplacementFields, ExplicitIndices) {
  auto Fields = scanReplacementFields(R"("{1}, {0}")");
  ASSERT_TRUE(Fields.has_value());
  ASSERT_EQ(Fields->size(), 2u);
  EXPECT_EQ((*Fields)[0].ArgIndex, 1);
  EXPECT_EQ((*Fields)[1].ArgIndex, 0);
}

TEST(ScanReplacementFields, SkipsEscapedBraces) {
  auto Fields = scanReplacementFields(R"("{{literal}} {}")");
  ASSERT_TRUE(Fields.has_value());
  ASSERT_EQ(Fields->size(), 1u);
  EXPECT_EQ((*Fields)[0].ArgIndex, 0);
}

TEST(ScanReplacementFields, CapturesFormatSpec) {
  auto Fields = scanReplacementFields(R"("{:>20}")");
  ASSERT_TRUE(Fields.has_value());
  ASSERT_EQ(Fields->size(), 1u);
  EXPECT_EQ((*Fields)[0].Inner, ":>20");
}

TEST(ScanReplacementFields, UnterminatedFieldFails) {
  EXPECT_FALSE(scanReplacementFields(R"("{")").has_value());
}

TEST(RunOnCodeEnergyPlus, RewritesPlainStringCall) {
  std::string Out = runOnCode(wrap("EnergyPlus::format(\"{}, {}\\n\", 1, filePath.string());\n"));
  EXPECT_NE(Out.find("std::format(\"{}, {}\\n\", 1, filePath);"), std::string::npos);
  EXPECT_EQ(Out.find(".string()"), std::string::npos);
}

TEST(RunOnCodeEnergyPlus, RewritesGenericStringCallAndInsertsFormatSpec) {
  std::string Out = runOnCode(wrap("EnergyPlus::format(\"{}, {}\\n\", 1, filePath.generic_string());\n"));
  EXPECT_NE(Out.find("std::format(\"{}, {:g}\\n\", 1, filePath);"), std::string::npos);
  EXPECT_EQ(Out.find("generic_string"), std::string::npos);
}

TEST(RunOnCodeEnergyPlus, HandlesExplicitArgumentIndices) {
  std::string Out = runOnCode(wrap("EnergyPlus::format(\"{1}, {0}\\n\", filePath.string(), 1);\n"));
  EXPECT_NE(Out.find("std::format(\"{1}, {0}\\n\", filePath, 1);"), std::string::npos);
}

TEST(RunOnCodeEnergyPlus, RewritesMultiplePathArguments) {
  std::string Out = runOnCode(wrap("EnergyPlus::format(\"a={} b={}\\n\", filePath.generic_string(), other.string());\n"));
  EXPECT_NE(Out.find("std::format(\"a={:g} b={}\\n\", filePath, other);"), std::string::npos);
}

TEST(RunOnCodeEnergyPlus, SkipsGenericStringWhenFieldAlreadyHasFormatSpec) {
  std::string Code = wrap("EnergyPlus::format(\"{:>20}\\n\", filePath.generic_string());\n");
  std::string Out = runOnCode(Code);
  // Diagnostic prevents the rewrite: the source is left untouched.
  EXPECT_EQ(Out, Code);
}

TEST(RunOnCodeEnergyPlus, LeavesEscapedBracesAlone) {
  std::string Out = runOnCode(wrap("EnergyPlus::format(\"{{literal}} {}\\n\", filePath.string());\n"));
  EXPECT_NE(Out.find("std::format(\"{{literal}} {}\\n\", filePath);"), std::string::npos);
}

TEST(RunOnCodeFMT, RewritesPlainStringCall) {
  std::string Out = runOnCode(wrap("fmt::format(\"{}, {}\\n\", 1, filePath.string());\n"));
  EXPECT_NE(Out.find("std::format(\"{}, {}\\n\", 1, filePath);"), std::string::npos);
  EXPECT_EQ(Out.find(".string()"), std::string::npos);
}

TEST(RunOnCodeFMT, RewritesGenericStringCallAndInsertsFormatSpec) {
  std::string Out = runOnCode(wrap("fmt::format(\"{}, {}\\n\", 1, filePath.generic_string());\n"));
  EXPECT_NE(Out.find("std::format(\"{}, {:g}\\n\", 1, filePath);"), std::string::npos);
  EXPECT_EQ(Out.find("generic_string"), std::string::npos);
}

TEST(RunOnCodeFMT, HandlesExplicitArgumentIndices) {
  std::string Out = runOnCode(wrap("fmt::format(\"{1}, {0}\\n\", filePath.string(), 1);\n"));
  EXPECT_NE(Out.find("std::format(\"{1}, {0}\\n\", filePath, 1);"), std::string::npos);
}

TEST(RunOnCodeFMT, RewritesMultiplePathArguments) {
  std::string Out = runOnCode(wrap("fmt::format(\"a={} b={}\\n\", filePath.generic_string(), other.string());\n"));
  EXPECT_NE(Out.find("std::format(\"a={:g} b={}\\n\", filePath, other);"), std::string::npos);
}

TEST(RunOnCodeFMT, SkipsGenericStringWhenFieldAlreadyHasFormatSpec) {
  std::string Code = wrap("fmt::format(\"{:>20}\\n\", filePath.generic_string());\n");
  std::string Out = runOnCode(Code);
  // Diagnostic prevents the rewrite: the source is left untouched.
  EXPECT_EQ(Out, Code);
}

TEST(RunOnCodeFMT, LeavesEscapedBracesAlone) {
  std::string Out = runOnCode(wrap("fmt::format(\"{{literal}} {}\\n\", filePath.string());\n"));
  EXPECT_NE(Out.find("std::format(\"{{literal}} {}\\n\", filePath);"), std::string::npos);
}

TEST(RunOnCodeSTL, RewritesPlainStringCall) {
  std::string Out = runOnCode(wrap("std::format(\"{}, {}\\n\", 1, filePath.string());\n"));
  EXPECT_NE(Out.find("std::format(\"{}, {}\\n\", 1, filePath);"), std::string::npos);
  EXPECT_EQ(Out.find(".string()"), std::string::npos);
}

TEST(RunOnCodeSTL, RewritesGenericStringCallAndInsertsFormatSpec) {
  std::string Out = runOnCode(wrap("std::format(\"{}, {}\\n\", 1, filePath.generic_string());\n"));
  EXPECT_NE(Out.find("std::format(\"{}, {:g}\\n\", 1, filePath);"), std::string::npos);
  EXPECT_EQ(Out.find("generic_string"), std::string::npos);
}

TEST(RunOnCodeSTL, HandlesExplicitArgumentIndices) {
  std::string Out = runOnCode(wrap("std::format(\"{1}, {0}\\n\", filePath.string(), 1);\n"));
  EXPECT_NE(Out.find("std::format(\"{1}, {0}\\n\", filePath, 1);"), std::string::npos);
}

TEST(RunOnCodeSTL, RewritesMultiplePathArguments) {
  std::string Out = runOnCode(wrap("std::format(\"a={} b={}\\n\", filePath.generic_string(), other.string());\n"));
  EXPECT_NE(Out.find("std::format(\"a={:g} b={}\\n\", filePath, other);"), std::string::npos);
}

TEST(RunOnCodeSTL, SkipsGenericStringWhenFieldAlreadyHasFormatSpec) {
  std::string Code = wrap("std::format(\"{:>20}\\n\", filePath.generic_string());\n");
  std::string Out = runOnCode(Code);
  // Diagnostic prevents the rewrite: the source is left untouched.
  EXPECT_EQ(Out, Code);
}

TEST(RunOnCodeSTL, LeavesEscapedBracesAlone) {
  std::string Out = runOnCode(wrap("std::format(\"{{literal}} {}\\n\", filePath.string());\n"));
  EXPECT_NE(Out.find("std::format(\"{{literal}} {}\\n\", filePath);"), std::string::npos);
}

TEST(RunOnCode, IgnoresUnrelatedCalls) {
  std::string Code = wrap("(void)filePath.string();\n(void)filePath.generic_string();\n");
  EXPECT_EQ(runOnCode(Code), Code);
}

TEST(RunOnCode, RespectsCustomFormatFunctionName) {
  std::string Code = "#include <filesystem>\n"
                     "#include <string>\n"
                     "#include <string_view>\n"
                     "namespace fs = std::filesystem;\n"
                     "namespace MyNs {\n"
                     "template <typename... Args>\n"
                     "std::string fmt(std::string_view f, Args &&...a) { return std::string(f); }\n"
                     "} // namespace MyNs\n"
                     "void demo(const fs::path &filePath) {\n"
                     "  MyNs::fmt(\"{}\\n\", filePath.string());\n"
                     "}\n";

  // Default function name doesn't match `MyNs::fmt` -> no rewrite.
  EXPECT_EQ(runOnCode(Code), Code);

  // Matching the custom name rewrites it (and normalizes it onto std::format).
  std::string Out = runOnCode(Code, {"MyNs::fmt"});
  EXPECT_NE(Out.find("std::format(\"{}\\n\", filePath);"), std::string::npos);
  EXPECT_EQ(Out.find("MyNs::fmt"), std::string::npos);
}
