#include "cross_tu_checks/WriteOnlyMemberCheck.hpp"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"

namespace cross_tu_checks {

namespace {

class WriteOnlyMemberVisitor final
    : public clang::RecursiveASTVisitor<WriteOnlyMemberVisitor> {
public:
  WriteOnlyMemberVisitor(WriteOnlyMemberFindings &findings,
                         clang::ASTContext &context)
      : findings_(findings), context_(context) {}

  bool VisitFieldDecl(clang::FieldDecl *field) {
    findings_.recordDeclaration(field, context_.getSourceManager());
    if (field->hasInClassInitializer()) {
      findings_.recordAccess(field, Access::Write);
    }
    return true;
  }

  bool VisitMemberExpr(clang::MemberExpr *expression) {
    recordNamedMember(expression->getMemberDecl(), classify(expression));
    return true;
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *expression) {
    recordNamedMember(expression->getDecl(), classify(expression));
    return true;
  }

  bool VisitCXXDependentScopeMemberExpr(
      clang::CXXDependentScopeMemberExpr *expression) {
    if (hasAccess(classify(expression), Access::Read)) {
      findings_.recordUnresolvedRead(expression->getMember());
    }
    return true;
  }

  bool VisitUnresolvedMemberExpr(clang::UnresolvedMemberExpr *expression) {
    if (hasAccess(classify(expression), Access::Read)) {
      findings_.recordUnresolvedRead(expression->getMemberName());
    }
    return true;
  }

  bool VisitInitListExpr(clang::InitListExpr *expression) {
    // RecursiveASTVisitor sees the written (syntactic) initializer, but Clang
    // keeps field mapping on its semantic counterpart when the two differ.
    const clang::InitListExpr *semanticExpression = expression;
    if (const auto *semanticForm = expression->getSemanticForm()) {
      semanticExpression = semanticForm;
    }
    if (const auto *unionField =
            semanticExpression->getInitializedFieldInUnion()) {
      findings_.recordAccess(unionField, Access::Write);
    } else {
      findings_.recordAggregateAccess(semanticExpression->getType(),
                                      Access::Write);
    }
    return true;
  }

  bool VisitCXXParenListInitExpr(clang::CXXParenListInitExpr *expression) {
    findings_.recordAggregateAccess(expression->getType(), Access::Write);
    return true;
  }

  bool VisitDecompositionDecl(clang::DecompositionDecl *declaration) {
    findings_.recordAggregateAccess(declaration->getType(), Access::Read);
    return true;
  }

  bool VisitOffsetOfExpr(clang::OffsetOfExpr *expression) {
    for (unsigned index = 0; index < expression->getNumComponents(); ++index) {
      const auto &component = expression->getComponent(index);
      if (component.getKind() == clang::OffsetOfNode::Field) {
        findings_.recordAccess(component.getField(), Access::Read);
      }
    }
    return true;
  }

  bool VisitDesignatedInitExpr(clang::DesignatedInitExpr *expression) {
    for (const auto &designator : expression->designators()) {
      if (designator.isFieldDesignator()) {
        findings_.recordAccess(designator.getFieldDecl(), Access::Write);
      }
    }
    return true;
  }

  bool VisitCXXConstructExpr(clang::CXXConstructExpr *expression) {
    const auto *constructor = expression->getConstructor();
    if (expression->requiresZeroInitialization()) {
      findings_.recordAggregateAccess(expression->getType(), Access::Write);
    }
    if (constructor && constructor->isDefaulted() &&
        (constructor->isCopyConstructor() ||
         constructor->isMoveConstructor())) {
      findings_.recordAggregateAccess(expression->getType(),
                                      Access::Read | Access::Write);
    }
    return true;
  }

  bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr *expression) {
    const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(
        expression->getDirectCallee());
    if (method && method->isDefaulted() &&
        (method->isCopyAssignmentOperator() ||
         method->isMoveAssignmentOperator())) {
      findings_.recordAggregateAccess(method->getParent(),
                                      Access::Read | Access::Write);
    }
    return true;
  }

  bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr *expression) {
    const auto *method = expression->getMethodDecl();
    if (method && method->isDefaulted() &&
        (method->isCopyAssignmentOperator() ||
         method->isMoveAssignmentOperator())) {
      findings_.recordAggregateAccess(method->getParent(),
                                      Access::Read | Access::Write);
    }
    return true;
  }

  bool VisitBinaryOperator(clang::BinaryOperator *expression) {
    if (expression->getOpcode() == clang::BO_Assign &&
        expression->getLHS()->getType()->isRecordType()) {
      findings_.recordAggregateAccess(expression->getLHS()->getType(),
                                      Access::Read | Access::Write);
    }
    return true;
  }

