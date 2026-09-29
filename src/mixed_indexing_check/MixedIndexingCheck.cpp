#include "mixed_indexing_check/MixedIndexingCheck.hpp"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"

using namespace clang;
using namespace clang::ast_matchers;

namespace mixed_indexing_check {

namespace {

enum class Kind : uint8_t { Call, Subscript };

struct Use {
  Kind K;
  SourceLocation Loc;
};

// Walks one function body, recording every index-position use of every variable
// as either a Call ("operator()", the ObjexxFCL Array1D/EPVector 1-indexed
// convention in this codebase) or a Subscript ("operator[]"/raw array indexing,
// the std::vector/std::array/ std::span 0-indexed convention) -- then, once the
// whole body has been seen, flags every variable that shows up as both. Only
// bare `identifier` and `identifier - literal`/ `identifier + literal` index
// expressions are tracked (an index buried in a larger expression, e.g. `foo(a
// + b)`, isn't attributable to a single variable, so it's ignored rather than
// guessed at).
class Visitor : public RecursiveASTVisitor<Visitor> {
public:
  explicit Visitor(const std::vector<MemberExclusion> &Exclusions)
      : Exclusions(Exclusions) {}

  const llvm::DenseMap<const VarDecl *, llvm::SmallVector<Use, 4>> &
  usesByVar() const {
    return UsesByVar;
  }

  bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *E) {
    // Exactly 2 args (object + one index): the genuine
    // ObjexxFCL::Array1D/EPVector single-index accessor this check's
    // "operator() == 1-indexed" assumption is actually about. A multi-arg
    // operator() (eg a 3D grid's cells(X, Y, Z)) is a different beast -- common
    // specifically because operator[] couldn't take multiple arguments before
    // C++23, so it carries no such guarantee; PlantPipingSystemsManager.cc's
    // `cells(X, Y, Z)` is explicitly 0-based by its own neighboring "//'zero
    // based index" comment.
    if (E->getOperator() == OO_Call && E->getNumArgs() == 2) {
      if (!isExcluded(E->getArg(0))) {
        record(E->getArg(1), Kind::Call, E->getOperatorLoc());
      }
    } else if (E->getOperator() == OO_Subscript && E->getNumArgs() >= 2) {
      if (!isExcluded(E->getArg(0)) && !isAssociativeContainer(E->getArg(0))) {
        record(E->getArg(1), Kind::Subscript, E->getOperatorLoc());
      }
    }
    return true;
  }

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *E) {
    if (!isExcluded(E->getBase())) {
      record(E->getIdx(), Kind::Subscript, E->getRBracketLoc());
    }
    return true;
  }

private:
  const std::vector<MemberExclusion> &Exclusions;
  llvm::DenseMap<const VarDecl *, llvm::SmallVector<Use, 4>> UsesByVar;

  // True if `BaseObj`'s type is a std::map/unordered_map (or multi- variant):
  // operator[] on those does *keyed* lookup, not positional indexing -- there's
  // no 0-vs-1-indexed convention at all (the key can be any int, including a
  // 1-based one on purpose), so it must never count as evidence of "0-indexed
  // convention" the way a real operator[] on a std::vector/std::array/raw array
  // does.
  bool isAssociativeContainer(const Expr *BaseObj) const {
    QualType QT = BaseObj->getType();
    if (QT->isPointerType() || QT->isReferenceType()) {
      QT = QT->getPointeeType();
    }
    const auto *RD = QT->getAsCXXRecordDecl();
    if (!RD || !RD->getDeclContext()->isStdNamespace()) {
      return false;
    }
    llvm::StringRef Name = RD->getName();
    return Name == "map" || Name == "unordered_map" || Name == "multimap" ||
           Name == "unordered_multimap";
  }

  // True if `BaseObj` (the object being called/subscripted, e.g.
  // `thisOutsideAirSys.compPointer` in
  // `thisOutsideAirSys.compPointer[CompNum]`) is a direct `Type::member` access
  // matching one of `Exclusions`. Matched against the FieldDecl's immediately
  // declaring RecordDecl, so a derived type inheriting the member doesn't
  // accidentally also match a base-type exclusion (or vice versa) unless it's
  // the type the member is actually declared on.
  bool isExcluded(const Expr *BaseObj) const {
    const auto *ME = dyn_cast<MemberExpr>(BaseObj->IgnoreParenImpCasts());
    if (!ME) {
      return false;
    }
    const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
    if (!FD) {
      return false;
    }
    const RecordDecl *RD = FD->getParent();
    return llvm::any_of(Exclusions, [&](const MemberExclusion &Ex) {
      return Ex.TypeName == RD->getName() && Ex.MemberName == FD->getName();
    });
  }

