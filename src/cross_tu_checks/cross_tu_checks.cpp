// cross_tu_checks: checks that can only decide after seeing every TU of a
// compilation database (so they can't be clang-tidy checks, which see one TU
// at a time):
//   - energyplus-unused-member: data members that are never referenced
//   - energyplus-write-only-member: data members written but never read
//
// All checks run in a single AllTUsToolExecutor pass: parsing each TU
// dominates the cost, so each one is parsed once and handed to every check's
// visitor via a MultiplexConsumer. The unused and write-only visitors stay
// separate because they deliberately disagree on what counts as a
// "reference" (e.g. an in-class initializer is a write for write-only, but not
// a use for unused).

#include "cross_tu_checks/CrossTuCommon.hpp"
#include "cross_tu_checks/UnusedMemberCheck.hpp"
#include "cross_tu_checks/WriteOnlyMemberCheck.hpp"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Tooling/AllTUsExecution.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/DiagnosticsYaml.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cross_tu_checks;

namespace {

struct Options {
  fs::path buildDir;
  fs::path repositoryRoot;
  fs::path sourceRoot;
  std::optional<fs::path> exportFixes;
  std::vector<fs::path> scanRoots;
  unsigned jobs = 1;
  bool keepPch = true;
  std::optional<std::regex> fileRegex;
  std::optional<std::regex> nameRegex;
};

void printUsage(llvm::raw_ostream &out, const char *program) {
  out << "Usage: " << program << " -p <build-dir> [options]\n"
      << "\n"
      << "Report data members that are never referenced, or written but never\n"
      << "read, in the selected Clang compilation database.\n"
      << "\n"
      << "Options:\n"
      << "  -p, --build-dir <path>    Directory containing "
         "compile_commands.json\n"
      << "  --source-root <path>      Root containing declarations to report\n"
      << "  --scan-root <path>        Root containing TUs to scan "
         "(repeatable)\n"
      << "  -j, --jobs <count>        Number of parallel Clang jobs (default: "
         "1)\n"
      << "  --no-pch                  Strip CMake's precompiled header (when "
         "it\n"
      << "                            was built by a different Clang version)\n"
      << "  --file-regex <regex>      Filter findings by repository-relative "
         "path\n"
      << "  --name-regex <regex>      Filter findings by qualified field name\n"
      << "  --export-fixes <file>     Write findings as a clang-tidy "
         "export-fixes YAML\n"
      << "  -h, --help                Show this help\n";
}

bool parseUnsigned(std::string_view value, unsigned &result) {
  if (value.empty()) {
    return false;
  }
  const auto conversion =
      std::from_chars(value.data(), value.data() + value.size(), result);
  return conversion.ec == std::errc{} &&
         conversion.ptr == value.data() + value.size() && result > 0;
}

std::optional<Options> parseOptions(int argc, const char **argv) {
  Options options;
  std::vector<std::string> scanRootArguments;
  std::optional<std::string> sourceRootArgument;

  auto requireValue = [&](int &index, std::string_view option) -> const char * {
    if (++index >= argc) {
      llvm::errs() << "error: " << option << " requires a value\n";
      return nullptr;
    }
    return argv[index];
  };

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "-p" || argument == "--build-dir") {
      const char *value = requireValue(index, argument);
      if (!value) {
        return std::nullopt;
      }
      options.buildDir = value;
    } else if (argument == "--source-root") {
      const char *value = requireValue(index, argument);
      if (!value) {
        return std::nullopt;
      }
      sourceRootArgument = value;
    } else if (argument == "--export-fixes") {
      const char *value = requireValue(index, argument);
      if (!value) {
        return std::nullopt;
      }
      options.exportFixes = fs::path(value);
    } else if (argument == "--scan-root") {
      const char *value = requireValue(index, argument);
      if (!value) {
        return std::nullopt;
      }
      scanRootArguments.emplace_back(value);
    } else if (argument == "-j" || argument == "--jobs") {
      const char *value = requireValue(index, argument);
      if (!value || !parseUnsigned(value, options.jobs)) {
        llvm::errs() << "error: " << argument
                     << " must be a positive integer\n";
        return std::nullopt;
      }
    } else if (argument == "--no-pch") {
      options.keepPch = false;
    } else if (argument == "--file-regex" || argument == "--name-regex") {
      const char *value = requireValue(index, argument);
      if (!value) {
        return std::nullopt;
      }
      try {
        std::regex expression(value, std::regex::ECMAScript);
        if (argument == "--file-regex") {
          options.fileRegex = std::move(expression);
        } else {
          options.nameRegex = std::move(expression);
        }
      } catch (const std::regex_error &error) {
        llvm::errs() << "error: invalid " << argument << ": " << error.what()
                     << '\n';
        return std::nullopt;
      }
    } else {
      llvm::errs() << "error: unknown option: " << argument << '\n';
      return std::nullopt;
    }
  }

  if (options.buildDir.empty()) {
    llvm::errs() << "error: -p/--build-dir is required\n";
    return std::nullopt;
  }

  options.buildDir = normalized(options.buildDir);
  options.repositoryRoot = normalized(options.buildDir / "..");
  options.sourceRoot =
      sourceRootArgument
          ? normalizedArgument(*sourceRootArgument, options.repositoryRoot)
          : normalized("src/EnergyPlus", options.repositoryRoot);
  if (scanRootArguments.empty()) {
    options.scanRoots.push_back(options.sourceRoot);
  } else {
    for (const auto &root : scanRootArguments) {
      options.scanRoots.push_back(
          normalizedArgument(root, options.repositoryRoot));
    }
  }
  return options;
}