  bool TraverseConstructorInitializer(clang::CXXCtorInitializer *initializer) {
    if (initializer->isWritten() && initializer->isAnyMemberInitializer()) {
      findings_.recordAccess(initializer->getAnyMember(), Access::Write);
    }
    return clang::RecursiveASTVisitor<
        WriteOnlyMemberVisitor>::TraverseConstructorInitializer(initializer);
  }

private:
  static bool sameExpression(const clang::Expr *left,
                             const clang::Expr *right) {
    return left && right &&
           left->IgnoreParenImpCasts() == right->IgnoreParenImpCasts();
  }

  Access classify(const clang::Expr *expression) const {
    const clang::Expr *current = expression;
    Access result = Access::Read;
    bool classified = false;

    while (current) {
      const auto parents = context_.getParents(*current);
      if (parents.size() != 1) {
        return result;
      }
      const auto *parent = parents[0].get<clang::Expr>();
      if (!parent) {
        return result;
      }

      if (llvm::isa<clang::UnaryExprOrTypeTraitExpr>(parent) ||
          llvm::isa<clang::CXXNoexceptExpr>(parent)) {
        return Access::None;
      }
      if (const auto *typeidExpression =
              llvm::dyn_cast<clang::CXXTypeidExpr>(parent)) {
        if (!typeidExpression->isPotentiallyEvaluated()) {
          return Access::None;
        }
      }
      if (const auto *call = llvm::dyn_cast<clang::CallExpr>(parent)) {
        if (call->isUnevaluatedBuiltinCall(context_)) {
          return Access::None;
        }
      }
      if (const auto *choose = llvm::dyn_cast<clang::ChooseExpr>(parent)) {
        if (!choose->isConditionDependent() &&
            !sameExpression(current, choose->getChosenSubExpr())) {
          return Access::None;
        }
      }

      if (!classified && llvm::isa<clang::ParenExpr>(parent)) {
        current = parent;
        continue;
      }

      if (!classified) {
        if (const auto *binary =
                llvm::dyn_cast<clang::BinaryOperator>(parent)) {
          if (binary->isAssignmentOp() &&
              sameExpression(current, binary->getLHS())) {
            result = binary->getOpcode() == clang::BO_Assign
                         ? Access::Write
                         : Access::Read | Access::Write;
          } else {
            result = Access::Read;
          }
        } else if (const auto *unary =
                       llvm::dyn_cast<clang::UnaryOperator>(parent)) {
          if (unary->isIncrementDecrementOp()) {
            result = Access::Read | Access::Write;
          } else if (unary->getOpcode() == clang::UO_AddrOf) {
            // Once the address escapes, a callee can both read and write it.
            result = Access::Read | Access::Write;
          } else {
            result = Access::Read;
          }
        } else if (const auto *operatorCall =
                       llvm::dyn_cast<clang::CXXOperatorCallExpr>(parent)) {
          const bool isFirstArgument =
              operatorCall->getNumArgs() > 0 &&
              sameExpression(current, operatorCall->getArg(0));
          if (isFirstArgument) {
            switch (operatorCall->getOperator()) {
            case clang::OO_Equal:
              result = Access::Write;
              break;
            case clang::OO_PlusEqual:
            case clang::OO_MinusEqual:
            case clang::OO_StarEqual:
            case clang::OO_SlashEqual:
            case clang::OO_PercentEqual:
            case clang::OO_CaretEqual:
            case clang::OO_AmpEqual:
            case clang::OO_PipeEqual:
            case clang::OO_LessLessEqual:
            case clang::OO_GreaterGreaterEqual:
            case clang::OO_PlusPlus:
            case clang::OO_MinusMinus:
              result = Access::Read | Access::Write;
              break;
            default:
              result = Access::Read;
              break;
            }
          } else {
            result = Access::Read;
          }
        } else {
          result = Access::Read;
        }
        classified = true;
      }
      current = parent;
    }
    return result;
  }

  void recordNamedMember(const clang::ValueDecl *member, Access access) {
    if (access == Access::None) {
      return;
    }
    if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(member)) {
      findings_.recordAccess(field, access);
    } else if (const auto *indirectField =
                   llvm::dyn_cast<clang::IndirectFieldDecl>(member)) {
      for (const clang::NamedDecl *declaration : indirectField->chain()) {
        if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(declaration)) {
          findings_.recordAccess(field, access);
        }
      }
    }
  }

  WriteOnlyMemberFindings &findings_;
  clang::ASTContext &context_;
};

class WriteOnlyMemberConsumer final : public clang::ASTConsumer {
public:
  WriteOnlyMemberConsumer(WriteOnlyMemberFindings &findings,
                          clang::ASTContext &context)
      : visitor_(findings, context) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    visitor_.TraverseDecl(context.getTranslationUnitDecl());
  }

private:
  WriteOnlyMemberVisitor visitor_;
};

} // namespace

std::unique_ptr<clang::ASTConsumer>
makeWriteOnlyMemberConsumer(WriteOnlyMemberFindings &findings,
                            clang::CompilerInstance &compiler) {
  return std::make_unique<WriteOnlyMemberConsumer>(findings,
                                                   compiler.getASTContext());
}

} // namespace cross_tu_checks
