#include "RecurringStaticMessageCheck.hpp"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"

using namespace clang;
using namespace clang::ast_matchers;

namespace recurring_static_message_check {

namespace {

// Walks a message-argument subtree: true if it contains at least one string
// literal and zero identifier references (DeclRefExpr/MemberExpr), however
// those references could have gotten in -- operator+ concatenation, a
// std::format()/fmt::format() argument, etc. Unlike libclang's stable C API,
// the real AST distinguishes CXXOperatorCallExpr/CXXConstructExpr/CallExpr
// properly, but that distinction turns out not to matter for this check: an
// identifier reference is the only thing that can make the message vary
// between objects, regardless of the expression shape it's embedded in.
class IdentifierHunter : public RecursiveASTVisitor<IdentifierHunter> {
public:
  bool SawStringLiteral = false;
  bool SawIdentifierRef = false;

  bool VisitStringLiteral(StringLiteral * /*Lit*/) {
    SawStringLiteral = true;
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr * /*Ref*/) {
    SawIdentifierRef = true;
    return true;
  }
  bool VisitMemberExpr(MemberExpr * /*Ref*/) {
    SawIdentifierRef = true;
    return true;
  }
};

bool isStringLiteralOnly(const Expr *E) {
  IdentifierHunter Hunter;
  // RecursiveASTVisitor::TraverseStmt() only takes a non-const Stmt*; it never mutates
  // the AST, so this is the standard, safe pattern for a read-only visitor in clang tooling.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  Hunter.TraverseStmt(const_cast<Expr *>(E));
  return Hunter.SawStringLiteral && !Hunter.SawIdentifierRef;
}

} // namespace

StatementMatcher makeMatcher(const std::vector<std::string> &RecurringFunctionNames) {
  std::vector<StringRef> Names(RecurringFunctionNames.begin(), RecurringFunctionNames.end());
  return callExpr(callee(functionDecl(hasAnyName(Names))), argumentCountAtLeast(2)).bind("call");
}

void Callback::run(const MatchFinder::MatchResult &Result) {
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
  if (!Call) {
    return;
  }

  const Expr *MsgArg = Call->getArg(1);
  if (!isStringLiteralOnly(MsgArg)) {
    return;
  }

  FoundAny = true;
  SourceLocation Loc = Call->getBeginLoc();
  DiagnosticsEngine &Diags = Result.Context->getDiagnostics();
  unsigned ID = Diags.getCustomDiagID(
      DiagnosticsEngine::Warning,
      "%0 has a pure string-literal message (no identifier reference) -- "
      "same text on every call, regardless of which object trips it");
  Diags.Report(Loc, ID) << Call->getDirectCallee()->getNameAsString();
}

} // namespace recurring_static_message_check
