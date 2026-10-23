#include "cross_tu_checks/UnusedMemberCheck.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"

namespace cross_tu_checks {

namespace {

class UnusedMemberVisitor final
    : public clang::RecursiveASTVisitor<UnusedMemberVisitor> {
public:
  UnusedMemberVisitor(UnusedMemberCheck &findings,
                      clang::SourceManager &sourceManager)
      : findings_(findings), sourceManager_(sourceManager) {}

  bool VisitFieldDecl(clang::FieldDecl *field) {
    findings_.recordDeclaration(field, sourceManager_);
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *expression) {
    recordNamedMember(expression->getMemberDecl());
    return true;
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *expression) {
    recordNamedMember(expression->getDecl());
    return true;
  }

  bool VisitCXXDependentScopeMemberExpr(
      clang::CXXDependentScopeMemberExpr *expression) {
    findings_.recordUnresolvedReference(expression->getMember());
    return true;
  }

  bool VisitUnresolvedMemberExpr(clang::UnresolvedMemberExpr *expression) {
    findings_.recordUnresolvedReference(expression->getMemberName());
    return true;
  }

  bool VisitInitListExpr(clang::InitListExpr *expression) {
    findings_.recordAggregateUse(expression->getType());
    return true;
  }

  bool VisitCXXParenListInitExpr(clang::CXXParenListInitExpr *expression) {
    findings_.recordAggregateUse(expression->getType());
    return true;
  }

  bool VisitDecompositionDecl(clang::DecompositionDecl *declaration) {
    findings_.recordAggregateUse(declaration->getType());
    return true;
  }

  bool VisitOffsetOfExpr(clang::OffsetOfExpr *expression) {
    for (unsigned index = 0; index < expression->getNumComponents(); ++index) {
      const auto &component = expression->getComponent(index);
      if (component.getKind() == clang::OffsetOfNode::Field) {
        findings_.recordReference(component.getField());
      }
    }
    return true;
  }

  bool VisitDesignatedInitExpr(clang::DesignatedInitExpr *expression) {
    for (const auto &designator : expression->designators()) {
      if (designator.isFieldDesignator()) {
        findings_.recordReference(designator.getFieldDecl());
      }
    }
    return true;
  }

  bool TraverseConstructorInitializer(clang::CXXCtorInitializer *initializer) {
    if (initializer->isWritten() && initializer->isAnyMemberInitializer()) {
      findings_.recordReference(initializer->getAnyMember());
    }
    return clang::RecursiveASTVisitor<
        UnusedMemberVisitor>::TraverseConstructorInitializer(initializer);
  }

private:
  void recordNamedMember(const clang::ValueDecl *member) {
    if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(member)) {
      findings_.recordReference(field);
    } else if (const auto *indirectField =
                   llvm::dyn_cast<clang::IndirectFieldDecl>(member)) {
      for (const clang::NamedDecl *declaration : indirectField->chain()) {
        if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(declaration)) {
          findings_.recordReference(field);
        }
      }
    }
  }

  UnusedMemberCheck &findings_;
  clang::SourceManager &sourceManager_;
};

class UnusedMemberConsumer final : public clang::ASTConsumer {
public:
  UnusedMemberConsumer(UnusedMemberCheck &findings,
                       clang::SourceManager &sourceManager)
      : visitor_(findings, sourceManager) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    visitor_.TraverseDecl(context.getTranslationUnitDecl());
  }

private:
  UnusedMemberVisitor visitor_;
};

} // namespace

std::unique_ptr<clang::ASTConsumer>
UnusedMemberCheck::createConsumer(clang::CompilerInstance &compiler) {
  return std::make_unique<UnusedMemberConsumer>(*this,
                                                compiler.getSourceManager());
}

} // namespace cross_tu_checks
