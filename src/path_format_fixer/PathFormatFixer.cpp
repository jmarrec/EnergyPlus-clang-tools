#include "path_format_fixer/PathFormatFixer.hpp"

#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Tooling.h"

#include <cctype>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

namespace path_format_fixer {

std::optional<std::vector<Field>> scanReplacementFields(StringRef Text) {
  std::vector<Field> Fields;
  unsigned AutoIndex = 0;
  for (size_t I = 0; I < Text.size();) {
    char C = Text[I];
    if (C == '{') {
      if (I + 1 < Text.size() && Text[I + 1] == '{') {
        I += 2;
        continue;
      }
      size_t Start = I;
      size_t J = I + 1;
      while (J < Text.size() && Text[J] != '}') {
        ++J;
      }
      if (J >= Text.size()) {
        return std::nullopt;
      }

      std::string Inner = Text.substr(Start + 1, J - Start - 1).str();

      // Leading digits, if any, are an explicit argument index.
      size_t NumEnd = 0;
      while (NumEnd < Inner.size() && isdigit(static_cast<unsigned char>(Inner[NumEnd]))) {
        ++NumEnd;
      }

      int ArgIndex = 0;
      if (NumEnd > 0) {
        ArgIndex = std::stoi(Inner.substr(0, NumEnd));
      } else {
        ArgIndex = static_cast<int>(AutoIndex++);
      }

      Fields.push_back(Field{Start, J - Start + 1, std::move(Inner), ArgIndex});
      I = J + 1;
    } else if (C == '}') {
      if (I + 1 < Text.size() && Text[I + 1] == '}') {
        I += 2;
        continue;
      }
      ++I;
    } else {
      ++I;
    }
  }
  return Fields;
}

StatementMatcher makeMatcher(const std::vector<std::string> &FormatFunctionNames) {
  std::vector<StringRef> Names(FormatFunctionNames.begin(), FormatFunctionNames.end());

  // Match each path-to-string call independently (rather than matching the
  // format call and looking for *an* argument among its arguments): with
  // hasAnyArgument()/anyOf(), MatchFinder only ever produces one match per
  // format call, so a call passing multiple paths would only get its first
  // path argument rewritten. Anchoring the matcher on the path call itself,
  // with the format call found via hasAncestor(), gives one match per path
  // call instead.
  return cxxMemberCallExpr(
             callee(cxxMethodDecl(hasAnyName("string", "generic_string"))),
             on(hasType(cxxRecordDecl(hasName("::std::filesystem::path")))),
             hasAncestor(callExpr(callee(functionDecl(hasAnyName(Names)))).bind("formatCall")))
      .bind("pathCall");
}

void PathFormatCallback::run(const MatchFinder::MatchResult &Result) {
  const auto *FormatCall = Result.Nodes.getNodeAs<CallExpr>("formatCall");
  const auto *PathCall = Result.Nodes.getNodeAs<CXXMemberCallExpr>("pathCall");
  if (!FormatCall || !PathCall)
    return;

  const SourceManager &SM = *Result.SourceManager;
  const LangOptions &LangOpts = Result.Context->getLangOpts();

  if (FormatCall->getNumArgs() < 2) {
    return;
  }

  const Expr *FmtArg = FormatCall->getArg(0)->IgnoreUnlessSpelledInSource();
  const auto *FmtLiteral = dyn_cast<StringLiteral>(FmtArg);
  if (!FmtLiteral) {
    diag(SM, FormatCall->getExprLoc(), "format string is not a simple string literal");
    return;
  }
  if (FmtLiteral->getNumConcatenated() != 1) {
    diag(SM, FormatCall->getExprLoc(), "concatenated format string literal");
    return;
  }

  // Which variadic argument (0-based -> matches {} auto-numbering) holds PathCall?
  int ArgIndex = -1;
  SourceRange CallRange = PathCall->getSourceRange();
  for (unsigned I = 1; I < FormatCall->getNumArgs(); ++I) {
    SourceRange ArgRange = FormatCall->getArg(I)->getSourceRange();
    if (!SM.isBeforeInTranslationUnit(CallRange.getBegin(), ArgRange.getBegin()) &&
        !SM.isBeforeInTranslationUnit(ArgRange.getEnd(), CallRange.getEnd())) {
      ArgIndex = static_cast<int>(I - 1);
      break;
    }
  }
  if (ArgIndex < 0) {
    diag(SM, PathCall->getExprLoc(), "could not map path call to a format argument");
    return;
  }

  bool IsGeneric = PathCall->getMethodDecl()->getName() == "generic_string";

  CharSourceRange FmtTokenRange = CharSourceRange::getTokenRange(FmtLiteral->getSourceRange());
  StringRef FmtSourceText = Lexer::getSourceText(FmtTokenRange, SM, LangOpts);

  if (IsGeneric) {
    auto MaybeFields = scanReplacementFields(FmtSourceText);
    if (!MaybeFields) {
      diag(SM, FmtLiteral->getExprLoc(), "could not parse replacement fields in format string");
      return;
    }
    const Field *Found = nullptr;
    for (const Field &Candidate : *MaybeFields) {
      if (Candidate.ArgIndex == ArgIndex) {
        Found = &Candidate;
        break;
      }
    }
    if (!Found) {
      diag(SM, FmtLiteral->getExprLoc(),
           "could not find a replacement field referring to argument #" +
               std::to_string(ArgIndex) + " in format string");
      return;
    }
    const Field &F = *Found;
    if (F.Inner.find(':') != std::string::npos) {
      diag(SM, FmtLiteral->getExprLoc(),
           "replacement field #" + std::to_string(ArgIndex) + " already has a format spec ('" +
               F.Inner + "')");
      return;
    }
    std::string NewField = "{" + F.Inner + ":g}";
    SourceLocation FieldStart = FmtLiteral->getBeginLoc().getLocWithOffset(F.Offset);
    addReplacement(SM,
                   CharSourceRange::getCharRange(FieldStart, FieldStart.getLocWithOffset(F.Length)),
                   NewField);
  }

  // <expr>.string() / <expr>.generic_string()  ->  <expr>
  const Expr *Base = PathCall->getImplicitObjectArgument()->IgnoreParenImpCasts();
  StringRef BaseText =
      Lexer::getSourceText(CharSourceRange::getTokenRange(Base->getSourceRange()), SM, LangOpts);

  addReplacement(SM, CharSourceRange::getTokenRange(PathCall->getSourceRange()), BaseText);

  // Normalize the call itself onto std::format (EnergyPlus::format, fmt::format, ...).
  const FunctionDecl *Callee = FormatCall->getDirectCallee();
  if ((Callee != nullptr) && Callee->getQualifiedNameAsString() != "std::format" &&
      RenamedCalls.insert(FormatCall).second) {
    const Expr *CalleeExpr = FormatCall->getCallee()->IgnoreParenImpCasts();
    addReplacement(SM, CharSourceRange::getTokenRange(CalleeExpr->getSourceRange()), "std::format");
  }
}

void PathFormatCallback::diag(const SourceManager &SM, SourceLocation Loc, const std::string &Msg) {
  llvm::errs() << "skip: " << Msg << " at " << Loc.printToString(SM) << "\n";
}

void PathFormatCallback::addReplacement(const SourceManager &SM, CharSourceRange Range,
                                        StringRef NewText) {
  Replacement Repl(SM, Range, NewText);
  if (llvm::Error Err = FileToReplaces[std::string(Repl.getFilePath())].add(Repl)) {
    llvm::errs() << "error adding replacement: " << llvm::toString(std::move(Err)) << "\n";
  }
}

std::string runOnCode(StringRef Code, const std::vector<std::string> &FormatFunctionNames) {
  std::map<std::string, Replacements> FileToReplaces;
  PathFormatCallback Callback(FileToReplaces);

  MatchFinder Finder;
  Finder.addMatcher(makeMatcher(FormatFunctionNames), &Callback);

  std::string FileName = "input.cc";
  bool Ran = runToolOnCodeWithArgs(newFrontendActionFactory(&Finder)->create(), Code,
                                   {"-std=c++20"}, FileName);
  if (!Ran || FileToReplaces.empty()) {
    return Code.str();
  }

  auto It = FileToReplaces.find(FileName);
  if (It == FileToReplaces.end()) {
    It = FileToReplaces.begin();
  }

  llvm::Expected<std::string> Result = applyAllReplacements(Code, It->second);
  if (!Result) {
    llvm::consumeError(Result.takeError());
    return Code.str();
  }
  return *Result;
}

} // namespace path_format_fixer
