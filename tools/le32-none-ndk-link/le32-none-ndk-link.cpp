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

#include <list>
#include <cstring>

#include "AndroidBitcodeLinker.h"

#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Wrap/BitcodeWrapper.h"

using namespace llvm;

static cl::list<std::string>
InputFilenames(cl::Positional, cl::OneOrMore,
               cl::desc("<input bitcode files>"));

static cl::opt<bool>
Shared("shared", cl::ZeroOrMore, cl::desc("Generate shared bitcode library"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Override output filename"),
                cl::value_desc("output bitcode file"));

static cl::list<std::string>
LibPaths("L", cl::Prefix,
         cl::desc("Specify a library search path"),
         cl::value_desc("directory"));

static cl::list<std::string>
Libraries("l", cl::Prefix,
          cl::desc("Specify libraries to link to"),
          cl::value_desc("library name"));

static cl::opt<bool>
Verbose("v", cl::desc("Print verbose information"));

static cl::opt<bool>
DisableOptimizations("disable-opt",
                     cl::desc("Do not run any optimization passes"));

static cl::opt<std::string>
SOName("soname", cl::desc("Set the DT_SONAME field to the specified name"));

static cl::list<bool> WholeArchive("whole-archive",
  cl::desc("include every bitcode in the archive after --whole-archive"));

static cl::list<bool> NoWholeArchive("no-whole-archive",
  cl::desc("Turn off of the --whole-archive option for for subsequent archive files"));

// Strip options
static cl::opt<bool>
Strip("strip-all", cl::desc("Strip all symbol info"));

static cl::opt<bool>
StripDebug("strip-debug", cl::desc("Strip debugger symbol info"));

static cl::alias A0("s", cl::desc("Alias for --strip-all"),
  cl::aliasopt(Strip));

static cl::alias A1("S", cl::desc("Alias for --strip-debug"),
  cl::aliasopt(StripDebug));

static cl::opt<bool>
NoUndefined("no-undefined", cl::desc("-z defs"));

static cl::list<std::string>
ZOptions("z", cl::desc("-z keyword"), cl::value_desc("keyword"));

static cl::list<std::string> CO1("Wl", cl::Prefix,
  cl::desc("Compatibility option: ignored"));

static cl::opt<std::string> CO2("sysroot",
  cl::desc("Compatibility option: ignored"));

static cl::opt<std::string> CO3("exclude-libs",
  cl::desc("Compatibility option: ignored"));

static cl::opt<std::string> CO4("icf",
  cl::desc("Compatibility option: ignored"));

static cl::opt<std::string> CO5("dynamic-linker",
  cl::desc("Compatibility option: ignored"));

static cl::opt<bool> CO6("gc-sections",
  cl::desc("Compatibility option: ignored"));

static cl::list<std::string> CO7("B", cl::Prefix,
 cl::desc("Compatibility option: ignored"));

// TODO: Support --start-group and --end-group

static cl::list<bool> CO8("start-group",
  cl::desc("Compatibility option: ignored"));

static cl::list<bool> CO9("end-group",
  cl::desc("Compatibility option: ignored"));

static std::string progname;

// FileRemover objects to clean up output files on the event of an error.
static FileRemover OutputRemover;

static void PrintAndExit(const std::string &Message, int errcode = 1)
{
  errs() << progname << ": " << Message << "\n";
  llvm_shutdown();
  exit(errcode);
}

enum ZOptionEnum {
    kCombReloc     = 1 << 0,  ///< [on] -z combreloc, [off] -z nocombreloc
    kDefs          = 1 << 1,  ///< -z defs
    kExecStack     = 1 << 2,  ///< [on] -z execstack, [off] -z noexecstack
    kInitFirst     = 1 << 3,  ///< -z initfirst
    kInterPose     = 1 << 4,  ///< -z interpose
    kLoadFltr      = 1 << 5,  ///< -z loadfltr
    kMulDefs       = 1 << 6,  ///< -z muldefs
    kNoCopyReloc   = 1 << 7,  ///< -z nocopyreloc
    kNoDefaultLib  = 1 << 8,  ///< -z nodefaultlib
    kNoDelete      = 1 << 9,  ///< -z nodelete
    kNoDLOpen      = 1 << 10, ///< -z nodlopen
    kNoDump        = 1 << 11, ///< -z nodump
    kRelro         = 1 << 12, ///< [on] -z relro, [off] -z norelro
    kLazy          = 1 << 13, ///< [on] -z lazy, [off] -z now
    kOrigin        = 1 << 14, ///< -z origin
    kZOptionMask   = 0xFFFF
};

static uint32_t EncodeZOptions() {
  uint32_t zflag = 0;
  cl::list<std::string>::const_iterator I = ZOptions.begin(),
                                        E = ZOptions.end();
  while (I != E) {
    if (*I == "combreloc")
      zflag |= kCombReloc;

    if (*I == "defs")
      zflag |= kDefs;

    if (*I == "execstack")
      zflag |= kExecStack;

    if (*I == "initfirst")
      zflag |= kInitFirst;

    if (*I == "interpose")
      zflag |= kInterPose;

    if (*I == "loadfltr")
      zflag |= kLoadFltr;

    if (*I == "muldefs")
      zflag |= kMulDefs;

    if (*I == "nocopyreloc")
      zflag |= kNoCopyReloc;

    if (*I == "nodefaultlib")
      zflag |= kNoDefaultLib;

    if (*I == "nodelete")
      zflag |= kNoDelete;

    if (*I == "nodlopen")
      zflag |= kNoDLOpen;

    if (*I == "nodump")
      zflag |= kNoDump;

    if (*I == "relro")
      zflag |= kRelro;

    if (*I == "lazy")
      zflag |= kLazy;

    if (*I == "origin")
      zflag |= kOrigin;

    ++I;
  }

  // -Wl,--no-undefined
  if (NoUndefined)
    zflag |= kDefs;
  return zflag;
}

static void WriteInt32(uint8_t *mem, unsigned offset, uint32_t value) {
  mem[offset  ] = value & 0x000000ff;
  mem[offset+1] = (value & 0x0000ff00) >> 8;
  mem[offset+2] = (value & 0x00ff0000) >> 16;
  mem[offset+3] = (value & 0xff000000) >> 24;
}

static void WrapAndroidBitcode(std::vector<std::string*> &BCStrings, raw_ostream &Output) {
  std::vector<BCHeaderField> header_fields;
  std::vector<uint8_t *> field_data;
  size_t variable_header_size = 0;

  // shared object or executable
  uint32_t BitcodeType = (Shared) ? BCHeaderField::BC_SharedObject : BCHeaderField::BC_Executable;
  field_data.push_back(new uint8_t[sizeof(uint32_t)]);
  WriteInt32(field_data.back(), 0, BitcodeType);
  BCHeaderField BitcodeTypeField(BCHeaderField::kAndroidBitcodeType,
                                 sizeof(uint32_t), field_data.back());
  header_fields.push_back(BitcodeTypeField);
  variable_header_size += BitcodeTypeField.GetTotalSize();

  // Encode -z options
  uint32_t LDFlags = EncodeZOptions();
  field_data.push_back(new uint8_t[sizeof(uint32_t)]);
  WriteInt32(field_data.back(), 0, LDFlags);
  BCHeaderField LDFlagsField(BCHeaderField::kAndroidLDFlags,
                             sizeof(uint32_t), field_data.back());
  header_fields.push_back(LDFlagsField);
  variable_header_size += LDFlagsField.GetTotalSize();

  // SOName
  if (Shared) {
    std::string soname;

    // default to output filename ; must end with .so
    if (SOName.empty()) {
       soname = sys::path::stem(OutputFilename);
    }
    else {
       soname = sys::path::stem(SOName);
    }
    soname += ".so";
    field_data.push_back(new uint8_t[soname.size()+1]);
    strcpy((char *) field_data.back(), soname.c_str());
    BCHeaderField SONameField(BCHeaderField::kAndroidSOName,
                              soname.size()+1, field_data.back());
    header_fields.push_back(SONameField);
    variable_header_size += SONameField.GetTotalSize();
  }

  // Add dependent library
  for (cl::list<std::string>::const_iterator lib_iter = Libraries.begin(),
       lib_end = Libraries.end(); lib_iter != lib_end; ++lib_iter) {
    const char *depend_lib = lib_iter->c_str();
    BCHeaderField DependLibField(BCHeaderField::kAndroidDependLibrary,
                                 lib_iter->size()+1, (uint8_t *) depend_lib);
    header_fields.push_back(DependLibField);
    variable_header_size += DependLibField.GetTotalSize();
  }

  // Compute bitcode size
  uint32_t totalBCSize = 0;
  for (unsigned i = 0; i < BCStrings.size(); ++i) {
    uint32_t BCSize = BCStrings[i]->size();
    totalBCSize += BCSize;
  }

  AndroidBitcodeWrapper wrapper;
  size_t actualWrapperLen = writeAndroidBitcodeWrapper(&wrapper,
                                                       totalBCSize,
                                                       14,   /* FIXME: TargetAPI     */
                                                       3200, /* llvm-3.2             */
                                                       0);   /* OptimizationLevel    */
  wrapper.BitcodeOffset += variable_header_size;

  // Write fixed fields
  Output.write(reinterpret_cast<char*>(&wrapper), actualWrapperLen);

  // Write variable fields
  for (unsigned i = 0 ; i < header_fields.size(); ++i) {
    const uint32_t buffer_size = 1024;
    uint8_t buffer[buffer_size];
    header_fields[i].Write(buffer, buffer_size);
    Output.write(reinterpret_cast<char*>(buffer), header_fields[i].GetTotalSize());
  }

  // Delete field data
  for (unsigned i = 0 ; i < field_data.size(); ++i) {
    delete [] field_data[i];
  }

  for (unsigned i = 0 ; i < BCStrings.size(); ++i) {
    Output.write(BCStrings[i]->c_str(), BCStrings[i]->size());
    delete BCStrings[i];
  }
}

void GenerateBitcode(std::vector<std::string*> &BCStrings, const std::string& FileName) {

  if (Verbose)
    errs() << "Generating Bitcode To " << FileName << '\n';

  // Create the output file.
  std::string ErrorInfo;
  tool_output_file Out(FileName.c_str(), ErrorInfo,
                       raw_fd_ostream::F_Binary);
  if (!ErrorInfo.empty()) {
    PrintAndExit(ErrorInfo);
    return;
  }

  WrapAndroidBitcode(BCStrings, Out.os());
  Out.keep();
}

static void BuildLinkItems(
  AndroidBitcodeLinker::ABCItemList& Items,
  const cl::list<std::string>& Files) {
  cl::list<bool>::const_iterator wholeIt = WholeArchive.begin();
  cl::list<bool>::const_iterator noWholeIt = NoWholeArchive.begin();
  int wholePos = -1, noWholePos = -1;
  std::vector<std::pair<int,int> > wholeRange;

  while (wholeIt != WholeArchive.end()) {
    wholePos =  WholeArchive.getPosition(wholeIt - WholeArchive.begin());
    if (noWholeIt != NoWholeArchive.end())
      noWholePos = NoWholeArchive.getPosition(noWholeIt - NoWholeArchive.begin());
    else
      noWholePos = -1;

    if (wholePos < noWholePos) {
      wholeRange.push_back(std::make_pair(wholePos, noWholePos));
      ++wholeIt;
      ++noWholeIt;
    }
    else if (noWholePos <= 0) {
      wholeRange.push_back(std::make_pair(wholePos, -1));
      break;
    }
    else {
      noWholeIt++;
    }
  }

  cl::list<std::string>::const_iterator fileIt = Files.begin();
  while ( fileIt != Files.end() ) {
      bool isWhole = false;
      int filePos = Files.getPosition(fileIt - Files.begin());
      for(unsigned i = 0 ; i < wholeRange.size() ; ++i) {
        if (filePos > wholeRange[i].first &&
           (filePos < wholeRange[i].second || wholeRange[i].second == -1)) {
          isWhole = true;
          break;
        }
      }
      if (Verbose)
        errs() << *fileIt << ":" << isWhole << '\n';
      Items.push_back(AndroidBitcodeItem(*fileIt++, isWhole));
  }
}

int main(int argc, char** argv) {
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  llvm_shutdown_obj _ShutdownObj;
  LLVMContext& Ctx = llvm::getGlobalContext();

  progname = sys::path::stem(argv[0]);

  cl::ParseCommandLineOptions(argc, argv, "Bitcode link tool\n");

  // Arrange for the output file to be delete on any errors.
  OutputRemover.setFile(OutputFilename);
  sys::RemoveFileOnSignal(sys::Path(OutputFilename));

  // Build a list of the items from our command line
  AndroidBitcodeLinker::ABCItemList Items;
  BuildLinkItems(Items, InputFilenames);

  // Save each bitcode in strings
  std::vector<std::string*> BCStrings;

  LinkerConfig Config(Ctx, progname, OutputFilename,
                      Verbose, DisableOptimizations,
                      Strip, StripDebug);

  AndroidBitcodeLinker linker(Config);

  // TODO: Add library path to the linker
  // linker.addPaths(LibPaths);

  if (linker.LinkInAndroidBitcodes(Items, BCStrings))
    return 1;

  // Add bitcode libraries dependents
  for (unsigned i = 0; i < Items.size(); ++i) {
    BitcodeWrapper *wrapper = Items[i].getWrapper();
    if (wrapper != 0 && wrapper->getBitcodeType() == BCHeaderField::BC_SharedObject) {
      std::string soname = wrapper->getSOName();
      if (soname.substr(0,3) == "lib") {
        // Extract the library name
        Libraries.push_back(soname.substr(3,soname.length()-3));
      }
    }
  }

  // Remove any consecutive duplication of the same library
  Libraries.erase(std::unique(Libraries.begin(), Libraries.end()), Libraries.end());

  // Write linked bitcode
  GenerateBitcode(BCStrings, OutputFilename);

  // Operation complete
  OutputRemover.releaseFile();
  return 0;
}
