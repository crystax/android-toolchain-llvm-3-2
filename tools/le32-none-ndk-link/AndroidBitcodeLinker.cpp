/*
 * Copyright 2013, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "AndroidBitcodeLinker.h"

#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Bitcode/Archive.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/DataLayout.h"
#include "llvm/Linker.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/system_error.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Wrap/BitcodeWrapper.h"

#include <memory>
#include <set>
#include <vector>

using namespace llvm;

// Generate current module to std::string
std::string* AndroidBitcodeLinker::GenerateBitcode() {
  std::string *BCString = new std::string;
  Module *M = linker->releaseModule();

  PassManager PM;
  raw_string_ostream Bitcode(*BCString);

  PM.add(createVerifierPass());
  PM.add(new DataLayout(M));

  if (!Config.isDisableOpt())
    PassManagerBuilder().populateLTOPassManager(PM,
                                                false /*Internalize*/,
                                                true /*RunInliner*/);
  // Doing clean up passes
  if (!Config.isDisableOpt())
  {
    PM.add(createInstructionCombiningPass());
    PM.add(createCFGSimplificationPass());
    PM.add(createAggressiveDCEPass());
    PM.add(createGlobalDCEPass());
  }

  // Make sure everything is still good
  PM.add(createVerifierPass());

  // Strip debug info and symbols.
  if (Config.isStripAll() || Config.isStripDebug())
    PM.add(createStripSymbolsPass(Config.isStripDebug() && !Config.isStripAll()));

  PM.add(createBitcodeWriterPass(Bitcode));
  PM.run(*M);
  Bitcode.flush();

  // Re-compute defined and undefined symbols
  UpdateSymbolList(M);

  delete M;
  delete linker;
  linker = 0;

  return BCString;
}

Module *
AndroidBitcodeLinker::LoadAndroidBitcode(AndroidBitcodeItem &Item) {
  std::string ParseErrorMessage;
  const sys::PathWithStatus &FN = Item.getFile();
  Module *Result = 0;

  OwningPtr<MemoryBuffer> Buffer;
  if (error_code ec = MemoryBuffer::getFileOrSTDIN(FN.c_str(), Buffer)) {
    Error = "Error reading file '" + FN.str() + "'" + ": " + ec.message();
    return Result;
  }

  MemoryBuffer *buffer = Buffer.get();
  BitcodeWrapper *wrapper = new BitcodeWrapper(buffer->getBufferStart(), buffer->getBufferSize());
  Item.setWrapper(wrapper);
  assert(Item.getWrapper() != 0);
  if (wrapper->getBCFileType() == BC_RAW) {
    std::string ParseErrorMessage;
    Result = ParseBitcodeFile(buffer, Config.getContext(), &ParseErrorMessage);
    if (Result == 0) {
      Error = "Bitcode file '" + FN.str() + "' could not be loaded."
                + ParseErrorMessage;
    }
  }

  return Result;
}

void
AndroidBitcodeLinker::UpdateSymbolList(Module *M) {
  std::set<std::string> UndefinedSymbols;
  std::set<std::string> DefinedSymbols;
  GetAllSymbols(M, UndefinedSymbols, DefinedSymbols);

  // Update global undefined/defined symbols
  set_union(GlobalDefinedSymbols, DefinedSymbols);
  set_union(GlobalUndefinedSymbols, UndefinedSymbols);
  set_subtract(GlobalUndefinedSymbols, GlobalDefinedSymbols);

  verbose("Dump global defined symbols:");
    for (std::set<std::string>::iterator I = DefinedSymbols.begin();
       I != DefinedSymbols.end(); ++I)
    verbose("D:" + *I);

  verbose("Dump global undefined symbols:");
  for (std::set<std::string>::iterator I = GlobalUndefinedSymbols.begin();
       I != GlobalUndefinedSymbols.end(); ++I)
    verbose("U:" + *I);
}

bool
AndroidBitcodeLinker::LinkInAndroidBitcodes(ABCItemList& Items,
                                            std::vector<std::string*> &BCStrings) {
  // Create llvm::Linker
  linker = new Linker(Config.getProgName(), Config.getModuleName(),
                      Config.getContext(), Config.getFlags());

  for (ABCItemList::iterator I = Items.begin(), E = Items.end();
         I != E; ++I) {
    if (LinkInAndroidBitcode(*I))
      return true;
  }

  if (linker != 0)
    BCStrings.push_back(GenerateBitcode());

  return false;
}

