//===- Utils.h - Misc utilities for the front-end ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This header contains miscellaneous utilities for various front-end actions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_FRONTEND_UTILS_H
#define LLVM_CLANG_FRONTEND_UTILS_H

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LLVM.h"
#include "clang/Driver/OptionUtils.h"
#include "clang/Frontend/DependencyOutputOptions.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Option/OptSpecifier.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace llvm {

class Triple;

} // namespace llvm

namespace clang {

class ASTReader;
class CompilerInstance;
class CompilerInvocation;
class DiagnosticsEngine;
class ExternalSemaSource;
class FrontendOptions;
class HeaderSearch;
class HeaderSearchOptions;
class LangOptions;
class PCHContainerReader;
class Preprocessor;
class PreprocessorOptions;
class PreprocessorOutputOptions;
/// Apply the header search options to get given HeaderSearch object.
void ApplyHeaderSearchOptions(HeaderSearch &HS,
                              const HeaderSearchOptions &HSOpts,
                              const LangOptions &Lang,
                              const llvm::Triple &triple);

/// InitializePreprocessor - Initialize the preprocessor getting it and the
/// environment ready to process a single file.
void InitializePreprocessor(Preprocessor &PP, const PreprocessorOptions &PPOpts,
                            const PCHContainerReader &PCHContainerRdr,
                            const FrontendOptions &FEOpts);

/// DoPrintPreprocessedInput - Implement -E mode.
void DoPrintPreprocessedInput(Preprocessor &PP, raw_ostream *OS,
                              const PreprocessorOutputOptions &Opts);

/// An interface for collecting the dependencies of a compilation. Users should
/// use \c attachToPreprocessor and \c attachToASTReader to get all of the
/// dependencies.
/// FIXME: Migrate DependencyGraphGen to use this interface.
class DependencyCollector {
public:
  virtual ~DependencyCollector();

  virtual void attachToPreprocessor(Preprocessor &PP);
  virtual void attachToASTReader(ASTReader &R);
  ArrayRef<std::string> getDependencies() const { return Dependencies; }

  /// Called when a new file is seen. Return true if \p Filename should be added
  /// to the list of dependencies.
  ///
  /// The default implementation ignores <built-in> and system files.
  virtual bool sawDependency(StringRef Filename, bool FromModule,
                             bool IsSystem, bool IsModuleFile, bool IsMissing);

  /// Called when the end of the main file is reached.
  virtual void finishedMainFile(DiagnosticsEngine &Diags) {}

  /// Return true if system files should be passed to sawDependency().
  virtual bool needSystemDependencies() { return false; }

  /// Add a dependency \p Filename if it has not been seen before and
  /// sawDependency() returns true.
  virtual void maybeAddDependency(StringRef Filename, bool FromModule,
                                  bool IsSystem, bool IsModuleFile,
                                  bool IsMissing);

protected:
  /// Return true if the filename was added to the list of dependencies, false
  /// otherwise.
  bool addDependency(StringRef Filename);

private:
  llvm::StringSet<> Seen;
  std::vector<std::string> Dependencies;
};

/// Builds a dependency file when attached to a Preprocessor (for includes) and
/// ASTReader (for module imports), and writes it out at the end of processing
/// a source file.  Users should attach to the ast reader whenever a module is
/// loaded.
class DependencyFileGenerator : public DependencyCollector {
public:
  DependencyFileGenerator(const DependencyOutputOptions &Opts);

  void attachToPreprocessor(Preprocessor &PP) override;

  void finishedMainFile(DiagnosticsEngine &Diags) override;

  bool needSystemDependencies() final override { return IncludeSystemHeaders; }

  bool sawDependency(StringRef Filename, bool FromModule, bool IsSystem,
                     bool IsModuleFile, bool IsMissing) final override;

protected:
  void outputDependencyFile(llvm::raw_ostream &OS);

private:
  void outputDependencyFile(DiagnosticsEngine &Diags);

