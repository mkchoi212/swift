//===--- Tools.cpp - Tools Implementations --------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "Tools.h"
#include "ToolChains.h"

#include "swift/Basic/Dwarf.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Range.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/Job.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Option/Options.h"
#include "clang/Driver/Util.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

using namespace swift;
using namespace swift::driver;
using namespace swift::driver::tools;
using namespace llvm::opt;

/// Swift Tool

static void addInputsOfType(ArgStringList &Arguments, const ActionList &Inputs,
                            types::ID InputType) {
  for (auto &Input : Inputs) {
    if (Input->getType() != InputType)
      continue;
    Arguments.push_back(cast<InputAction>(Input)->getInputArg().getValue());
  }
}

static void addInputsOfType(ArgStringList &Arguments, const Job *J,
                            types::ID InputType) {
  if (const Command *Cmd = dyn_cast<Command>(J)) {
    auto &output = Cmd->getOutput().getAnyOutputForType(InputType);
    if (!output.empty())
      Arguments.push_back(output.c_str());
  } else if (const JobList *JL = dyn_cast<JobList>(J)) {
    for (const Job *J : *JL)
      addInputsOfType(Arguments, J, InputType);
  } else {
    llvm_unreachable("Unable to add input arguments for unknown Job class");
  }
}

static void addPrimaryInputsOfType(ArgStringList &Arguments, const Job *J,
                                   types::ID InputType) {
  if (const Command *Cmd = dyn_cast<Command>(J)) {
    auto &outputInfo = Cmd->getOutput();
    if (outputInfo.getPrimaryOutputType() == InputType)
      Arguments.push_back(outputInfo.getPrimaryOutputFilename().c_str());
  } else if (const JobList *JL = dyn_cast<JobList>(J)) {
    for (const Job *J : *JL)
      addPrimaryInputsOfType(Arguments, J, InputType);
  } else {
    llvm_unreachable("Unable to add input arguments for unknown Job class");
  }
}

// FIXME: This should use the logic in clang::driver::Clang::ConstructJob.
static void configureDefaultCPU(const llvm::Triple &triple,
                                ArgStringList &args) {
  switch (triple.getArch()) {
  case llvm::Triple::aarch64:
  case llvm::Triple::aarch64_be:
    if (!triple.isOSDarwin())
      return;
    args.push_back("-target-cpu");
    args.push_back("cyclone");
    args.push_back("-target-feature");
    args.push_back("+neon");
    break;

  case llvm::Triple::arm:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb:
    if (auto CPUStr = triple.getARMCPUForArch()) {
      args.push_back("-target-cpu");
      args.push_back(CPUStr);
    }
    break;

  case llvm::Triple::x86:
  case llvm::Triple::x86_64:
    if (triple.isOSDarwin()) {
      args.push_back("-target-cpu");
      if (triple.getArchName() == "x86_64h") {
        args.push_back("core-avx2");
        args.push_back("-target-feature");
        args.push_back("-rdrnd,-aes,-pclmul,-rtm,-hle,-fsgsbase");
      } else {
        bool is64Bit = (triple.getArch() == llvm::Triple::x86_64);
        args.push_back(is64Bit ? "core2" : "yonah");
      }
    }
    break;
  default:
    break;
  }
}

// Configure the ABI default
static void configureDefaultABI(const llvm::Triple &triple,
                                ArgStringList &args) {
  if (!triple.isOSDarwin())
    return;

  const char *ABIName = nullptr;

  switch (triple.getArch()) {
  case llvm::Triple::aarch64:
    ABIName = "darwinpcs";
    break;
  case llvm::Triple::arm:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb:
    // FIXME: We should use aapcs for M-class processors, EABI,
    //        etc. See Clang's driver. For current targets we just
    //        need to support apcs-gnu.
    ABIName = "apcs-gnu";
    break;
  default:
    // We do not need to specify a -target-abi for other architectures
    // we currently care about.
    return;
  }

  args.push_back("-target-abi");
  args.push_back(ABIName);
}

