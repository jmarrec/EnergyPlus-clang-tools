#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
// Moved out of clang/Index in LLVM 23 (same clang::index namespace).
#if __has_include("clang/UnifiedSymbolResolution/USRGeneration.h")
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#else
#include "clang/Index/USRGeneration.h"
#endif
#include "clang/Tooling/AllTUsExecution.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
  fs::path buildDir;
  fs::path sourceRoot;
  std::vector<fs::path> scanRoots;
  unsigned jobs = 1;
  std::optional<std::regex> fileRegex;
  std::optional<std::regex> nameRegex;
};

void printUsage(llvm::raw_ostream &out, const char *program) {
  out << "Usage: " << program << " -p <build-dir> [options]\n"
      << "\n"
      << "Report data members with no semantic references in the selected\n"
      << "Clang compilation database.\n"
      << "\n"
      << "Options:\n"
      << "  -p, --build-dir <path>    Directory containing "
         "compile_commands.json\n"
      << "  --source-root <path>      Root containing declarations to report\n"
      << "  --scan-root <path>        Root containing TUs to scan "
         "(repeatable)\n"
      << "  -j, --jobs <count>        Number of parallel Clang jobs (default: "
         "1)\n"
      << "  --file-regex <regex>      Filter findings by source-relative path\n"
      << "  --name-regex <regex>      Filter findings by qualified field name\n"
      << "  -h, --help                Show this help\n";
}

fs::path normalized(const fs::path &path, const fs::path &base = {}) {
  std::error_code error;
  fs::path absolutePath = path;
  if (absolutePath.is_relative()) {
    absolutePath =
        (base.empty() ? fs::current_path(error) : base) / absolutePath;
  }
  auto canonicalPath = fs::weakly_canonical(absolutePath, error);
  return error ? absolutePath.lexically_normal() : canonicalPath;
}

bool isWithin(const fs::path &path, const fs::path &root) {
  auto pathIt = path.begin();
  auto rootIt = root.begin();
  for (; rootIt != root.end(); ++rootIt, ++pathIt) {
    if (pathIt == path.end() || *pathIt != *rootIt) {
      return false;
    }
  }
  return true;
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
  const fs::path repositoryRoot = normalized(options.buildDir / "..");
  options.sourceRoot =
      normalized(sourceRootArgument.value_or("src/EnergyPlus"), repositoryRoot);
  if (scanRootArguments.empty()) {
    options.scanRoots.push_back(options.sourceRoot);
  } else {
    for (const auto &root : scanRootArguments) {
      options.scanRoots.push_back(normalized(root, repositoryRoot));
    }
  }
  return options;
}

class FilteredCompilationDatabase final
    : public clang::tooling::CompilationDatabase {
public:
  FilteredCompilationDatabase(
      const clang::tooling::CompilationDatabase &database,
      const std::vector<fs::path> &scanRoots) {
    for (const auto &command : database.getAllCompileCommands()) {
      const fs::path file = normalized(command.Filename, command.Directory);
      if (std::any_of(
              scanRoots.begin(), scanRoots.end(),
              [&](const fs::path &root) { return isWithin(file, root); })) {
        // CMake's PCH support adds a synthetic TU that compiles cmake_pch.hxx
        // itself (to generate the .pch/.gch); it has no real declarations to
        // report and would just waste a parse if a --scan-root ever widens
        // enough to catch it.
        if (file.filename().string().rfind("cmake_pch", 0) == 0) {
          continue;
        }
        commands_.push_back(command);
        commandsByFile_.emplace(file.string(), command);
      }
    }
  }

  std::vector<clang::tooling::CompileCommand>
  getCompileCommands(llvm::StringRef filePath) const override {
    std::vector<clang::tooling::CompileCommand> result;
    const std::string key = normalized(filePath.str()).string();
    const auto range = commandsByFile_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
      result.push_back(it->second);
    }
    return result;
  }

  std::vector<std::string> getAllFiles() const override {
    std::vector<std::string> files;
    files.reserve(commands_.size());
    for (const auto &command : commands_) {
      files.push_back(normalized(command.Filename, command.Directory).string());
    }
    return files;
  }

  std::vector<clang::tooling::CompileCommand>
  getAllCompileCommands() const override {
    return commands_;
  }

  std::size_t size() const { return commands_.size(); }

