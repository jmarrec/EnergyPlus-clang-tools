#include "path_format_fixer/PathFormatFixer.hpp"

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

using namespace clang;
using namespace clang::tooling;

namespace {

llvm::cl::OptionCategory ToolCategory("path-format-fixer options");

llvm::cl::list<std::string> FormatFunctionNames(
    "format-function",
    llvm::cl::desc("Qualified name of a format function to match and normalize onto std::format; "
                   "may be repeated (default: EnergyPlus::format, fmt::format, std::format)"),
    llvm::cl::cat(ToolCategory));

} // namespace

int main(int argc, const char **argv) {
  auto OptionsParser = CommonOptionsParser::create(argc, argv, ToolCategory);
  if (!OptionsParser) {
    llvm::errs() << llvm::toString(OptionsParser.takeError()) << "\n";
    return 1;
  }

  RefactoringTool Tool(OptionsParser->getCompilations(), OptionsParser->getSourcePathList());

  std::vector<std::string> Names(FormatFunctionNames.begin(), FormatFunctionNames.end());
  if (Names.empty()) {
    Names = {"EnergyPlus::format", "fmt::format", "std::format"};
  }

  path_format_fixer::PathFormatCallback Callback(Tool.getReplacements());
  ast_matchers::MatchFinder Finder;
  Finder.addMatcher(path_format_fixer::makeMatcher(Names), &Callback);

  return Tool.runAndSave(newFrontendActionFactory(&Finder).get());
}
