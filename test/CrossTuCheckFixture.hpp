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

// Virtual (in-memory) files live under here; only fields declared below
// sourceRoot are considered, just like --source-root on a real build.
constexpr auto *sourceRoot = "/fixture/src";

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

// Runs every TU through `check` (headers are shared by all TUs) and returns
// the sorted qualified names of the members it reports.
inline Names
flaggedMembers(cross_tu_checks::CrossTuCheck &check,
               const std::vector<TranslationUnit> &tus,
               const clang::tooling::FileContentMappings &headers) {
  for (const TranslationUnit &tu : tus) {
    EXPECT_TRUE(clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<CheckAction>(check), tu.code, {"-std=c++20"}, tu.path,
        std::string(check.name()),
        std::make_shared<clang::PCHContainerOperations>(), headers))
        << tu.path << " failed to compile";
  }

  Names names;
  for (const cross_tu_checks::Finding &finding : check.findings()) {
    names.push_back(finding.field.qualifiedName);
  }
  std::sort(names.begin(), names.end());
  return names;
}

template <typename Check>
Names flaggedMembers(const std::vector<TranslationUnit> &tus,
                     const clang::tooling::FileContentMappings &headers = {}) {
  Check check{sourceRoot};
  return flaggedMembers(check, tus, headers);
}

// A single TU.
template <typename Check> Names flaggedMembers(const std::string &code) {
  return flaggedMembers<Check>({{"/fixture/src/main.cc", code}});
}

} // namespace cross_tu_checks_test
