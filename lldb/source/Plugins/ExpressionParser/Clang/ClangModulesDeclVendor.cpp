//===-- ClangModulesDeclVendor.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/AST/Decl.h"
#include "clang/CodeGen/ObjectFilePCHContainerOperations.h"

#include "clang/Basic/TargetInfo.h"
#include "lldb/Core/Module.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/Lookup.h"
#include "clang/Serialization/ASTReader.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Threading.h"
#include <iostream>
#include <mutex>

#include "ClangHost.h"
#include "ClangModulesDeclVendor.h"
#include "ModuleDependencyCollector.h"

#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/SourceModule.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/LLDBAssert.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Reproducer.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/Target/Target.h"
#include <memory>
#include <mutex>

using namespace lldb_private;

namespace {
// Any Clang compiler requires a consumer for diagnostics.  This one stores
// them as strings so we can provide them to the user in case a module failed
// to load.
class StoringDiagnosticConsumer : public clang::DiagnosticConsumer {
public:
  StoringDiagnosticConsumer();

  void HandleDiagnostic(clang::DiagnosticsEngine::Level DiagLevel,
                        const clang::Diagnostic &info) override;

  void ClearDiagnostics();

  void DumpDiagnostics(Stream &error_stream);

  void BeginSourceFile(const clang::LangOptions &LangOpts,
                       const clang::Preprocessor *PP = nullptr) override;
  void EndSourceFile() override;

private:
  typedef std::pair<clang::DiagnosticsEngine::Level, std::string>
      IDAndDiagnostic;
  std::vector<IDAndDiagnostic> m_diagnostics;
  /// The DiagnosticPrinter used for creating the full diagnostic messages
  /// that are stored in m_diagnostics.
  std::shared_ptr<clang::TextDiagnosticPrinter> m_diag_printer;
  /// Output stream of m_diag_printer.
  std::shared_ptr<llvm::raw_string_ostream> m_os;
  /// Output string filled by m_os. Will be reused for different diagnostics.
  std::string m_output;
  Log *m_log;
};

// The private implementation of our ClangModulesDeclVendor.  Contains all the
// Clang state required to load modules.
class ClangModulesDeclVendorImpl : public ClangModulesDeclVendor {
public:
  ClangModulesDeclVendorImpl(
      llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics_engine,
      std::shared_ptr<clang::CompilerInvocation> compiler_invocation,
      std::unique_ptr<clang::CompilerInstance> compiler_instance,
      std::unique_ptr<clang::Parser> parser);

  ~ClangModulesDeclVendorImpl() override = default;

  bool AddModule(const SourceModule &module, ModuleVector *exported_modules,
                 Stream &error_stream) override;

  bool AddModulesForCompileUnit(CompileUnit &cu, ModuleVector &exported_modules,
                                Stream &error_stream) override;

  uint32_t FindDecls(clang::DeclContext *context, ConstString name, bool append,
                     uint32_t max_matches,
                     std::vector<CompilerDecl> &decls) override;

  void ForEachMacro(const ModuleVector &modules,
                    std::function<bool(const std::string &)> handler) override;

private:
  void
  ReportModuleExportsHelper(std::set<ClangModulesDeclVendor::ModuleID> &exports,
                            clang::Module *module);

  void ReportModuleExports(ModuleVector &exports, clang::Module *module);

  clang::ModuleLoadResult DoGetModule(clang::ModuleIdPath path,
                                      bool make_visible);

  bool m_enabled = false;

  llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> m_diagnostics_engine;
  std::shared_ptr<clang::CompilerInvocation> m_compiler_invocation;
  std::unique_ptr<clang::CompilerInstance> m_compiler_instance;
  std::unique_ptr<clang::Parser> m_parser;
  size_t m_source_location_index =
      0; // used to give name components fake SourceLocations

  typedef std::vector<ConstString> ImportedModule;
  typedef std::map<ImportedModule, clang::Module *> ImportedModuleMap;
  typedef std::set<ModuleID> ImportedModuleSet;
  ImportedModuleMap m_imported_modules;
  ImportedModuleSet m_user_imported_modules;
  // We assume that every ASTContext has an TypeSystemClang, so we also store
  // a custom TypeSystemClang for our internal ASTContext.
  std::unique_ptr<TypeSystemClang> m_ast_context;
};
} // anonymous namespace

