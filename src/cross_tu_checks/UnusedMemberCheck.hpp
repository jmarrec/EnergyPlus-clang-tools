#pragma once

#include "cross_tu_checks/CrossTuCommon.hpp"

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

namespace clang {
class ASTConsumer;
class CompilerInstance;
} // namespace clang

namespace cross_tu_checks {

// Fields with no semantic reference at all (never named, positionally
// initialized, bound, or offsetof'd) in any scanned TU.
class UnusedMemberFindings {
public:
  explicit UnusedMemberFindings(fs::path sourceRoot)
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

  void recordReference(const clang::FieldDecl *field) {
    std::string usr = usrFor(field);
    if (usr.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    references_.insert(std::move(usr));
  }

  // Template-dependent member accesses (this->member or Base::member inside a
  // template, before instantiation) can't be resolved to a concrete FieldDecl,
  // so recordReference() never sees them. Track the plain name instead and
  // treat any by-name match as "used" -- conservative, but avoids flagging
  // members that dependent lookup just couldn't confirm.
  void recordUnresolvedReference(clang::DeclarationName name) {
    if (name.isEmpty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    unresolvedReferenceNames_.insert(name.getAsString());
  }

  // Aggregate init (Foo f{1, 2, 3}), C++20 paren-list init, and structured
  // bindings all reference every member positionally without ever naming it via
  // a MemberExpr, so the visitor would otherwise never see those members as
  // referenced.
  void recordAggregateUse(clang::QualType type) {
    type = type.getNonReferenceType();
    const auto *record = type->getAsRecordDecl();
    if (!record) {
      return;
    }
    for (const auto *field : record->fields()) {
      recordReference(field);
    }
    if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
      for (const auto &base : cxxRecord->bases()) {
        recordAggregateUse(base.getType());
      }
    }
  }

  static constexpr const char *checkName = "energyplus-unused-member";

  std::vector<Finding> findings() const {
    std::vector<Finding> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &[usr, field] : declarations_) {
      if (references_.find(usr) == references_.end() &&
          unresolvedReferenceNames_.find(field.name) ==
              unresolvedReferenceNames_.end()) {
        result.push_back(
            {field, checkName, describeMember(field) + " is never referenced"});
      }
    }
    return result;
  }

private:
  fs::path sourceRoot_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, FieldInfo> declarations_;
  std::unordered_set<std::string> references_;
  std::unordered_set<std::string> unresolvedReferenceNames_;
};

std::unique_ptr<clang::ASTConsumer>
makeUnusedMemberConsumer(UnusedMemberFindings &findings,
                         clang::CompilerInstance &compiler);

} // namespace cross_tu_checks
