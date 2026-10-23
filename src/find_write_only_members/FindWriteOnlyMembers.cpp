#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
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
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/DiagnosticsYaml.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iterator>
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
  fs::path repositoryRoot;
  fs::path sourceRoot;
  std::optional<fs::path> exportFixes;
  std::vector<fs::path> scanRoots;
  unsigned jobs = 1;
  std::optional<std::regex> fileRegex;
  std::optional<std::regex> nameRegex;
};

void printUsage(llvm::raw_ostream &out, const char *program) {
  out << "Usage: " << program << " -p <build-dir> [options]\n"
      << "\n"
      << "Report non-static data members that are written but have no reads "
         "in\n"
      << "the selected Clang compilation database.\n"
      << "\n"
      << "Options:\n"
      << "  -p, --build-dir <path>    Directory containing "
         "compile_commands.json\n"
      << "  --source-root <path>      Root containing declarations to report\n"
      << "  --scan-root <path>        Root containing TUs to scan "
         "(repeatable)\n"
      << "  -j, --jobs <count>        Number of parallel Clang jobs (default: "
         "1)\n"
      << "  --file-regex <regex>      Filter findings by repository-relative "
         "path\n"
      << "  --name-regex <regex>      Filter findings by qualified field name\n"
      << "  --export-fixes <file>     Write findings as a clang-tidy "
         "export-fixes YAML\n"
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

fs::path normalizedArgument(const fs::path &path,
                            const fs::path &repositoryRoot) {
  if (path.is_absolute()) {
    return normalized(path);
  }

  const fs::path workingDirectoryPath = normalized(path);
  std::error_code error;
  if (fs::exists(workingDirectoryPath, error)) {
    return workingDirectoryPath;
  }
  return normalized(path, repositoryRoot);
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

class FilteredCompilationDatabase final
    : public clang::tooling::CompilationDatabase {
public:
  FilteredCompilationDatabase(
      const clang::tooling::CompilationDatabase &database,
      const std::vector<fs::path> &scanRoots) {
    for (const auto &command : database.getAllCompileCommands()) {
      const fs::path file = normalized(command.Filename, command.Directory);
      if (!std::any_of(
              scanRoots.begin(), scanRoots.end(),
              [&](const fs::path &root) { return isWithin(file, root); })) {
        continue;
      }
      if (file.filename().string().rfind("cmake_pch", 0) == 0) {
        continue;
      }

      auto clangCommand = command;
      auto &arguments = clangCommand.CommandLine;
      for (auto it = arguments.begin(); it != arguments.end();) {
        if (*it == "-Winvalid-pch") {
          it = arguments.erase(it);
        } else if (*it == "-include" && std::next(it) != arguments.end() &&
                   std::next(it)->find("cmake_pch") != std::string::npos) {
          it = arguments.erase(it, std::next(it, 2));
        } else {
          ++it;
        }
      }
      arguments.emplace_back("-Wno-error=unknown-warning-option");
      commands_.push_back(clangCommand);
      commandsByFile_.emplace(file.string(), std::move(clangCommand));
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
  unsigned fileOffset = 0;
};

enum class Access : unsigned { None = 0, Read = 1, Write = 2 };

Access operator|(Access left, Access right) {
  return static_cast<Access>(static_cast<unsigned>(left) |
                             static_cast<unsigned>(right));
}

bool hasAccess(Access value, Access requested) {
  return (static_cast<unsigned>(value) & static_cast<unsigned>(requested)) != 0;
}

class Findings {
public:
  Findings(fs::path repositoryRoot, fs::path sourceRoot)
      : repositoryRoot_(std::move(repositoryRoot)),
        sourceRoot_(std::move(sourceRoot)) {}

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
                   presumed.isValid() ? presumed.getColumn() : 0,
                   sourceManager.getFileOffset(location)};
    std::lock_guard<std::mutex> lock(mutex_);
    declarations_.try_emplace(usr, std::move(info));
  }

  void recordAccess(const clang::FieldDecl *field, Access access) {
    std::string usr = usrFor(field);
    if (usr.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (hasAccess(access, Access::Read)) {
      reads_.insert(usr);
    }
    if (hasAccess(access, Access::Write)) {
      writes_.insert(std::move(usr));
    }
  }

  void recordUnresolvedRead(clang::DeclarationName name) {
    if (name.isEmpty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    unresolvedReadNames_.insert(name.getAsString());
  }

  void recordAggregateAccess(clang::QualType type, Access access) {
    recordAggregateAccess(type.getNonReferenceType()->getAsRecordDecl(),
                          access);
  }

  // Takes the RecordDecl directly so callers holding one don't need to build a
  // QualType for it: ASTContext::getRecordType() was removed in LLVM 22.
  void recordAggregateAccess(const clang::RecordDecl *record, Access access) {
    if (!record) {
      return;
    }
    for (const auto *field : record->fields()) {
      recordAccess(field, access);
    }
    if (const auto *cxxRecord = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
      for (const auto &base : cxxRecord->bases()) {
        recordAggregateAccess(base.getType(), access);
      }
    }
  }

  std::vector<FieldInfo> writeOnlyFields() const {
    std::vector<FieldInfo> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &[usr, field] : declarations_) {
      if (writes_.find(usr) != writes_.end() &&
          reads_.find(usr) == reads_.end() &&
          unresolvedReadNames_.find(field.name) == unresolvedReadNames_.end()) {
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

  const fs::path &repositoryRoot() const { return repositoryRoot_; }

private:
  fs::path repositoryRoot_;
  fs::path sourceRoot_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, FieldInfo> declarations_;
  std::unordered_set<std::string> reads_;
  std::unordered_set<std::string> writes_;
  std::unordered_set<std::string> unresolvedReadNames_;
};

class MemberVisitor final : public clang::RecursiveASTVisitor<MemberVisitor> {
public:
  MemberVisitor(Findings &findings, clang::ASTContext &context)
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
        MemberVisitor>::TraverseConstructorInitializer(initializer);
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

  Findings &findings_;
  clang::ASTContext &context_;
};

class MemberConsumer final : public clang::ASTConsumer {
public:
  MemberConsumer(Findings &findings, clang::ASTContext &context)
      : visitor_(findings, context) {}

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
                                            compiler.getASTContext());
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
  Findings findings(options->repositoryRoot, options->sourceRoot);
  clang::tooling::AllTUsToolExecutor executor(filteredDatabase, options->jobs);
  if (llvm::Error error =
          executor.execute(std::make_unique<MemberActionFactory>(findings))) {
    llvm::errs() << "error: Clang analysis failed: "
                 << llvm::toString(std::move(error)) << '\n';
    return 2;
  }

  const auto writeOnlyFields = findings.writeOnlyFields();
  clang::tooling::TranslationUnitDiagnostics exported;
  exported.MainSourceFile = options->sourceRoot.string();
  std::size_t displayed = 0;
  for (const auto &field : writeOnlyFields) {
    std::error_code error;
    fs::path displayPath =
        fs::relative(field.file, findings.repositoryRoot(), error);
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

    clang::tooling::Diagnostic diagnostic("energyplus-write-only-member",
                                          clang::tooling::Diagnostic::Warning,
                                          options->buildDir.string());
    diagnostic.Message.Message = "data member '" + field.qualifiedName + "' [" +
                                 field.type + "] is written but never read";
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

  llvm::errs() << "Found " << writeOnlyFields.size()
               << " fields with writes but no reads; displayed " << displayed
               << " after filters\n"
               << "Treat findings as candidates and validate them against "
                  "other build configurations.\n";
  return displayed > 0 ? 1 : 0;
}