//
// Link in bitcode relocatables and bitcode archive
//
bool
AndroidBitcodeLinker::LinkInAndroidBitcode(AndroidBitcodeItem &Item) {
  const sys::PathWithStatus &File = Item.getFile();

  if (File.str() == "-") {
    return error("Not supported!");
  }

  // Determine what variety of file it is.
  std::string Magic;
  if (!File.getMagicNumber(Magic, 8))
    return error("Cannot find linker input '" + File.str() + "'");

  switch (sys::IdentifyFileType(Magic.c_str(), 8)) {
    case sys::Archive_FileType: {
      if (Item.isWholeArchive()) {
        verbose("Link whole archive" + File.str());
        if (LinkInWholeArchive(Item))
          return true;
      }
      else {
        verbose("Link no-whole archive" + File.str());
        if (LinkInArchive(Item))
          return true;
      }
      break;
    }

    case sys::Bitcode_FileType: {

      verbose("Linking bitcode file '" + File.str() + "'");

      std::auto_ptr<Module> M(LoadAndroidBitcode(Item));

      int BCFileType = Item.getWrapper()->getBCFileType();
      int BitcodeType = -1;

      if (BCFileType == BC_RAW)
        BitcodeType = BCHeaderField::BC_Relocatable;
      else if (BCFileType == BC_WRAPPER)
        BitcodeType = Item.getWrapper()->getBitcodeType();
      else
        return error("Invalid bitcode file type" + File.str());

      if (M.get() == 0 && BitcodeType == BCHeaderField::BC_Relocatable)
        return error("Cannot load file '" + File.str() + "': " + Error);

      Triple triple(M.get()->getTargetTriple());

      if (triple.getArch() != Triple::le32 || triple.getOS() != Triple::NDK) {
        Item.setNative(true);
        return error("Cannot link '" + File.str() + "', triple:" +  M.get()->getTargetTriple());
      }

      switch (BitcodeType) {
        default:
          return error("Unknown android bitcode type");

        case BCHeaderField::BC_Relocatable:
          assert(linker != 0);
          if (linker->LinkInModule(M.get(), &Error))
            return error("Cannot link file '" + File.str() + "': " + Error);
          break;

        case BCHeaderField::BC_SharedObject:
          break;

        case BCHeaderField::BC_Executable:
          return error("Cannot link bitcode executable: " + File.str());
      }
      break;
    }

    default: {
      return error("Ignoring file '" + File.str() +
                   "' because does not contain bitcode.");
    }
  }
  return false;
}

bool
AndroidBitcodeLinker::LinkInWholeArchive(AndroidBitcodeItem &Item) {

const sys::PathWithStatus &Filename = Item.getFile();

  // Make sure this is an archive file we're dealing with
  if (!Filename.isArchive())
    return error("File '" + Filename.str() + "' is not an archive.");

  // Open the archive file
  verbose("Linking archive file '" + Filename.str() + "'");

  std::string ErrMsg;
  std::auto_ptr<Archive> AutoArch (
    Archive::OpenAndLoad(Filename, Config.getContext(), &ErrMsg));
  Archive* arch = AutoArch.get();

  // possible empty archive?
  if (!arch) {
    return false;
  }

  if (!arch->isBitcodeArchive()) {
    Item.setNative(true);
    return error("File '" + Filename.str() + "' is not a bitcode archive.");
  }

  std::vector<Module*> Modules;

  if (arch->getAllModules(Modules, &ErrMsg))
    return error("Cannot read modules in '" + Filename.str() + "': " + ErrMsg);

  if (Modules.empty()) {
    return false;
  }

  // Loop over all the Modules
  for (std::vector<Module*>::iterator I=Modules.begin(), E=Modules.end();
       I != E; ++I) {
    // Get the module we must link in.
    std::string moduleErrorMsg;
    Module* aModule = *I;
    if (aModule != NULL) {
      if (aModule->MaterializeAll(&moduleErrorMsg))
        return error("Could not load a module: " + moduleErrorMsg);

      verbose("  Linking in module: " + aModule->getModuleIdentifier());

      assert(linker != 0);
      // Link it in
      if (linker->LinkInModule(aModule, &moduleErrorMsg))
        return error("Cannot link in module '" +
                     aModule->getModuleIdentifier() + "': " + moduleErrorMsg);
      delete aModule;
    }
  }

  /* Success! */
  return false;
}

