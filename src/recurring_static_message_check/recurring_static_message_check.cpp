#include "RecurringStaticMessageCheck.hpp"

#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;
using namespace clang::tooling;

namespace {

llvm::cl::OptionCategory ToolCategory("recurring-static-message-check options");

llvm::cl::list<std::string> RecurringFunctionNames(
    "recurring-function",
    llvm::cl::desc("Qualified name of a recurring-error function to check; may be repeated "
                    "(default: ShowRecurringWarningErrorAtEnd, ShowRecurringSevereErrorAtEnd, "
                    "ShowRecurringContinueErrorAtEnd)"),
    llvm::cl::cat(ToolCategory));

// Keeps the compile_commands.json's "-Xclang -include-pch <path>" / "-Xclang -include <path>"
// pairs by default (off): a PCH only loads if it was built by the exact same compiler
// invocation and is unchanged since. When that holds -- eg a build tree you know is
// up to date -- reusing it skips reparsing EnergyPlus's headers on every single file, which
// otherwise dominates this tool's runtime. When it doesn't hold, parsing hard-fails with a
// clear "modified since the precompiled header ... was built" diagnostic; rerun with this on.
llvm::cl::opt<bool> StripPch(
    "strip-pch",
    llvm::cl::desc("Drop -Xclang -include-pch/-include from the compile command instead of "
                    "reusing it (needed if the PCH wasn't built by this exact compiler, or is "
                    "stale relative to the checked-out headers)"),
    llvm::cl::cat(ToolCategory));

// Drops "-Werror" (the project's warnings shouldn't become hard parse failures for this
// tool) and, when `StripPch`, the PCH-forcing flags above. When a valid PCH is kept, the
// frontend never needs to physically locate builtin headers like stdarg.h -- their
// declarations are already serialized inside it -- so no resource-dir is needed either.
// Stripping the PCH forces a full from-scratch reparse, which does need one: ClangTool
// invokes the frontend library directly (not the `clang` driver binary), so it doesn't
// infer a resource-dir from this executable's own install location the way the driver
// would; pass it explicitly in that case.
ArgumentsAdjuster makeAdjuster(StringRef ResourceDir) {
  return [ResourceDir = ResourceDir.str()](const CommandLineArguments &Args, StringRef /*Filename*/) {
    CommandLineArguments Result;
    for (size_t I = 0; I < Args.size(); ++I) {
      if (Args[I] == "-Werror") {
        continue;
      }
      if (StripPch && Args[I] == "-Xclang" && I + 1 < Args.size()) {
        ++I; // also skip the paired value
        continue;
      }
      Result.push_back(Args[I]);
    }
    if (StripPch) {
      Result.push_back("-resource-dir=" + ResourceDir);
    }
    return Result;
  };
}

} // namespace

int main(int argc, const char **argv) {
  auto OptionsParser = CommonOptionsParser::create(argc, argv, ToolCategory);
  if (!OptionsParser) {
    llvm::errs() << llvm::toString(OptionsParser.takeError()) << "\n";
    return 1;
  }

  ClangTool Tool(OptionsParser->getCompilations(), OptionsParser->getSourcePathList());
  Tool.appendArgumentsAdjuster(makeAdjuster(CLANG_RESOURCE_DIR));

  std::vector<std::string> Names(RecurringFunctionNames.begin(), RecurringFunctionNames.end());
  if (Names.empty()) {
    Names = {"ShowRecurringWarningErrorAtEnd", "ShowRecurringSevereErrorAtEnd",
              "ShowRecurringContinueErrorAtEnd"};
  }

  recurring_static_message_check::Callback Callback;
  clang::ast_matchers::MatchFinder Finder;
  Finder.addMatcher(recurring_static_message_check::makeMatcher(Names), &Callback);

  int Ret = Tool.run(newFrontendActionFactory(&Finder).get());
  if (Ret != 0) {
    return Ret;
  }
  return Callback.foundAny() ? 1 : 0;
}