private:
  std::vector<clang::tooling::CompileCommand> commands_;
  std::unordered_multimap<std::string, clang::tooling::CompileCommand>
      commandsByFile_;
};

std::string usrFor(const clang::Decl *declaration) {
  llvm::SmallString<256> buffer;
  if (!declaration || clang::index::generateUSRForDecl(
                          declaration->getCanonicalDecl(), buffer)) {
    return {};
  }
  return std::string(buffer);
}

struct FieldInfo {
  std::string usr;
  std::string name;
  std::string qualifiedName;
  std::string type;
  fs::path file;
  unsigned line = 0;
  unsigned column = 0;
};

class Findings {
public:
  explicit Findings(fs::path sourceRoot) : sourceRoot_(std::move(sourceRoot)) {}

  void recordDeclaration(const clang::FieldDecl *field,
                         const clang::SourceManager &sourceManager) {
    if (field->isImplicit()) {
      return;
    }

    const clang::SourceLocation location =
        sourceManager.getExpansionLoc(field->getLocation());
    if (location.isInvalid()) {
      return;
    }
    const llvm::StringRef filename = sourceManager.getFilename(location);
    if (filename.empty()) {
      return;
    }
    const fs::path file = normalized(filename.str());
    if (!isWithin(file, sourceRoot_)) {
      return;
    }

    std::string usr = usrFor(field);
    if (usr.empty()) {
      return;
    }

    const clang::PresumedLoc presumed = sourceManager.getPresumedLoc(location);
    FieldInfo info{usr,
                   field->getNameAsString(),
                   field->getQualifiedNameAsString(),
                   field->getType().getAsString(),
                   file,
                   presumed.isValid() ? presumed.getLine() : 0,
                   presumed.isValid() ? presumed.getColumn() : 0};
    std::lock_guard<std::mutex> lock(mutex_);
    declarations_.try_emplace(usr, std::move(info));
  }

  void recordReference(const clang::FieldDecl *field) {
    std::string usr = usrFor(field);
    if (usr.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    references_.insert(std::move(usr));
  }

  // Template-dependent member accesses (this->member or Base::member inside a
  // template, before instantiation) can't be resolved to a concrete FieldDecl,
  // so recordReference() never sees them. Track the plain name instead and
  // treat any by-name match as "used" -- conservative, but avoids flagging
  // members that dependent lookup just couldn't confirm.
  void recordUnresolvedReference(clang::DeclarationName name) {
    if (name.isEmpty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    unresolvedReferenceNames_.insert(name.getAsString());
  }

  // Aggregate init (Foo f{1, 2, 3}), C++20 paren-list init, and structured
  // bindings all reference every member positionally without ever naming it via
  // a MemberExpr, so the visitor would otherwise never see those members as
  // referenced.
  void recordAggregateUse(clang::QualType type) {
    type = type.getNonReferenceType();
    const auto *record = type->getAsRecordDecl();
    if (!record) {
      return;
    }
    for (const auto *field : record->fields()) {
      recordReference(field);
    }
    if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
      for (const auto &base : cxxRecord->bases()) {
        recordAggregateUse(base.getType());
      }
    }
  }

  std::vector<FieldInfo> unusedFields() const {
    std::vector<FieldInfo> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &[usr, field] : declarations_) {
      if (references_.find(usr) == references_.end() &&
          unresolvedReferenceNames_.find(field.name) ==
              unresolvedReferenceNames_.end()) {
        result.push_back(field);
      }
    }
    std::sort(result.begin(), result.end(),
              [](const FieldInfo &left, const FieldInfo &right) {
                if (left.file != right.file) {
                  return left.file < right.file;
                }
                if (left.line != right.line) {
                  return left.line < right.line;
                }
                return left.column < right.column;
              });
    return result;
  }

  const fs::path &sourceRoot() const { return sourceRoot_; }

private:
  fs::path sourceRoot_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, FieldInfo> declarations_;
  std::unordered_set<std::string> references_;
  std::unordered_set<std::string> unresolvedReferenceNames_;
};

