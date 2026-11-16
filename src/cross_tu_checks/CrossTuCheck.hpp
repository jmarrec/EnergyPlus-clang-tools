#pragma once

#include "cross_tu_checks/CrossTuCommon.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace clang {
class ASTConsumer;
class CompilerInstance;
} // namespace clang

namespace cross_tu_checks {

// Abstract base class that every individual cross-TU check derives from (and
// is then registered in cross_tu_checks.cpp's makeAllChecks()).
//
// A cross-TU check only decides after every TU has been processed. Each TU is
// parsed once and its AST handed to every check's consumer, so a check
// accumulates what it needs across TUs and reports at the end.
class CrossTuCheck {
public:
  CrossTuCheck() = default;
  CrossTuCheck(const CrossTuCheck &) = delete;
  CrossTuCheck &operator=(const CrossTuCheck &) = delete;
  CrossTuCheck(CrossTuCheck &&) = delete;
  CrossTuCheck &operator=(CrossTuCheck &&) = delete;
  virtual ~CrossTuCheck() = default;

  // The diagnostic name, eg "energyplus-unused-member".
  virtual std::string_view name() const = 0;

  // Called once per TU, possibly concurrently from several threads: whatever
  // the consumer records into the check must be synchronized.
  virtual std::unique_ptr<clang::ASTConsumer>
  createConsumer(clang::CompilerInstance &compiler) = 0;

  // Called once, after every TU has been processed.
  virtual std::vector<Finding> findings() const = 0;
};

} // namespace cross_tu_checks