StoringDiagnosticConsumer::StoringDiagnosticConsumer() {
  m_log = lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EXPRESSIONS);

  clang::DiagnosticOptions *m_options = new clang::DiagnosticOptions();
  m_os = std::make_shared<llvm::raw_string_ostream>(m_output);
  m_diag_printer =
      std::make_shared<clang::TextDiagnosticPrinter>(*m_os, m_options);
}

void StoringDiagnosticConsumer::HandleDiagnostic(
    clang::DiagnosticsEngine::Level DiagLevel, const clang::Diagnostic &info) {
  // Print the diagnostic to m_output.
  m_output.clear();
  m_diag_printer->HandleDiagnostic(DiagLevel, info);
  m_os->flush();

  // Store the diagnostic for later.
  m_diagnostics.push_back(IDAndDiagnostic(DiagLevel, m_output));
}

void StoringDiagnosticConsumer::ClearDiagnostics() { m_diagnostics.clear(); }

void StoringDiagnosticConsumer::DumpDiagnostics(Stream &error_stream) {
  for (IDAndDiagnostic &diag : m_diagnostics) {
    switch (diag.first) {
    default:
      error_stream.PutCString(diag.second);
      error_stream.PutChar('\n');
      break;
    case clang::DiagnosticsEngine::Level::Ignored:
      break;
    }
  }
}

void StoringDiagnosticConsumer::BeginSourceFile(
    const clang::LangOptions &LangOpts, const clang::Preprocessor *PP) {
  m_diag_printer->BeginSourceFile(LangOpts, PP);
}

void StoringDiagnosticConsumer::EndSourceFile() {
  m_diag_printer->EndSourceFile();
}

ClangModulesDeclVendor::ClangModulesDeclVendor()
    : ClangDeclVendor(eClangModuleDeclVendor) {}

ClangModulesDeclVendor::~ClangModulesDeclVendor() {}

ClangModulesDeclVendorImpl::ClangModulesDeclVendorImpl(
    llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics_engine,
    std::shared_ptr<clang::CompilerInvocation> compiler_invocation,
    std::unique_ptr<clang::CompilerInstance> compiler_instance,
    std::unique_ptr<clang::Parser> parser)
    : m_diagnostics_engine(std::move(diagnostics_engine)),
      m_compiler_invocation(std::move(compiler_invocation)),
      m_compiler_instance(std::move(compiler_instance)),
      m_parser(std::move(parser)) {

  // Initialize our TypeSystemClang.
  m_ast_context.reset(
      new TypeSystemClang("ClangModulesDeclVendor ASTContext",
                          m_compiler_instance->getASTContext()));
}

void ClangModulesDeclVendorImpl::ReportModuleExportsHelper(
    std::set<ClangModulesDeclVendor::ModuleID> &exports,
    clang::Module *module) {
  if (exports.count(reinterpret_cast<ClangModulesDeclVendor::ModuleID>(module)))
    return;

  exports.insert(reinterpret_cast<ClangModulesDeclVendor::ModuleID>(module));

  llvm::SmallVector<clang::Module *, 2> sub_exports;

  module->getExportedModules(sub_exports);

  for (clang::Module *module : sub_exports) {
    ReportModuleExportsHelper(exports, module);
  }
}

void ClangModulesDeclVendorImpl::ReportModuleExports(
    ClangModulesDeclVendor::ModuleVector &exports, clang::Module *module) {
  std::set<ClangModulesDeclVendor::ModuleID> exports_set;

  ReportModuleExportsHelper(exports_set, module);

  for (ModuleID module : exports_set) {
    exports.push_back(module);
  }
}

