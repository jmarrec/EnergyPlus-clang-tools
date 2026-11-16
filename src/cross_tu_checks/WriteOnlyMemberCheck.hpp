#pragma once

#include "cross_tu_checks/CrossTuCheck.hpp"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/Type.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cross_tu_checks {

enum class Access : unsigned { None = 0, Read = 1, Write = 2 };

inline Access operator|(Access left, Access right) {
  return static_cast<Access>(static_cast<unsigned>(left) |
                             static_cast<unsigned>(right));
}

inline bool hasAccess(Access value, Access requested) {
  return (static_cast<unsigned>(value) & static_cast<unsigned>(requested)) != 0;
}

// Fields that are written (assigned, member-initialized, aggregate-initialized,
// ...) but never read in any scanned TU.
class WriteOnlyMemberCheck final : public CrossTuCheck {
public:
  explicit WriteOnlyMemberCheck(fs::path sourceRoot)
      : sourceRoot_(std::move(sourceRoot)) {}

  void recordDeclaration(const clang::FieldDecl *field,
                         const clang::SourceManager &sourceManager) {
    auto info = describeField(field, sourceManager, sourceRoot_);
    if (!info) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    declarations_.try_emplace(info->usr, std::move(*info));
  }

  void recordAccess(const clang::FieldDecl *field, Access access) {
    std::string usr = usrFor(field);
    if (usr.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (hasAccess(access, Access::Read)) {
      reads_.insert(usr);
    }
    if (hasAccess(access, Access::Write)) {
      writes_.insert(std::move(usr));
    }
  }

  void recordUnresolvedRead(clang::DeclarationName name) {
    if (name.isEmpty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    unresolvedReadNames_.insert(name.getAsString());
  }

  void recordAggregateAccess(clang::QualType type, Access access) {
    recordAggregateAccess(type.getNonReferenceType()->getAsRecordDecl(),
                          access);
  }

  // Takes the RecordDecl directly so callers holding one don't need to build a
  // QualType for it: ASTContext::getRecordType() was removed in LLVM 22.
  void recordAggregateAccess(const clang::RecordDecl *record, Access access) {
    if (!record) {
      return;
    }
    for (const auto *field : record->fields()) {
      recordAccess(field, access);
    }
    if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
      for (const auto &base : cxxRecord->bases()) {
        recordAggregateAccess(base.getType(), access);
      }
    }
  }

  static constexpr std::string_view checkName = "energyplus-write-only-member";

  std::string_view name() const override { return checkName; }

  std::unique_ptr<clang::ASTConsumer>
  createConsumer(clang::CompilerInstance &compiler) override;

  std::vector<Finding> findings() const override {
    std::vector<Finding> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &[usr, field] : declarations_) {
      if (writes_.find(usr) != writes_.end() &&
          reads_.find(usr) == reads_.end() &&
          unresolvedReadNames_.find(field.name) == unresolvedReadNames_.end()) {
        result.push_back(
            {field, std::string(checkName),
             describeMember(field) + " is written but never read"});
      }
    }
    return result;
  }

private:
  fs::path sourceRoot_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, FieldInfo> declarations_;
  std::unordered_set<std::string> reads_;
  std::unordered_set<std::string> writes_;
  std::unordered_set<std::string> unresolvedReadNames_;
};

} // namespace cross_tu_checks
