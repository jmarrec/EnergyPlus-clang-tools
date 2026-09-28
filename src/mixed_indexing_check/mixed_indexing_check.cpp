#include "mixed_indexing_check/MixedIndexingCheck.hpp"

#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;
using namespace clang::tooling;

namespace {

llvm::cl::OptionCategory ToolCategory("mixed-indexing-check options");

// See recurring_static_message_check.cpp's identical StripPch/makeAdjuster for the full
// rationale (PCH cross-compiler/staleness, -Werror, resource-dir).
llvm::cl::opt<bool> StripPch("strip-pch",
                              llvm::cl::desc("Drop -Xclang -include-pch/-include from the compile "
                                            "command instead of reusing it"),
                              llvm::cl::cat(ToolCategory));

llvm::cl::list<std::string> ExtraExcludeMembers(
    "exclude-member",
    llvm::cl::desc("Additional Type::member to exclude from cross-referencing (a std::vector "
                    "deliberately resize(N + 1)'d to support 1-based operator[], etc); may be "
                    "repeated. Appends to the built-in defaults below."),
    llvm::cl::cat(ToolCategory));

std::vector<mixed_indexing_check::MemberExclusion> parseExclusions() {
  std::vector<mixed_indexing_check::MemberExclusion> Exclusions = mixed_indexing_check::defaultExclusions();
  for (const std::string &S : ExtraExcludeMembers) {
    size_t Pos = S.find("::");
    if (Pos == std::string::npos) {
      llvm::errs() << "warning: ignoring malformed --exclude-member (expected Type::member): " << S << "\n";
      continue;
    }
    Exclusions.push_back({S.substr(0, Pos), S.substr(Pos + 2)});
  }
  return Exclusions;
}

ArgumentsAdjuster makeAdjuster(StringRef ResourceDir) {
  return [ResourceDir = ResourceDir.str()](const CommandLineArguments &Args, StringRef /*Filename*/) {
    CommandLineArguments Result;
    for (size_t I = 0; I < Args.size(); ++I) {
      if (Args[I] == "-Werror") {
        continue;
      }
      if (StripPch && Args[I] == "-Xclang" && I + 1 < Args.size()) {
        ++I;
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

  mixed_indexing_check::Callback Callback(parseExclusions());
  clang::ast_matchers::MatchFinder Finder;
  Finder.addMatcher(mixed_indexing_check::makeMatcher(), &Callback);

  return Tool.run(newFrontendActionFactory(&Finder).get());
}