  std::string OutputFile;
  std::vector<std::string> Targets;
  bool IncludeSystemHeaders;
  bool PhonyTarget;
  bool AddMissingHeaderDeps;
  bool SeenMissingHeader;
  bool IncludeModuleFiles;
  DependencyOutputFormat OutputFormat;
  unsigned InputFileIndex;
};

/// Collects the dependencies for imported modules into a directory.  Users
/// should attach to the AST reader whenever a module is loaded.
class ModuleDependencyCollector : public DependencyCollector {
  std::string DestDir;
  bool HasErrors = false;
  llvm::StringSet<> Seen;
  llvm::vfs::YAMLVFSWriter VFSWriter;
  llvm::StringMap<std::string> SymLinkMap;

  bool getRealPath(StringRef SrcPath, SmallVectorImpl<char> &Result);
  std::error_code copyToRoot(StringRef Src, StringRef Dst = {});

public:
  ModuleDependencyCollector(std::string DestDir)
      : DestDir(std::move(DestDir)) {}
  ~ModuleDependencyCollector() override { writeFileMap(); }

  StringRef getDest() { return DestDir; }
  virtual bool insertSeen(StringRef Filename) { return Seen.insert(Filename).second; }
  virtual void addFile(StringRef Filename, StringRef FileDst = {});

  virtual void addFileMapping(StringRef VPath, StringRef RPath) {
    VFSWriter.addFileMapping(VPath, RPath);
  }

  void attachToPreprocessor(Preprocessor &PP) override;
  void attachToASTReader(ASTReader &R) override;

  virtual void writeFileMap();
  virtual bool hasErrors() { return HasErrors; }
};

/// AttachDependencyGraphGen - Create a dependency graph generator, and attach
/// it to the given preprocessor.
void AttachDependencyGraphGen(Preprocessor &PP, StringRef OutputFile,
                              StringRef SysRoot);

/// AttachHeaderIncludeGen - Create a header include list generator, and attach
/// it to the given preprocessor.
///
/// \param DepOpts - Options controlling the output.
/// \param ShowAllHeaders - If true, show all header information instead of just
/// headers following the predefines buffer. This is useful for making sure
/// includes mentioned on the command line are also reported, but differs from
/// the default behavior used by -H.
/// \param OutputPath - If non-empty, a path to write the header include
/// information to, instead of writing to stderr.
/// \param ShowDepth - Whether to indent to show the nesting of the includes.
/// \param MSStyle - Whether to print in cl.exe /showIncludes style.
void AttachHeaderIncludeGen(Preprocessor &PP,
                            const DependencyOutputOptions &DepOpts,
                            bool ShowAllHeaders = false,
                            StringRef OutputPath = {},
                            bool ShowDepth = true, bool MSStyle = false);

/// The ChainedIncludesSource class converts headers to chained PCHs in
/// memory, mainly for testing.
IntrusiveRefCntPtr<ExternalSemaSource>
createChainedIncludesSource(CompilerInstance &CI,
                            IntrusiveRefCntPtr<ExternalSemaSource> &Reader);

/// createInvocationFromCommandLine - Construct a compiler invocation object for
/// a command line argument vector.
///
/// \param ShouldRecoverOnErrors - whether we should attempt to return a
/// non-null (and possibly incorrect) CompilerInvocation if any errors were
/// encountered. When this flag is false, always return null on errors.
///
/// \param CC1Args - if non-null, will be populated with the args to cc1
/// expanded from \p Args. May be set even if nullptr is returned.
///
/// \return A CompilerInvocation, or nullptr if none was built for the given
/// argument vector.
std::unique_ptr<CompilerInvocation> createInvocationFromCommandLine(
    ArrayRef<const char *> Args,
    IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
        IntrusiveRefCntPtr<DiagnosticsEngine>(),
    IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS = nullptr,
    bool ShouldRecoverOnErrors = false,
    std::vector<std::string> *CC1Args = nullptr);

// Frontend timing utils

/// If the user specifies the -ftime-report argument on an Clang command line
/// then the value of this boolean will be true, otherwise false.
extern bool FrontendTimesIsEnabled;

} // namespace clang

#endif // LLVM_CLANG_FRONTEND_UTILS_H