bool ClangModulesDeclVendorImpl::AddModule(const SourceModule &module,
                                           ModuleVector *exported_modules,
                                           Stream &error_stream) {
  // Fail early.

  if (m_compiler_instance->hadModuleLoaderFatalFailure()) {
    error_stream.PutCString("error: Couldn't load a module because the module "
                            "loader is in a fatal state.\n");
    return false;
  }

  // Check if we've already imported this module.

  std::vector<ConstString> imported_module;

  for (ConstString path_component : module.path) {
    imported_module.push_back(path_component);
  }

  {
    ImportedModuleMap::iterator mi = m_imported_modules.find(imported_module);

    if (mi != m_imported_modules.end()) {
      if (exported_modules) {
        ReportModuleExports(*exported_modules, mi->second);
      }
      return true;
    }
  }

  clang::HeaderSearch &HS =
      m_compiler_instance->getPreprocessor().getHeaderSearchInfo();

  if (module.search_path) {
    auto path_begin = llvm::sys::path::begin(module.search_path.GetStringRef());
    auto path_end = llvm::sys::path::end(module.search_path.GetStringRef());
    auto sysroot_begin = llvm::sys::path::begin(module.sysroot.GetStringRef());
    auto sysroot_end = llvm::sys::path::end(module.sysroot.GetStringRef());
    // FIXME: Use C++14 std::equal(it, it, it, it) variant once it's available.
    bool is_system_module = false;
    // No need to inject search paths to modules in the sysroot.
    if (!is_system_module) {
      auto error = [&]() {
        error_stream.Printf("error: No module map file in %s\n",
                            module.search_path.AsCString());
        return false;
      };

      bool is_system = true;
      bool is_framework = false;
      auto dir =
          HS.getFileMgr().getDirectory(module.search_path.GetStringRef());
      if (!dir)
        return error();
      auto *file = HS.lookupModuleMapFile(*dir, is_framework);
      if (!file)
        return error();
      if (HS.loadModuleMapFile(file, is_system))
        return error();
    }
  }
  if (!HS.lookupModule(module.path.front().GetStringRef())) {
    error_stream.Printf("error: Header search couldn't locate module %s\n",
                        module.path.front().AsCString());
    return false;
  }

  llvm::SmallVector<std::pair<clang::IdentifierInfo *, clang::SourceLocation>,
                    4>
      clang_path;

  {
    clang::SourceManager &source_manager =
        m_compiler_instance->getASTContext().getSourceManager();

    for (ConstString path_component : module.path) {
      clang_path.push_back(std::make_pair(
          &m_compiler_instance->getASTContext().Idents.get(
              path_component.GetStringRef()),
          source_manager.getLocForStartOfFile(source_manager.getMainFileID())
              .getLocWithOffset(m_source_location_index++)));
    }
  }

  StoringDiagnosticConsumer *diagnostic_consumer =
      static_cast<StoringDiagnosticConsumer *>(
          m_compiler_instance->getDiagnostics().getClient());

  diagnostic_consumer->ClearDiagnostics();

  clang::Module *top_level_module = DoGetModule(clang_path.front(), false);
  if (!top_level_module) {
    diagnostic_consumer->DumpDiagnostics(error_stream);
    error_stream.Printf("error: Couldn't load top-level module %s\n",
                        module.path.front().AsCString());
    return false;
  }

  clang::Module *submodule = top_level_module;

  for (auto &component :
       llvm::ArrayRef<ConstString>(module.path).drop_front()) {
    submodule = submodule->findSubmodule(component.GetStringRef());
    if (!submodule) {
      diagnostic_consumer->DumpDiagnostics(error_stream);
      error_stream.Printf("error: Couldn't load submodule %s\n",
                          component.GetCString());
      return false;
    }
  }

  clang::Module *requested_module = DoGetModule(clang_path, true);
  if (requested_module != nullptr) {
    if (exported_modules) {
      ReportModuleExports(*exported_modules, requested_module);
    }

    m_imported_modules[imported_module] = requested_module;

    m_enabled = true;

    return true;
  }

  return false;
}

bool ClangModulesDeclVendor::LanguageSupportsClangModules(
    lldb::LanguageType language) {
  switch (language) {
  default:
    return false;
  case lldb::LanguageType::eLanguageTypeC:
  case lldb::LanguageType::eLanguageTypeC11:
  case lldb::LanguageType::eLanguageTypeC89:
  case lldb::LanguageType::eLanguageTypeC99:
  case lldb::LanguageType::eLanguageTypeC_plus_plus:
  case lldb::LanguageType::eLanguageTypeC_plus_plus_03:
  case lldb::LanguageType::eLanguageTypeC_plus_plus_11:
  case lldb::LanguageType::eLanguageTypeC_plus_plus_14:
  case lldb::LanguageType::eLanguageTypeObjC:
  case lldb::LanguageType::eLanguageTypeObjC_plus_plus:
    return true;
  }
}