class CrossTuAction final : public clang::ASTFrontendAction {
public:
  CrossTuAction(UnusedMemberFindings &unused,
                WriteOnlyMemberFindings &writeOnly)
      : unused_(unused), writeOnly_(writeOnly) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef) override {
    std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
    consumers.push_back(makeUnusedMemberConsumer(unused_, compiler));
    consumers.push_back(makeWriteOnlyMemberConsumer(writeOnly_, compiler));
    return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
  }

private:
  UnusedMemberFindings &unused_;
  WriteOnlyMemberFindings &writeOnly_;
};

class CrossTuActionFactory final
    : public clang::tooling::FrontendActionFactory {
public:
  CrossTuActionFactory(UnusedMemberFindings &unused,
                       WriteOnlyMemberFindings &writeOnly)
      : unused_(unused), writeOnly_(writeOnly) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<CrossTuAction>(unused_, writeOnly_);
  }

private:
  UnusedMemberFindings &unused_;
  WriteOnlyMemberFindings &writeOnly_;
};

bool matches(const std::optional<std::regex> &expression,
             const std::string &value) {
  return !expression || std::regex_search(value, *expression);
}

} // namespace

int main(int argc, const char **argv) {
  const bool helpRequested =
      std::any_of(argv + 1, argv + argc, [](const char *argument) {
        return std::string_view(argument) == "-h" ||
               std::string_view(argument) == "--help";
      });
  if (helpRequested) {
    printUsage(llvm::outs(), argv[0]);
    return 0;
  }
  const auto options = parseOptions(argc, argv);
  if (!options) {
    return 2;
  }

  std::string databaseError;
  auto database = clang::tooling::CompilationDatabase::loadFromDirectory(
      options->buildDir.string(), databaseError);
  if (!database) {
    llvm::errs() << "error: could not load compilation database from "
                 << options->buildDir.string() << ": " << databaseError << '\n';
    return 2;
  }

  FilteredCompilationDatabase filteredDatabase(*database, options->scanRoots,
                                               options->keepPch);
  if (filteredDatabase.size() == 0) {
    llvm::errs()
        << "error: no compile commands matched the selected scan roots\n";
    return 2;
  }

  llvm::errs() << "Scanning " << filteredDatabase.size()
               << " translation units with " << options->jobs << " jobs\n";
  UnusedMemberFindings unused(options->sourceRoot);
  WriteOnlyMemberFindings writeOnly(options->sourceRoot);
  clang::tooling::AllTUsToolExecutor executor(filteredDatabase, options->jobs);
  if (llvm::Error error = executor.execute(
          std::make_unique<CrossTuActionFactory>(unused, writeOnly))) {
    llvm::errs() << "error: Clang analysis failed: "
                 << llvm::toString(std::move(error)) << '\n';
    return 2;
  }

  std::vector<Finding> findings = unused.findings();
  std::vector<Finding> writeOnlyFindings = writeOnly.findings();
  findings.insert(findings.end(),
                  std::make_move_iterator(writeOnlyFindings.begin()),
                  std::make_move_iterator(writeOnlyFindings.end()));
  std::stable_sort(findings.begin(), findings.end(),
                   [](const Finding &left, const Finding &right) {
                     return locationLess(left.field, right.field);
                   });

  clang::tooling::TranslationUnitDiagnostics exported;
  exported.MainSourceFile = options->sourceRoot.string();
  std::map<std::string, std::size_t> foundByCheck;
  std::size_t displayed = 0;
  for (const auto &[field, check, message] : findings) {
    ++foundByCheck[check];
    std::error_code error;
    fs::path displayPath =
        fs::relative(field.file, options->repositoryRoot, error);
    if (error) {
      displayPath = field.file;
    }
    const std::string displayPathString = displayPath.generic_string();
    if (!matches(options->fileRegex, displayPathString) ||
        !matches(options->nameRegex, field.qualifiedName)) {
      continue;
    }
    llvm::outs() << displayPathString << ':' << field.line << ':'
                 << field.column << ": warning: " << message << " [" << check
                 << "]\n";
    ++displayed;

    clang::tooling::Diagnostic diagnostic(
        check, clang::tooling::Diagnostic::Warning, options->buildDir.string());
    diagnostic.Message.Message = message;
    diagnostic.Message.FilePath = field.file.string();
    diagnostic.Message.FileOffset = field.fileOffset;
    exported.Diagnostics.push_back(std::move(diagnostic));
  }

  if (options->exportFixes) {
    std::error_code error;
    llvm::raw_fd_ostream out(options->exportFixes->string(), error);
    if (error) {
      llvm::errs() << "error: could not write "
                   << options->exportFixes->string() << ": " << error.message()
                   << '\n';
      return 2;
    }
    llvm::yaml::Output yaml(out);
    yaml << exported;
  }

  llvm::errs() << "Found ";
  const char *separator = "";
  for (const auto &[check, count] : foundByCheck) {
    llvm::errs() << separator << count << ' ' << check;
    separator = ", ";
  }
  if (foundByCheck.empty()) {
    llvm::errs() << "nothing";
  }
  llvm::errs() << "; displayed " << displayed << " after filters\n"
               << "Treat findings as candidates and validate them against "
                  "other build configurations.\n";
  return displayed > 0 ? 1 : 0;
}