  void record(const Expr *IndexExpr, Kind K, SourceLocation Loc) {
    const Expr *E = IndexExpr->IgnoreParenImpCasts();
    // An `i - 1` / `i + 1`-shaped adjustment means the author already accounted
    // for the convention mismatch at this specific use -- e.g. `coeff[in] =
    // Numbers(in + 1)`, a 0-based loop variable correctly bumped by one for the
    // 1-indexed ObjexxFCL array right next to its correct bare use on the
    // 0-indexed side. That's exactly a *non*-suspicious use, so it must be
    // dropped here, not normalized down to the bare identifier and recorded as
    // if unadjusted -- doing that would make every such (correct) pairing look
    // exactly like the co-mingled-convention pattern this check exists to
    // catch.
    if (const auto *BinOp = dyn_cast<BinaryOperator>(E)) {
      if (BinOp->isAdditiveOp()) {
        return;
      }
    }
    const auto *DRE = dyn_cast<DeclRefExpr>(E);
    if (!DRE) {
      return;
    }
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (!VD) {
      return;
    }
    UsesByVar[VD->getCanonicalDecl()].push_back({K, Loc});
  }
};

} // namespace

std::vector<MemberExclusion> defaultExclusions() {
  return {
      {"OutsideAirSysProps", "compPointer"},
      {"EquipList", "compPointer"},
      {"SurfaceWindowCalc", "thetaFace"},
      {"UnitarySys", "m_MSCoolingSpeedRatio"},
      {"UnitarySys", "m_MSHeatingSpeedRatio"},
      {"UnitarySys", "m_CoolVolumeFlowRate"},
      {"UnitarySys", "m_HeatVolumeFlowRate"},
      {"UnitarySys", "m_CoolMassFlowRate"},
      {"UnitarySys", "m_HeatMassFlowRate"},
      {"UnitarySys", "FullLatOutput"},
      {"UnitarySys", "FullOutput"},
      {"UnitarySys", "SpeedSHR"},
      {"BlindBmDf", "Bm"},
  };
}

DeclarationMatcher makeMatcher() {
  return functionDecl(hasBody(compoundStmt())).bind("func");
}

std::vector<FlaggedVar>
findFlaggedVars(const FunctionDecl &Func,
                const std::vector<MemberExclusion> &Exclusions) {
  if (!Func.hasBody() || !Func.isThisDeclarationADefinition()) {
    return {};
  }
  // Only the primary body owner (skip out-of-line redeclarations pointing at
  // the same body).
  if (Func.getBody()->getBeginLoc().isInvalid()) {
    return {};
  }

  Visitor V(Exclusions);
  V.TraverseStmt(Func.getBody());

  std::vector<FlaggedVar> Flagged;
  for (const auto &[VD, Uses] : V.usesByVar()) {
    bool SawCall = false;
    bool SawSubscript = false;
    for (const Use &U : Uses) {
      SawCall |= (U.K == Kind::Call);
      SawSubscript |= (U.K == Kind::Subscript);
    }
    if (!SawCall || !SawSubscript) {
      continue;
    }
    // Only the first use of each kind: a heavily-used loop variable can rack up
    // dozens otherwise, and one example of each is enough to act on.
    const auto *FirstCall =
        &*llvm::find_if(Uses, [](const Use &U) { return U.K == Kind::Call; });
    const auto *FirstSubscript = &*llvm::find_if(
        Uses, [](const Use &U) { return U.K == Kind::Subscript; });
    Flagged.push_back({VD->getName().str(), VD->getLocation(), FirstCall->Loc,
                       FirstSubscript->Loc});
  }
  return Flagged;
}

void Callback::run(const MatchFinder::MatchResult &Result) {
  const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func");
  if (!Func) {
    return;
  }

  DiagnosticsEngine &Diags = Result.Context->getDiagnostics();
  unsigned WarningID = Diags.getCustomDiagID(
      DiagnosticsEngine::Warning, "%0 is used both as operator() (1-indexed "
                                  "convention) and operator[] (0-indexed "
                                  "convention) in this function -- verify each "
                                  "use has the correct index base");
  unsigned CallNoteID =
      Diags.getCustomDiagID(DiagnosticsEngine::Note, "used here as operator()");
  unsigned SubscriptNoteID =
      Diags.getCustomDiagID(DiagnosticsEngine::Note, "used here as operator[]");

  for (const FlaggedVar &FV : findFlaggedVars(*Func, Exclusions)) {
    Diags.Report(FV.DeclLoc, WarningID) << FV.Name;
    Diags.Report(FV.FirstCallLoc, CallNoteID);
    Diags.Report(FV.FirstSubscriptLoc, SubscriptNoteID);
  }
}

} // namespace mixed_indexing_check
