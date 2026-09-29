// A clang-tidy plugin module, loaded at runtime via `clang-tidy
// --load=<this>.dylib`. Adds two checks:
//  - `energyplus-recurring-static-message`, reusing the same
//  identifier-reference-based
//    logic as RecurringStaticMessageCheck.cpp/recurring_static_message_check
//    (the standalone ClangTool version of this same check) -- see that file's
//    docstring for why identifier references, not call/operator shape, is the
//    right signal here.
//  - `energyplus-mixed-indexing`, reusing
//  MixedIndexingCheck.cpp/mixed_indexing_check
//    (also a standalone ClangTool first) via its findFlaggedVars() -- the pure
//    computation, with no diagnostic reporting of its own.
//    MixedIndexingTidyCheck below calls it and reports through
//    ClangTidyCheck::diag() itself, rather than reusing
//    mixed_indexing_check::Callback (the ClangTool driver's MatchCallback):
//    Callback reports via the raw DiagnosticsEngine, which crashes under real
//    clang-tidy -- ClangTidyDiagnosticConsumer::HandleDiagnostic() maps every
//    diagnostic's ID back to a check name for --checks=/NOLINT filtering, and
//    only diag()-issued diagnostics are registered that way.
//    MixedIndexingCheck.hpp/.cpp is a real static library
//    (mixed_indexing_check_lib) linked into this plugin too, unlike the
//    clang/LLVM libs below (see why not, below): it has no
//    ManagedStatic/cl::opt/registry globals of its own to duplicate, just
//    AST-walking code.
//
// Deliberately does NOT link against clangTidy/clangAST/clangASTMatchers/etc:
// this .so is dlopen()'d into a clang-tidy process that already has all of
// those statically linked in. Linking them again here would create a second
// copy of every global (ClangTidyModuleRegistry, LLVM's cl::opt registry,
// ManagedStatics, ...) -- the plugin would load without error, but register
// itself into a registry instance clang-tidy's own main() never looks at, so
// `--list-checks` would silently never show it. Symbols instead resolve against
// the host process at load time (see CMakeLists.txt's `-undefined
// dynamic_lookup` on this target).

#include "mixed_indexing_check/MixedIndexingCheck.hpp"

#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModule.h"

#include "llvm/Config/llvm-config.h"

// ClangTidyModuleRegistry.h's symbols were merged into ClangTidyModule.h
// (included above) in LLVM 22; only include the standalone header pre-22, where
// it's still required.
#if LLVM_VERSION_MAJOR < 22
#include "clang-tidy/ClangTidyModuleRegistry.h"
#endif

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

using namespace clang::ast_matchers;

namespace clang::tidy::energyplus {

namespace {

// See RecurringStaticMessageCheck.cpp's IdentifierHunter for the full
// rationale.
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
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  Hunter.TraverseStmt(const_cast<Expr *>(E));
  return Hunter.SawStringLiteral && !Hunter.SawIdentifierRef;
}

} // namespace

class RecurringStaticMessageCheck : public ClangTidyCheck {
public:
  RecurringStaticMessageCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(MatchFinder *Finder) override {
    Finder->addMatcher(callExpr(callee(functionDecl(hasAnyName(
                                    "ShowRecurringWarningErrorAtEnd",
                                    "ShowRecurringSevereErrorAtEnd",
                                    "ShowRecurringContinueErrorAtEnd"))),
                                argumentCountAtLeast(2))
                           .bind("call"),
                       this);
  }

  void check(const MatchFinder::MatchResult &Result) override {
    const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
    if (!Call) {
      return;
    }
    const Expr *MsgArg = Call->getArg(1);
    if (!isStringLiteralOnly(MsgArg)) {
      return;
    }
    diag(Call->getBeginLoc(),
         "%0 has a pure string-literal message (no identifier reference) -- "
         "same "
         "text on every call, regardless of which object trips it")
        << Call->getDirectCallee()->getNameAsString();
  }
};

class MixedIndexingTidyCheck : public ClangTidyCheck {
public:
  MixedIndexingTidyCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context),
        Exclusions(mixed_indexing_check::defaultExclusions()) {}

  void registerMatchers(MatchFinder *Finder) override {
    Finder->addMatcher(mixed_indexing_check::makeMatcher(), this);
  }

  void check(const MatchFinder::MatchResult &Result) override {
    const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func");
    if (!Func) {
      return;
    }
    for (const mixed_indexing_check::FlaggedVar &FV :
         mixed_indexing_check::findFlaggedVars(*Func, Exclusions)) {
      diag(FV.DeclLoc, "%0 is used both as operator() (1-indexed convention) "
                       "and operator[] (0-indexed "
                       "convention) in this function -- verify each use has "
                       "the correct index base")
          << FV.Name;
      diag(FV.FirstCallLoc, "used here as operator()", DiagnosticIDs::Note);
      diag(FV.FirstSubscriptLoc, "used here as operator[]",
           DiagnosticIDs::Note);
    }
  }

private:
  std::vector<mixed_indexing_check::MemberExclusion> Exclusions;
};

class EnergyPlusModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<RecurringStaticMessageCheck>(
        "energyplus-recurring-static-message");
    CheckFactories.registerCheck<MixedIndexingTidyCheck>(
        "energyplus-mixed-indexing");
  }
};

} // namespace clang::tidy::energyplus

namespace clang::tidy {
namespace {
// Runs when clang-tidy dlopen()s this .so via --load=, registering this module
// into the (host process's) ClangTidyModuleRegistry.
ClangTidyModuleRegistry::Add<energyplus::EnergyPlusModule>
    X("energyplus-module", "Adds EnergyPlus-specific checks.");
} // namespace
} // namespace clang::tidy