void
AndroidBitcodeLinker::GetAllSymbols(Module *M,
  std::set<std::string> &UndefinedSymbols,
  std::set<std::string> &DefinedSymbols) {

  UndefinedSymbols.clear();
  DefinedSymbols.clear();

  Function *Main = M->getFunction("main");
  if (Main == 0 || Main->isDeclaration())
    UndefinedSymbols.insert("main");

  for (Module::iterator I = M->begin(), E = M->end(); I != E; ++I)
    if (I->hasName()) {
      if (I->isDeclaration())
        UndefinedSymbols.insert(I->getName());
      else if (!I->hasLocalLinkage()) {
        assert(!I->hasDLLImportLinkage()
               && "Found dllimported non-external symbol!");
        DefinedSymbols.insert(I->getName());
      }
    }

  for (Module::global_iterator I = M->global_begin(), E = M->global_end();
       I != E; ++I)
    if (I->hasName()) {
      if (I->isDeclaration())
        UndefinedSymbols.insert(I->getName());
      else if (!I->hasLocalLinkage()) {
        assert(!I->hasDLLImportLinkage()
               && "Found dllimported non-external symbol!");
        DefinedSymbols.insert(I->getName());
      }
    }

  for (Module::alias_iterator I = M->alias_begin(), E = M->alias_end();
       I != E; ++I)
    if (I->hasName())
      DefinedSymbols.insert(I->getName());

  for (std::set<std::string>::iterator I = UndefinedSymbols.begin();
       I != UndefinedSymbols.end(); )
    if (DefinedSymbols.count(*I))
      UndefinedSymbols.erase(I++);
    else
      ++I;
}

bool
AndroidBitcodeLinker::LinkInArchive(AndroidBitcodeItem &Item) {
const sys::PathWithStatus &Filename = Item.getFile();

  if (!Filename.isArchive())
    return error("File '" + Filename.str() + "' is not an archive.");

  verbose("Linking archive file '" + Filename.str() + "'");

  std::set<std::string> UndefinedSymbols;
  std::set<std::string> DefinedSymbols;
  GetAllSymbols(linker->getModule(), UndefinedSymbols, DefinedSymbols);

  // Update list
  set_union(UndefinedSymbols, GlobalUndefinedSymbols);
  set_union(DefinedSymbols, GlobalDefinedSymbols);
  set_subtract(UndefinedSymbols, DefinedSymbols);

  if (UndefinedSymbols.empty()) {
    verbose("No symbols undefined, skipping library '" + Filename.str() + "'");
    return false;  // No need to link anything in!
  }

  std::string ErrMsg;
  std::auto_ptr<Archive> AutoArch (
    Archive::OpenAndLoadSymbols(Filename, Config.getContext(), &ErrMsg));

  Archive* arch = AutoArch.get();

  // possible empty archive?
  if (!arch) {
    return false;
  }

  if (!arch->isBitcodeArchive()) {
    Item.setNative(true);
    return error("File '" + Filename.str() + "' is not a bitcode archive.");
  }

  std::set<std::string> NotDefinedByArchive;

  std::set<std::string> CurrentlyUndefinedSymbols;

  do {
    CurrentlyUndefinedSymbols = UndefinedSymbols;

    SmallVector<Module*, 16> Modules;
    if (!arch->findModulesDefiningSymbols(UndefinedSymbols, Modules, &ErrMsg))
      return error("Cannot find symbols in '" + Filename.str() +
                   "': " + ErrMsg);

    if (Modules.empty())
      break;

    NotDefinedByArchive.insert(UndefinedSymbols.begin(),
        UndefinedSymbols.end());

    for (SmallVectorImpl<Module*>::iterator I=Modules.begin(), E=Modules.end();
         I != E; ++I) {

      std::string moduleErrorMsg;
      Module* aModule = *I;
      if (aModule != NULL) {
        if (aModule->MaterializeAll(&moduleErrorMsg))
          return error("Could not load a module: " + moduleErrorMsg);

        verbose("  Linking in module: " + aModule->getModuleIdentifier());

        // Link it in
        if (linker->LinkInModule(aModule, &moduleErrorMsg))
          return error("Cannot link in module '" +
                       aModule->getModuleIdentifier() + "': " + moduleErrorMsg);
      }
    }

    GetAllSymbols(linker->getModule(), UndefinedSymbols, DefinedSymbols);

    set_subtract(UndefinedSymbols, NotDefinedByArchive);

    if (UndefinedSymbols.empty())
      break;
  } while (CurrentlyUndefinedSymbols != UndefinedSymbols);

  return false;
}