/// Handle arguments common to all invocations of the frontend (compilation,
/// module-merging, LLDB's REPL, etc).
static void addCommonFrontendArgs(const ToolChain &TC,
                                  const OutputInfo &OI,
                                  CommandOutput *output,
                                  const ArgList &inputArgs,
                                  ArgStringList &arguments) {
  arguments.push_back("-target");
  std::string TripleStr = TC.getTripleString();
  arguments.push_back(inputArgs.MakeArgString(TripleStr));
  const llvm::Triple &Triple = TC.getTriple();

  // Enable address top-byte ignored in the ARM64 backend.
  if (Triple.getArch() == llvm::Triple::aarch64) {
    arguments.push_back("-Xllvm");
    arguments.push_back("-aarch64-use-tbi");
  }

  // Handle the CPU and its preferences.
  if (auto arg = inputArgs.getLastArg(options::OPT_target_cpu))
    arg->render(inputArgs, arguments);
  else
    configureDefaultCPU(Triple, arguments);
  inputArgs.AddAllArgs(arguments, options::OPT_target_feature);

  // Default the ABI based on the triple.
  configureDefaultABI(Triple, arguments);

  if (!OI.SDKPath.empty()) {
    arguments.push_back("-sdk");
    arguments.push_back(inputArgs.MakeArgString(OI.SDKPath));
  }

  inputArgs.AddAllArgs(arguments, options::OPT_I);
  inputArgs.AddAllArgs(arguments, options::OPT_F);

  inputArgs.AddLastArg(arguments, options::OPT_AssertConfig);
  inputArgs.AddLastArg(arguments, options::OPT_autolink_force_load);
  inputArgs.AddLastArg(arguments, options::OPT_color_diagnostics);
  inputArgs.AddLastArg(arguments, options::OPT_enable_app_extension);
  inputArgs.AddLastArg(arguments, options::OPT_g);
  inputArgs.AddLastArg(arguments, options::OPT_import_objc_header);
  inputArgs.AddLastArg(arguments, options::OPT_import_underlying_module);
  inputArgs.AddLastArg(arguments, options::OPT_module_cache_path);
  inputArgs.AddLastArg(arguments, options::OPT_module_link_name);
  inputArgs.AddLastArg(arguments, options::OPT_nostdimport);
  inputArgs.AddLastArg(arguments, options::OPT_parse_stdlib);
  inputArgs.AddLastArg(arguments, options::OPT_resource_dir);
  inputArgs.AddLastArg(arguments, options::OPT_split_objc_selectors);
  inputArgs.AddLastArg(arguments, options::OPT_solver_memory_threshold);
  
  // Pass on any build config options
  inputArgs.AddAllArgs(arguments, options::OPT_D);

  // Pass through the values passed to -Xfrontend.
  inputArgs.AddAllArgValues(arguments, options::OPT_Xfrontend);

  // Pass through any subsystem flags.
  inputArgs.AddAllArgs(arguments, options::OPT_Xllvm);
  inputArgs.AddAllArgs(arguments, options::OPT_Xcc);

  const std::string &moduleDocOutputPath =
      output->getAdditionalOutputForType(types::TY_SwiftModuleDocFile);
  if (!moduleDocOutputPath.empty()) {
    arguments.push_back("-emit-module-doc-path");
    arguments.push_back(moduleDocOutputPath.c_str());
  }

  if (llvm::sys::Process::StandardErrHasColors())
    arguments.push_back("-color-diagnostics");
}

