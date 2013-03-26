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

#ifndef ANDROID_BITCODE_LINKER_H
#define ANDROID_BITCODE_LINKER_H

#include <string>
#include <set>
#include <vector>
#include "llvm/ADT/StringRef.h"
#include "llvm/Linker.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Wrap/BitcodeWrapper.h"

namespace llvm {

class AndroidBitcodeItem {

  public:
    AndroidBitcodeItem(std::string FileName, bool isWhole = false) :
      File(FileName), WholeArchive(isWhole), NativeBinary(false),
      Wrapper(0) {
    }

    ~AndroidBitcodeItem() {
      if (Wrapper != 0)
        delete Wrapper;
    }

    void setWholeArchive(bool whole) { WholeArchive = whole; }

    void setNative(bool native) { NativeBinary = native; }

    void setWrapper(BitcodeWrapper *wrapper) {
      if (Wrapper != 0)
        delete Wrapper;

      Wrapper = wrapper;
    }

    bool isWholeArchive() { return WholeArchive; }

    bool isNative() { return NativeBinary; }

    BitcodeWrapper* getWrapper() { return Wrapper; }

   const sys::PathWithStatus& getFile() { return File; }

  private:
    sys::PathWithStatus File;
    bool WholeArchive;
    bool NativeBinary;
    BitcodeWrapper *Wrapper;
};

class LinkerConfig {
  public:
    LinkerConfig(LLVMContext& context, StringRef progname,
                 StringRef modulename, unsigned flags,
                 bool disableopt, bool stripall, bool stripdebug) :
                 C(context), ProgName(progname), ModuleName(modulename),
                 Flags(flags), DisableOpt(disableopt), StripAll(stripall),
                 StripDebug(stripdebug) {
    }

    StringRef& getProgName() { return ProgName; }

    StringRef& getModuleName() { return ModuleName; }

    LLVMContext& getContext() { return C; }

    unsigned getFlags() { return Flags; }

    bool isDisableOpt() { return DisableOpt; }

    bool isStripAll() { return StripAll; }

    bool isStripDebug() { return StripDebug; }

  private:
    LLVMContext &C;
    StringRef ProgName;
    StringRef ModuleName;
    unsigned Flags;
    bool DisableOpt;
    bool StripAll;
    bool StripDebug;
};

class AndroidBitcodeLinker {
  public:
    typedef std::vector<AndroidBitcodeItem> ABCItemList;

    AndroidBitcodeLinker(LinkerConfig &config) :
      Config(config), linker(0) {
    }

    ~AndroidBitcodeLinker() {
      if (linker != 0)
        delete linker;
    }

    // main procedure to link bitcodes
    bool LinkInAndroidBitcodes(ABCItemList& Item, std::vector<std::string*> &BCStrings);

  private:

    bool LinkInAndroidBitcode(AndroidBitcodeItem& Item);

    bool LinkInArchive(AndroidBitcodeItem &Item);

    bool LinkInWholeArchive(AndroidBitcodeItem &Item);

    Module* LoadAndroidBitcode(AndroidBitcodeItem &Item);

    std::string* GenerateBitcode();

    void UpdateSymbolList(Module* M);

    void GetAllSymbols(Module *M, std::set<std::string> &UndefinedSymbols,
                       std::set<std::string> &DefinedSymbols);

    bool warning(StringRef message) {
      Error = message;
      if (!(Config.getFlags()&Linker::QuietWarnings))
        errs() << Config.getProgName() << ": warning: " << message << "\n";
      return false;
    }

    bool error(StringRef message) {
      Error = message;
      if (!(Config.getFlags()&Linker::QuietErrors))
        errs() << Config.getProgName() << ": error: " << message << "\n";
      return true;
    }

    void verbose(StringRef message) {
      if (Config.getFlags()&Linker::Verbose)
        errs() << "  " << message << "\n";
    }

  private:
    std::set<std::string> GlobalUndefinedSymbols;
    std::set<std::string> GlobalDefinedSymbols;
    LinkerConfig& Config;
    Linker* linker;
    std::string Error;
};

} // end namespace llvm

#endif