class MemberVisitor final : public clang::RecursiveASTVisitor<MemberVisitor> {
public:
  MemberVisitor(Findings &findings, clang::SourceManager &sourceManager)
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
        MemberVisitor>::TraverseConstructorInitializer(initializer);
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

  Findings &findings_;
  clang::SourceManager &sourceManager_;
};

class MemberConsumer final : public clang::ASTConsumer {
public:
  MemberConsumer(Findings &findings, clang::SourceManager &sourceManager)
      : visitor_(findings, sourceManager) {}

  void HandleTranslationUnit(clang::ASTContext &context) override {
    visitor_.TraverseDecl(context.getTranslationUnitDecl());
  }

private:
  MemberVisitor visitor_;
};

class MemberAction final : public clang::ASTFrontendAction {
public:
  explicit MemberAction(Findings &findings) : findings_(findings) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef) override {
    return std::make_unique<MemberConsumer>(findings_,
                                            compiler.getSourceManager());
  }

private:
  Findings &findings_;
};

class MemberActionFactory final : public clang::tooling::FrontendActionFactory {
public:
  explicit MemberActionFactory(Findings &findings) : findings_(findings) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<MemberAction>(findings_);
  }

private:
  Findings &findings_;
};

bool matches(const std::optional<std::regex> &expression,
             const std::string &value) {
  return !expression || std::regex_search(value, *expression);
}

// The compile database's compile commands were generated for GCC: -Werror
// paired with GCC-only -Wno-* flags Clang doesn't recognize (e.g.
// -Wno-dangling-reference, -Wno-stringop-overflow) turns "unknown warning
// option" into a hard error, and the same -Werror turns "precompiled header
// ...cmake_pch.hxx.gch was ignored because it is not a clang PCH file" into
// one too (GCC's .gch format isn't Clang's). Dropping -Werror downgrades
// both back to non-fatal warnings -- the GCC PCH then just gets ignored and
// cmake_pch.hxx reparsed as a normal header, slower but correct.
clang::tooling::ArgumentsAdjuster makeArgumentsAdjuster() {
  return [](const clang::tooling::CommandLineArguments &args,
            llvm::StringRef /*filename*/) {
    clang::tooling::CommandLineArguments result;
    for (const std::string &arg : args) {
      if (arg != "-Werror") {
        result.push_back(arg);
      }
    }
    return result;
  };
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

  FilteredCompilationDatabase filteredDatabase(*database, options->scanRoots);
  if (filteredDatabase.size() == 0) {
    llvm::errs()
        << "error: no compile commands matched the selected scan roots\n";
    return 2;
  }

  llvm::errs() << "Scanning " << filteredDatabase.size()
               << " translation units with " << options->jobs << " jobs\n";
  Findings findings(options->sourceRoot);
  clang::tooling::AllTUsToolExecutor executor(filteredDatabase, options->jobs);
  std::vector<std::pair<std::unique_ptr<clang::tooling::FrontendActionFactory>,
                        clang::tooling::ArgumentsAdjuster>>
      actions;
  actions.emplace_back(std::make_unique<MemberActionFactory>(findings),
                       makeArgumentsAdjuster());
  if (llvm::Error error = executor.execute(actions)) {
    llvm::errs() << "error: Clang analysis failed: "
                 << llvm::toString(std::move(error)) << '\n';
    return 2;
  }

  const auto unusedFields = findings.unusedFields();
  std::size_t displayed = 0;
  for (const auto &field : unusedFields) {
    std::error_code error;
    fs::path displayPath = fs::relative(
        field.file, findings.sourceRoot().parent_path().parent_path(), error);
    if (error) {
      displayPath = field.file;
    }
    const std::string displayPathString = displayPath.generic_string();
    if (!matches(options->fileRegex, displayPathString) ||
        !matches(options->nameRegex, field.qualifiedName)) {
      continue;
    }
    llvm::outs() << displayPathString << ':' << field.line << ':'
                 << field.column << ": " << field.qualifiedName << " ["
                 << field.type << "]\n";
    ++displayed;
  }

  llvm::errs() << "Found " << unusedFields.size()
               << " unreferenced fields; displayed " << displayed
               << " after filters\n"
               << "Treat findings as candidates and validate them against "
                  "other build configurations.\n";
  return displayed > 0 ? 1 : 0;
}