Job *Swift::constructJob(const JobAction &JA, std::unique_ptr<JobList> Inputs,
                         std::unique_ptr<CommandOutput> Output,
                         const ActionList &InputActions, const ArgList &Args,
                         const OutputInfo &OI) const {
  ArgStringList Arguments;

  const char *Exec = getToolChain().getDriver().getSwiftProgramPath().c_str();
  
  // Invoke ourselves in -frontend mode.
  Arguments.push_back("-frontend");

  // Determine the frontend mode option.
  const char *FrontendModeOption = nullptr;
  switch (OI.CompilerMode) {
  case OutputInfo::Mode::StandardCompile:
  case OutputInfo::Mode::SingleCompile: {
    switch (Output->getPrimaryOutputType()) {
    case types::TY_Object:
      FrontendModeOption = "-c";
      break;
    case types::TY_RawSIL:
      FrontendModeOption = "-emit-silgen";
      break;
    case types::TY_SIL:
      FrontendModeOption = "-emit-sil";
      break;
    case types::TY_LLVM_IR:
      FrontendModeOption = "-emit-ir";
      break;
    case types::TY_LLVM_BC:
      FrontendModeOption = "-emit-bc";
      break;
    case types::TY_Assembly:
      FrontendModeOption = "-S";
      break;
    case types::TY_SwiftModuleFile:
      // Since this is our primary output, we need to specify the option here.
      FrontendModeOption = "-emit-module";
      break;
    case types::TY_Nothing:
      // We were told to output nothing, so get the last mode option and use that.
      if (const Arg *A = Args.getLastArg(options::OPT_modes_Group))
        FrontendModeOption = A->getSpelling().data();
      else
        llvm_unreachable("We were told to perform a standard compile, "
                         "but no mode option was passed to the driver.");
      break;
    case types::TY_Swift:
    case types::TY_dSYM:
    case types::TY_Dependencies:
    case types::TY_SwiftModuleDocFile:
    case types::TY_ClangModuleFile:
    case types::TY_SerializedDiagnostics:
    case types::TY_ObjCHeader:
    case types::TY_Image:
      llvm_unreachable("Output type can never be primary output.");
    case types::TY_INVALID:
    case types::TY_LAST:
      llvm_unreachable("Invalid type ID");
    }
    break;
  }
  case OutputInfo::Mode::Immediate:
    FrontendModeOption = "-interpret";
    break;
  case OutputInfo::Mode::REPL:
    FrontendModeOption = "-repl";
    break;
  }

  assert(FrontendModeOption != nullptr && "No frontend mode option specified!");
  
  Arguments.push_back(FrontendModeOption);
  
  assert(Inputs->empty() &&
         "The Swift frontend does not expect to be fed any input Jobs!");

  // Add input arguments.
  switch (OI.CompilerMode) {
  case OutputInfo::Mode::StandardCompile: {
    assert(InputActions.size() == 1 &&
           "The Swift frontend expects exactly one input (the primary file)!");

    const InputAction *IA = dyn_cast<InputAction>(InputActions[0]);
    assert(IA && "Only InputActions can be passed as inputs!");
    const Arg &PrimaryInputArg = IA->getInputArg();
    bool FoundPrimaryInput = false;

    for (const Arg *A : make_range(Args.filtered_begin(options::OPT_INPUT),
                                   Args.filtered_end())) {
      Option Opt = A->getOption();
      if (A->getOption().matches(options::OPT_INPUT)) {
        // See if this input should be passed with -primary-file.
        if (!FoundPrimaryInput && PrimaryInputArg.getIndex() == A->getIndex()) {
          Arguments.push_back("-primary-file");
          FoundPrimaryInput = true;
        }
        Arguments.push_back(A->getValue());
      }
    }
    break;
  }
  case OutputInfo::Mode::SingleCompile:
  case OutputInfo::Mode::Immediate: {
    for (const Action *A : InputActions) {
      const InputAction *IA = dyn_cast<InputAction>(A);
      assert(IA && "Only InputActions can be passed as inputs!");

      IA->getInputArg().render(Args, Arguments);
    }
    break;
  }
  case OutputInfo::Mode::REPL: {
    assert(InputActions.empty() && "REPL mode accepts no inputs!");
    break;
  }
  }

  if (!Args.hasArg(options::OPT_parse_stdlib))
    Arguments.push_back("-enable-objc-attr-requires-foundation-module");

  addCommonFrontendArgs(getToolChain(), OI, Output.get(), Args, Arguments);

  // Pass the optimization level down to the frontend.
  Args.AddLastArg(Arguments, options::OPT_O_Group);

  if (Args.hasArg(options::OPT_parse_as_library) ||
      Args.hasArg(options::OPT_emit_library))
    Arguments.push_back("-parse-as-library");

  Args.AddLastArg(Arguments, options::OPT_parse_sil);
  Args.AddAllArgs(Arguments, options::OPT_l, options::OPT_framework);

  Arguments.push_back("-module-name");
  Arguments.push_back(Args.MakeArgString(OI.ModuleName));

  const std::string &ModuleOutputPath =
    Output->getAdditionalOutputForType(types::ID::TY_SwiftModuleFile);
  if (!ModuleOutputPath.empty()) {
    Arguments.push_back("-emit-module-path");
    Arguments.push_back(ModuleOutputPath.c_str());
  }

  const std::string &ObjCHeaderOutputPath =
    Output->getAdditionalOutputForType(types::ID::TY_ObjCHeader);
  if (!ObjCHeaderOutputPath.empty()) {
    assert(OI.CompilerMode == OutputInfo::Mode::SingleCompile &&
           "The Swift tool should only emit an Obj-C header in single compile"
           "mode!");

    Arguments.push_back("-emit-objc-header-path");
    Arguments.push_back(ObjCHeaderOutputPath.c_str());
  }

  const std::string &SerializedDiagnosticsPath =
    Output->getAdditionalOutputForType(types::TY_SerializedDiagnostics);
  if (!SerializedDiagnosticsPath.empty()) {
    Arguments.push_back("-serialize-diagnostics-path");
    Arguments.push_back(SerializedDiagnosticsPath.c_str());
  }

  const std::string &DependenciesPath =
    Output->getAdditionalOutputForType(types::TY_Dependencies);
  if (!DependenciesPath.empty()) {
    Arguments.push_back("-emit-dependencies-path");
    Arguments.push_back(DependenciesPath.c_str());
  }

  // Add the output file argument if necessary.
  if (Output->getPrimaryOutputType() != types::TY_Nothing) {
    Arguments.push_back("-o");
    Arguments.push_back(Output->getPrimaryOutputFilename().c_str());
  }

  if (OI.CompilerMode == OutputInfo::Mode::Immediate)
    Args.AddLastArg(Arguments, options::OPT__DASH_DASH);

  return new Command(JA, *this, std::move(Inputs), std::move(Output), Exec,
                     Arguments);
}


