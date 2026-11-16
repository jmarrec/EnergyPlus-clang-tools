#pragma once

// Shared harness for testing a CrossTuCheck without AllTUsToolExecutor or a
// compile database: each snippet is parsed as its own TU and fed to a single
// check instance, then the check's findings() are collected.

#include "cross_tu_checks/CrossTuCheck.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace cross_tu_checks_test {

using Names = std::vector<std::string>;

// Hands each TU's AST to the check, as cross_tu_checks' CrossTuAction does.
class CheckAction final : public clang::ASTFrontendAction {
public:
  explicit CheckAction(cross_tu_checks::CrossTuCheck &check) : check_(check) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef /*inFile*/) override {
    return check_.createConsumer(compiler);
  }

private:
  cross_tu_checks::CrossTuCheck &check_;
};

struct TranslationUnit {
  std::string path;
  std::string code;
};

// Typed fixture: `using FooCheckTest = CrossTuCheckTest<FooCheck>;` then
// TEST_F(FooCheckTest, ...).
template <typename Check> class CrossTuCheckTest : public ::testing::Test {
protected:
  // Virtual (in-memory) files live under here; only fields declared below it
  // are considered, just like --source-root on a real build.
  static const cross_tu_checks::fs::path &sourceRoot() {
    static const cross_tu_checks::fs::path root{"/fixture/src"};
    return root;
  }

  // Runs every TU through a fresh Check (headers are shared by all TUs) and
  // returns the sorted qualified names of the members it reports.
  static Names
  flagged(const std::vector<TranslationUnit> &tus,
          const clang::tooling::FileContentMappings &headers = {}) {
    Check check{sourceRoot()};
    for (const TranslationUnit &tu : tus) {
      EXPECT_TRUE(clang::tooling::runToolOnCodeWithArgs(
          std::make_unique<CheckAction>(check), tu.code, {"-std=c++20"},
          tu.path, std::string(check.name()),
          std::make_shared<clang::PCHContainerOperations>(), headers))
          << tu.path << " failed to compile";
    }

    Names names;
    for (const cross_tu_checks::Finding &finding : check.findings()) {
      names.push_back(finding.field.qualifiedName);
    }
    std::ranges::sort(names);
    return names;
  }

  // A single TU, as <sourceRoot>/main.cc, with no extra headers.
  static Names flagged(const std::string &code) {
    const TranslationUnit mainTu{.path = (sourceRoot() / "main.cc").string(),
                                 .code = code};
    return flagged({mainTu});
  }
};

} // namespace cross_tu_checks_test