bool ClangModulesDeclVendorImpl::AddModulesForCompileUnit(
    CompileUnit &cu, ClangModulesDeclVendor::ModuleVector &exported_modules,
    Stream &error_stream) {
  if (LanguageSupportsClangModules(cu.GetLanguage())) {
    for (auto &imported_module : cu.GetImportedModules())
      if (!AddModule(imported_module, &exported_modules, error_stream))
        return false;
  }
  return true;
}

// ClangImporter::lookupValue
class ExampleVisitor : public clang::RecursiveASTVisitor<ExampleVisitor> {
public:
  ExampleVisitor(const std::string &wantedDeclName) {
    m_wantedDeclName = wantedDeclName;
  }

  std::vector<clang::NamedDecl *> getWatnedDecls(clang::ASTContext &AST) {
    if (!s_is_cached_decls) {
      s_is_cached_decls = true;
      TraverseAST(AST);
    }

    std::vector<clang::NamedDecl *> wanted_decls;
    for (const auto &decl : s_cached_decls) {
      if (auto var = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
        if (var->isThisDeclarationADefinition()) {
          if (m_wantedDeclName == var->getTemplatedDecl()->getName().str()) {
            wanted_decls.push_back(decl);
          }
        }
      } else if (auto var = llvm::dyn_cast<
                     clang::ClassTemplatePartialSpecializationDecl>(decl)) {
        if (var->isThisDeclarationADefinition()) {

          auto templated = var->getSpecializedTemplate()->getTemplatedDecl();
          if (m_wantedDeclName == templated->getName()) {
            wanted_decls.push_back(decl);
          }
        }
      } else if (auto var =
                     llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
                         decl)) {
        if (var->isThisDeclarationADefinition()) {

          auto templated = var->getSpecializedTemplate()
                               ->getTemplatedDecl()
                               ->getName()
                               .str();
          if (m_wantedDeclName == templated) {
            wanted_decls.push_back(decl);
          }
        }
      } else {
        if (decl->getDeclName().isIdentifier() && m_wantedDeclName == decl->getName()) {
          wanted_decls.push_back(decl);
        }
      }
    }
    return wanted_decls;
  }

  bool TraverseClassTemplateDecl(clang::ClassTemplateDecl *decl) {

    if (dynamic_cast<clang::NamedDecl *>(decl) &&
        decl->isThisDeclarationADefinition()) {
      s_cached_decls.push_back(decl);
    }
    return clang::RecursiveASTVisitor<
        ExampleVisitor>::TraverseClassTemplateDecl(decl);
  }

  bool TraverseClassTemplatePartialSpecializationDecl(
      clang::ClassTemplatePartialSpecializationDecl *decl) {

    if (dynamic_cast<clang::NamedDecl *>(decl) &&
        decl->isThisDeclarationADefinition()) {
      s_cached_decls.push_back(decl);
    }
    return clang::RecursiveASTVisitor<
        ExampleVisitor>::TraverseClassTemplatePartialSpecializationDecl(decl);
  }

  bool TraverseClassTemplateSpecializationDecl(
      clang::ClassTemplateSpecializationDecl *decl) {

    if (dynamic_cast<clang::NamedDecl *>(decl) &&
        decl->isThisDeclarationADefinition()) {
      s_cached_decls.push_back(decl);
    }
    return clang::RecursiveASTVisitor<
        ExampleVisitor>::TraverseClassTemplateSpecializationDecl(decl);
  }

  bool TraverseDecl(clang::Decl *decl) {

    if (decl && nullptr != decl->getDeclContext() &&
        llvm::dyn_cast<clang::NamespaceDecl>(decl->getDeclContext())) {
      if (llvm::dyn_cast<clang::NamedDecl>(decl)) {
        s_cached_decls.push_back(llvm::dyn_cast<clang::NamedDecl>(decl));
      }
    }

    return clang::RecursiveASTVisitor<ExampleVisitor>::TraverseDecl(decl);
  }

  std::string m_wantedDeclName;
  thread_local static bool s_is_cached_decls;
  thread_local static std::vector<clang::NamedDecl *> s_cached_decls;
};

thread_local bool ExampleVisitor::s_is_cached_decls = false;
thread_local std::vector<clang::NamedDecl *> ExampleVisitor::s_cached_decls{};

void clearClangModulesDeclVendorImplCache() {
  ExampleVisitor::s_is_cached_decls = false;
  ExampleVisitor::s_cached_decls.clear();
}