Job *MergeModule::constructJob(const JobAction &JA,
                               std::unique_ptr<JobList> Inputs,
                               std::unique_ptr<CommandOutput> Output,
                               const ActionList &InputActions,
                               const ArgList &Args,
                               const OutputInfo &OI) const {
  ArgStringList Arguments;

  const char *Exec = getToolChain().getDriver().getSwiftProgramPath().c_str();

  // Invoke ourself in -frontend mode.
  Arguments.push_back("-frontend");

  // We just want to emit a module, so pass -emit-module without any other
  // mode options.
  Arguments.push_back("-emit-module");

  size_t origLen = Arguments.size();
  (void)origLen;
  addInputsOfType(Arguments, Inputs.get(), types::TY_SwiftModuleFile);
  addInputsOfType(Arguments, InputActions, types::TY_SwiftModuleFile);
  assert(Arguments.size() - origLen >= Inputs->size() + InputActions.size());
  assert((Arguments.size() - origLen == Inputs->size() ||
          !InputActions.empty()) &&
         "every input to MergeModule must generate a swiftmodule");

  // Tell all files to parse as library, which is necessary to load them as
  // serialized ASTs.
  Arguments.push_back("-parse-as-library");

  addCommonFrontendArgs(getToolChain(), OI, Output.get(), Args, Arguments);

  Arguments.push_back("-module-name");
  Arguments.push_back(Args.MakeArgString(OI.ModuleName));

  assert(Output->getPrimaryOutputType() == types::TY_SwiftModuleFile &&
         "The MergeModule tool only produces swiftmodule files!");

  const std::string &ObjCHeaderOutputPath =
    Output->getAdditionalOutputForType(types::TY_ObjCHeader);
  if (!ObjCHeaderOutputPath.empty()) {
    Arguments.push_back("-emit-objc-header-path");
    Arguments.push_back(ObjCHeaderOutputPath.c_str());
  }

  Arguments.push_back("-o");
  Arguments.push_back(Args.MakeArgString(Output->getPrimaryOutputFilename()));

  return new Command(JA, *this, std::move(Inputs), std::move(Output), Exec,
                     Arguments);
}

bool LLDB::isPresentRelativeToDriver() const {
  if (!Bits.DidCheckRelativeToDriver) {
    const Driver &D = getToolChain().getDriver();
    Path = findRelativeExecutable(D.getSwiftProgramPath(), "lldb");
    Bits.DidCheckRelativeToDriver = true;
  }
  return !Path.empty();
}

Job *LLDB::constructJob(const JobAction &JA,
                        std::unique_ptr<JobList> Inputs,
                        std::unique_ptr<CommandOutput> Output,
                        const ActionList &InputActions,
                        const ArgList &Args,
                        const OutputInfo &OI) const {
  assert(Inputs->empty());
  assert(InputActions.empty());

  // Squash important frontend options into a single argument for LLDB.
  ArgStringList FrontendArgs;
  addCommonFrontendArgs(getToolChain(), OI, Output.get(), Args, FrontendArgs);

  std::string SingleArg = "--repl=";
  {
    llvm::raw_string_ostream os(SingleArg);
    Command::printArguments(os, FrontendArgs);
  }

  ArgStringList Arguments;
  Arguments.push_back(Args.MakeArgString(std::move(SingleArg)));

  const char *Exec;
  if (isPresentRelativeToDriver()) {
    Exec = Path.c_str();
  } else {
    auto &TC = getToolChain();
    Exec = Args.MakeArgString(TC.getProgramPath("lldb"));
  }

  return new Command(JA, *this, std::move(Inputs), std::move(Output), Exec,
                     Arguments);
}

Job *Dsymutil::constructJob(const JobAction &JA,
                            std::unique_ptr<JobList> Inputs,
                            std::unique_ptr<CommandOutput> Output,
                            const ActionList &InputActions,
                            const ArgList &Args,
                            const OutputInfo &OI) const {
  assert(Inputs->size() == 1);
  assert(InputActions.empty());
  assert(Output->getPrimaryOutputType() == types::TY_dSYM);

  ArgStringList Arguments;

  auto inputJob = cast<Command>(Inputs->front());
  StringRef inputPath = inputJob->getOutput().getPrimaryOutputFilename();
  Arguments.push_back(Args.MakeArgString(inputPath));

  Arguments.push_back("-o");
  Arguments.push_back(Args.MakeArgString(Output->getPrimaryOutputFilename()));

  std::string Exec = getToolChain().getProgramPath("dsymutil");
  return new Command(JA, *this, std::move(Inputs), std::move(Output),
                     Args.MakeArgString(Exec), Arguments);
}

/// Darwin Tools

llvm::Triple::ArchType darwin::getArchTypeForDarwinArchName(StringRef Arch) {
  return llvm::StringSwitch<llvm::Triple::ArchType>(Arch)
    .Cases("i386", "i486", "i486SX", "i586", "i686", llvm::Triple::x86)
    .Cases("pentium", "pentpro", "pentIIm3", "pentIIm5", "pentium4",
           llvm::Triple::x86)

    .Case("x86_64", llvm::Triple::x86_64)

    .Case("arm64", llvm::Triple::aarch64)

    .Cases("arm", "armv4t", "armv5", "armv6", "armv6m", llvm::Triple::arm)
    .Cases("armv7", "armv7em", "armv7f", "armv7k", "armv7m", llvm::Triple::arm)
    .Cases("armv7s", "xscale", llvm::Triple::arm)

    .Default(llvm::Triple::UnknownArch);
}

void darwin::DarwinTool::anchor() {}

void darwin::DarwinTool::AddDarwinArch(const ArgList &Args,
                                       ArgStringList &CmdArgs) const {
  StringRef ArchName = getDarwinToolChain().getDarwinArchName(Args);

  CmdArgs.push_back("-arch");
  CmdArgs.push_back(Args.MakeArgString(ArchName));
}

static void addVersionString(const ArgList &inputArgs, ArgStringList &arguments,
                             unsigned major, unsigned minor, unsigned micro) {
  llvm::SmallString<8> buf;
  llvm::raw_svector_ostream os{buf};
  os << major << '.' << minor << '.' << micro;
  arguments.push_back(inputArgs.MakeArgString(os.str()));
}