uint32_t ClangModulesDeclVendorImpl::FindDecls(
    clang::DeclContext *context, ConstString name, bool append,
    uint32_t max_matches, std::vector<CompilerDecl> &decls) {
  if (!m_enabled) {
    return 0;
  }

  if (!append)
    decls.clear();

  clang::IdentifierInfo &ident =
      m_compiler_instance->getASTContext().Idents.get(name.GetStringRef());

  ExampleVisitor example(name.GetStringRef().str());
  auto wanted_decls =
      example.getWatnedDecls(m_compiler_instance->getASTContext());

  clang::LookupResult lookup_result(
      m_compiler_instance->getSema(), clang::DeclarationName(&ident),
      clang::SourceLocation(), clang::Sema::LookupOrdinaryName);

  m_compiler_instance->getSema().LookupName(
      lookup_result,
      m_compiler_instance->getSema().getScopeForContext(
          m_compiler_instance->getASTContext().getTranslationUnitDecl()));

  uint32_t num_matches = 0;
  std::vector<clang::NamedDecl *> decls_result(lookup_result.begin(),
                                               lookup_result.end());
  decls_result.insert(decls_result.begin(), wanted_decls.begin(),
                      wanted_decls.end());

  for (clang::NamedDecl *named_decl : decls_result) {
    if (num_matches >= max_matches)
      return num_matches;

    decls.push_back(m_ast_context->GetCompilerDecl(named_decl));
    ++num_matches;
  }

  return num_matches;
}

void ClangModulesDeclVendorImpl::ForEachMacro(
    const ClangModulesDeclVendor::ModuleVector &modules,
    std::function<bool(const std::string &)> handler) {
  if (!m_enabled) {
    return;
  }

  typedef std::map<ModuleID, ssize_t> ModulePriorityMap;
  ModulePriorityMap module_priorities;

  ssize_t priority = 0;

  for (ModuleID module : modules) {
    module_priorities[module] = priority++;
  }

  if (m_compiler_instance->getPreprocessor().getExternalSource()) {
    m_compiler_instance->getPreprocessor()
        .getExternalSource()
        ->ReadDefinedMacros();
  }

  for (clang::Preprocessor::macro_iterator
           mi = m_compiler_instance->getPreprocessor().macro_begin(),
           me = m_compiler_instance->getPreprocessor().macro_end();
       mi != me; ++mi) {
    const clang::IdentifierInfo *ii = nullptr;

    {
      if (clang::IdentifierInfoLookup *lookup =
              m_compiler_instance->getPreprocessor()
                  .getIdentifierTable()
                  .getExternalIdentifierLookup()) {
        lookup->get(mi->first->getName());
      }
      if (!ii) {
        ii = mi->first;
      }
    }

    ssize_t found_priority = -1;
    clang::MacroInfo *macro_info = nullptr;

    for (clang::ModuleMacro *module_macro :
         m_compiler_instance->getPreprocessor().getLeafModuleMacros(ii)) {
      clang::Module *module = module_macro->getOwningModule();

      {
        ModulePriorityMap::iterator pi =
            module_priorities.find(reinterpret_cast<ModuleID>(module));

        if (pi != module_priorities.end() && pi->second > found_priority) {
          macro_info = module_macro->getMacroInfo();
          found_priority = pi->second;
        }
      }

      clang::Module *top_level_module = module->getTopLevelModule();

      if (top_level_module != module) {
        ModulePriorityMap::iterator pi = module_priorities.find(
            reinterpret_cast<ModuleID>(top_level_module));

        if ((pi != module_priorities.end()) && pi->second > found_priority) {
          macro_info = module_macro->getMacroInfo();
          found_priority = pi->second;
        }
      }
    }

    if (macro_info) {
      std::string macro_expansion = "#define ";
      macro_expansion.append(mi->first->getName().str());

      {
        if (macro_info->isFunctionLike()) {
          macro_expansion.append("(");

          bool first_arg = true;

          for (auto pi = macro_info->param_begin(),
                    pe = macro_info->param_end();
               pi != pe; ++pi) {
            if (!first_arg) {
              macro_expansion.append(", ");
            } else {
              first_arg = false;
            }

            macro_expansion.append((*pi)->getName().str());
          }

          if (macro_info->isC99Varargs()) {
            if (first_arg) {
              macro_expansion.append("...");
            } else {
              macro_expansion.append(", ...");
            }
          } else if (macro_info->isGNUVarargs()) {
            macro_expansion.append("...");
          }

          macro_expansion.append(")");
        }

        macro_expansion.append(" ");

        bool first_token = true;

        for (clang::MacroInfo::tokens_iterator ti = macro_info->tokens_begin(),
                                               te = macro_info->tokens_end();
             ti != te; ++ti) {
          if (!first_token) {
            macro_expansion.append(" ");
          } else {
            first_token = false;
          }

          if (ti->isLiteral()) {
            if (const char *literal_data = ti->getLiteralData()) {
              std::string token_str(literal_data, ti->getLength());
              macro_expansion.append(token_str);
            } else {
              bool invalid = false;
              const char *literal_source =
                  m_compiler_instance->getSourceManager().getCharacterData(
                      ti->getLocation(), &invalid);

              if (invalid) {
                lldbassert(0 && "Unhandled token kind");
                macro_expansion.append("<unknown literal value>");
              } else {
                macro_expansion.append(
                    std::string(literal_source, ti->getLength()));
              }
            }
          } else if (const char *punctuator_spelling =
                         clang::tok::getPunctuatorSpelling(ti->getKind())) {
            macro_expansion.append(punctuator_spelling);
          } else if (const char *keyword_spelling =
                         clang::tok::getKeywordSpelling(ti->getKind())) {
            macro_expansion.append(keyword_spelling);
          } else {
            switch (ti->getKind()) {
            case clang::tok::TokenKind::identifier:
              macro_expansion.append(ti->getIdentifierInfo()->getName().str());
              break;
            case clang::tok::TokenKind::raw_identifier:
              macro_expansion.append(ti->getRawIdentifier().str());
              break;
            default:
              macro_expansion.append(ti->getName());
              break;
            }
          }
        }

        if (handler(macro_expansion)) {
          return;
        }
      }
    }
  }
}

clang::ModuleLoadResult
ClangModulesDeclVendorImpl::DoGetModule(clang::ModuleIdPath path,
                                        bool make_visible) {
  clang::Module::NameVisibilityKind visibility =
      make_visible ? clang::Module::AllVisible : clang::Module::Hidden;

  const bool is_inclusion_directive = false;

  return m_compiler_instance->loadModule(path.front().second, path, visibility,
                                         is_inclusion_directive);
}

static const char *ModuleImportBufferName = "LLDBModulesMemoryBuffer";

extern bool getCompileSettingsFromSection(const std::string &exe_path, lldb_private::Target &target,
                     clang::CompilerInstance &CI);

lldb_private::ClangModulesDeclVendor *
ClangModulesDeclVendor::Create(Target &target) {
  // FIXME we should insure programmatically that the expression parser's
  // compiler and the modules runtime's
  // compiler are both initialized in the same way – preferably by the same
  // code.

  if (!target.GetPlatform()->SupportsModules())
    return nullptr;

  const ArchSpec &arch = target.GetArchitecture();

  std::vector<std::string> compiler_invocation_arguments = {
      "clang",
      "-fmodules",
      "-fimplicit-module-maps",
      "-fcxx-modules",
      "-fsyntax-only",
      "-femit-all-decls",
      "-target",
      arch.GetTriple().str(),
      "-fmodules-validate-system-headers",
      "-Werror=non-modular-include-in-framework-module"};

  target.GetPlatform()->AddClangModuleCompilationOptions(
      &target, compiler_invocation_arguments);

  compiler_invocation_arguments.push_back(ModuleImportBufferName);

  // Add additional search paths with { "-I", path } or { "-F", path } here.

  {
    llvm::SmallString<128> path;
    const auto &props = ModuleList::GetGlobalModuleListProperties();
    props.GetClangModulesCachePath().GetPath(path);
    std::string module_cache_argument("-fmodules-cache-path=");
    module_cache_argument.append(std::string(path.str()));
    compiler_invocation_arguments.push_back(module_cache_argument);
  }

  FileSpecList module_search_paths = target.GetClangModuleSearchPaths();

  for (size_t spi = 0, spe = module_search_paths.GetSize(); spi < spe; ++spi) {
    const FileSpec &search_path = module_search_paths.GetFileSpecAtIndex(spi);

    std::string search_path_argument = "-I";
    search_path_argument.append(search_path.GetPath());

    compiler_invocation_arguments.push_back(search_path_argument);
  }

  {
    FileSpec clang_resource_dir = GetClangResourceDir();

    if (FileSystem::Instance().IsDirectory(clang_resource_dir.GetPath())) {
      compiler_invocation_arguments.push_back("-resource-dir");
      compiler_invocation_arguments.push_back(clang_resource_dir.GetPath());
    }
  }

  llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics_engine =
      clang::CompilerInstance::createDiagnostics(new clang::DiagnosticOptions,
                                                 new StoringDiagnosticConsumer);

  std::vector<const char *> compiler_invocation_argument_cstrs;
  compiler_invocation_argument_cstrs.reserve(
      compiler_invocation_arguments.size());
  for (const std::string &arg : compiler_invocation_arguments)
    compiler_invocation_argument_cstrs.push_back(arg.c_str());

  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));
  LLDB_LOG(log, "ClangModulesDeclVendor's compiler flags {0:$[ ]}",
           llvm::make_range(compiler_invocation_arguments.begin(),
                            compiler_invocation_arguments.end()));

  std::shared_ptr<clang::CompilerInvocation> invocation =
      clang::createInvocationFromCommandLine(compiler_invocation_argument_cstrs,
                                             diagnostics_engine);

  if (!invocation)
    return nullptr;

  std::unique_ptr<llvm::MemoryBuffer> source_buffer =
      llvm::MemoryBuffer::getMemBuffer(
          "extern int __lldb __attribute__((unavailable));",
          ModuleImportBufferName);

  invocation->getPreprocessorOpts().addRemappedFile(ModuleImportBufferName,
                                                    source_buffer.release());

  std::unique_ptr<clang::CompilerInstance> instance(
      new clang::CompilerInstance);

  // When capturing a reproducer, hook up the file collector with clang to
  // collector modules and headers.
  if (repro::Generator *g = repro::Reproducer::Instance().GetGenerator()) {
    repro::FileProvider &fp = g->GetOrCreate<repro::FileProvider>();
    instance->setModuleDepCollector(
        std::make_shared<ModuleDependencyCollectorAdaptor>(
            fp.GetFileCollector()));
    clang::DependencyOutputOptions &opts = instance->getDependencyOutputOpts();
    opts.IncludeSystemHeaders = true;
    opts.IncludeModuleFiles = true;
  }

  // Make sure clang uses the same VFS as LLDB.
  instance->createFileManager(FileSystem::Instance().GetVirtualFileSystem());
  instance->setDiagnostics(diagnostics_engine.get());
  instance->setInvocation(invocation);
  auto PCHOps = instance->getPCHContainerOperations();
  PCHOps->registerWriter(std::make_unique<clang::ObjectFilePCHContainerWriter>());
  PCHOps->registerReader(std::make_unique<clang::ObjectFilePCHContainerReader>());
  std::string exe_path = target.CalculateProcess()->getPath();
  if (exe_path.empty()) {
    exe_path = target.GetExecutableModule()->GetFileSpec().GetPath();
  }
  bool isSucceededToHaveSettingsFromSection =
      getCompileSettingsFromSection(exe_path, target, *instance);

  if (!isSucceededToHaveSettingsFromSection && log) {
    log->Warning("There is no llvm_command section in the pe file\n");
  }
  std::unique_ptr<clang::FrontendAction> action(new clang::SyntaxOnlyAction);

  instance->setTarget(clang::TargetInfo::CreateTargetInfo(
      *diagnostics_engine, instance->getInvocation().TargetOpts));

  if (!instance->hasTarget())
    return nullptr;

  instance->getTarget().adjust(instance->getLangOpts());

  if (!action->BeginSourceFile(*instance,
                               instance->getFrontendOpts().Inputs[0]))
    return nullptr;
  
  instance->getPreprocessor().enableIncrementalProcessing();

  instance->createASTReader();

  instance->createSema(action->getTranslationUnitKind(), nullptr);

  const bool skipFunctionBodies = false;
  std::unique_ptr<clang::Parser> parser(new clang::Parser(
      instance->getPreprocessor(), instance->getSema(), skipFunctionBodies));

  instance->getPreprocessor().EnterMainSourceFile();
  parser->Initialize();

   

  

  clang::Parser::DeclGroupPtrTy parsed;

  while (!parser->ParseTopLevelDecl(parsed));

  return new ClangModulesDeclVendorImpl(std::move(diagnostics_engine),
                                        std::move(invocation),
                                        std::move(instance), std::move(parser));
}