Job *darwin::Linker::constructJob(const JobAction &JA,
                                  std::unique_ptr<JobList> Inputs,
                                  std::unique_ptr<CommandOutput> Output,
                                  const ActionList &InputActions,
                                  const ArgList &Args,
                                  const OutputInfo &OI) const {
  assert(Output->getPrimaryOutputType() == types::TY_Image &&
         "Invalid linker output type.");

  const toolchains::Darwin &TC = getDarwinToolChain();
  const Driver &D = TC.getDriver();
  const llvm::Triple &Triple = TC.getTriple();

  ArgStringList Arguments;
  addPrimaryInputsOfType(Arguments, Inputs.get(), types::TY_Object);
  addInputsOfType(Arguments, InputActions, types::TY_Object);

  if (OI.ShouldGenerateDebugInfo) {
    Arguments.push_back("-add_ast_path");

    size_t argCount = Arguments.size();
    if (OI.CompilerMode == OutputInfo::Mode::SingleCompile)
      addInputsOfType(Arguments, Inputs.get(), types::TY_SwiftModuleFile);
    else
      addPrimaryInputsOfType(Arguments, Inputs.get(),
                             types::TY_SwiftModuleFile);
    assert(argCount + 1 == Arguments.size() && "no swiftmodule found for -g");
    (void)argCount;
  }

  switch (cast<LinkJobAction>(JA).getKind()) {
  case LinkKind::None:
    llvm_unreachable("invalid link kind");
  case LinkKind::Executable:
    // The default for ld; no extra flags necessary.
    break;
  case LinkKind::DynamicLibrary:
    Arguments.push_back("-dylib");
    break;
  }

  assert(Triple.isOSDarwin());
  bool wantsObjCRuntime =
    Triple.isiOS() ? Triple.isOSVersionLT(8) : Triple.isMacOSXVersionLT(10, 10);
  if (Args.hasFlag(options::OPT_link_objc_runtime,
                   options::OPT_no_link_objc_runtime,
                   /*default=*/wantsObjCRuntime)) {
    llvm::SmallString<128> ARCLiteLib(D.getSwiftProgramPath());
    llvm::sys::path::remove_filename(ARCLiteLib); // 'swift'
    llvm::sys::path::remove_filename(ARCLiteLib); // 'bin'
    llvm::sys::path::append(ARCLiteLib, "lib", "arc", "libarclite_");
    ARCLiteLib += getPlatformNameForTriple(Triple);
    ARCLiteLib += ".a";

    Arguments.push_back("-force_load");
    Arguments.push_back(Args.MakeArgString(ARCLiteLib));
  }

  Args.AddAllArgValues(Arguments, options::OPT_Xlinker);
  Args.AddAllArgs(Arguments, options::OPT_linker_option_Group);
  Args.AddAllArgs(Arguments, options::OPT_F);

  if (Args.hasArg(options::OPT_enable_app_extension)) {
    // Keep this string fixed in case the option used by the
    // compiler itself changes.
    Arguments.push_back("-application_extension");
  }

  if (!OI.SDKPath.empty()) {
    Arguments.push_back("-syslibroot");
    Arguments.push_back(Args.MakeArgString(OI.SDKPath));
  }

  Arguments.push_back("-lSystem");
  AddDarwinArch(Args, Arguments);

  // Add the runtime library link path, which is platform-specific and found
  // relative to the compiler.
  // FIXME: Duplicated from CompilerInvocation, but in theory the runtime
  // library link path and the standard library module import path don't
  // need to be the same.
  llvm::SmallString<128> RuntimeLibPath;

  if (const Arg *A = Args.getLastArg(options::OPT_resource_dir)) {
    RuntimeLibPath = A->getValue();
  } else {
    RuntimeLibPath = D.getSwiftProgramPath();
    llvm::sys::path::remove_filename(RuntimeLibPath); // remove /swift
    llvm::sys::path::remove_filename(RuntimeLibPath); // remove /bin
    llvm::sys::path::append(RuntimeLibPath, "lib", "swift");
  }
  llvm::sys::path::append(RuntimeLibPath,
                          getPlatformNameForTriple(TC.getTriple()));
  Arguments.push_back("-L");
  Arguments.push_back(Args.MakeArgString(RuntimeLibPath));

  // FIXME: We probably shouldn't be adding an rpath here unless we know ahead
  // of time the standard library won't be copied.
  Arguments.push_back("-rpath");
  Arguments.push_back(Args.MakeArgString(RuntimeLibPath));

  // FIXME: Properly handle deployment targets.
  assert(Triple.isiOS() || Triple.isMacOSX());
  if (Triple.isiOS()) {
    if (tripleIsiOSSimulator(Triple))
      Arguments.push_back("-ios_simulator_version_min");
    else
      Arguments.push_back("-iphoneos_version_min");
    unsigned major, minor, micro;
    Triple.getiOSVersion(major, minor, micro);
    addVersionString(Args, Arguments, major, minor, micro);
  } else {
    Arguments.push_back("-macosx_version_min");
    unsigned major, minor, micro;
    Triple.getMacOSXVersion(major, minor, micro);
    addVersionString(Args, Arguments, major, minor, micro);
  }

  Arguments.push_back("-no_objc_category_merging");

  // This should be the last option, for convenience in checking output.
  Arguments.push_back("-o");
  Arguments.push_back(Output->getPrimaryOutputFilename().c_str());

  std::string Exec = getToolChain().getProgramPath("ld");

  return new Command(JA, *this, std::move(Inputs), std::move(Output),
                     Args.MakeArgString(Exec), Arguments);
}
