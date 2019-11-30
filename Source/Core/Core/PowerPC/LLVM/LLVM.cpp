// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"


#include "Core/PowerPC/LLVM/LLVM.h"

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"

#include <sstream>
#include <chrono>
#include <iostream>

using namespace llvm;
using namespace llvm::orc;

static ExitOnError ExitOnErr;

struct LLVMJit::LLVMJitCtx
{
  using CommonCallback = void (*)(UGeckoInstruction);
  using ConditionalCallback = bool (*)(u32);

  LLVMJitCtx():
    TSC_(std::make_unique<LLVMContext>())
  {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    // TODO: proper error handling
    auto JTMB = ExitOnErr(JITTargetMachineBuilder::detectHost());
    JTMB.setCodeGenOptLevel(CodeGenOpt::Default);
    LLJITBuilder B;
    B.setJITTargetMachineBuilder(std::move(JTMB));
    JIT_ = ExitOnErr(B.create());
    // I really don't understand how we are supposed to get the target machine
    // from the LLJIT object, so recreating it here...
    TM_ = ExitOnErr(ExitOnErr(JITTargetMachineBuilder::detectHost()).createTargetMachine());

		auto Gen = ExitOnErr(DynamicLibrarySearchGenerator::GetForCurrentProcess(
				JIT_->getDataLayout().getGlobalPrefix()));
    JIT_->getMainJITDylib().setGenerator(Gen);

    SMDiagnostic Err;
    std::string BCPath = File::GetExeDirectory() + "/llvmjitrt.bc";
    RTM_ = parseIRFile(BCPath,
      Err, LCtx(), false);
    if (!RTM_) {
      Err.print("jit", errs());
      exit(1);
    }
    RTM_->setSourceFileName(StringRef{});
    // Inline internally used functions
    for (Function& F: *RTM_) {
      if (!F.empty() && !F.hasFnAttribute(Attribute::AlwaysInline)) {
        F.addFnAttr(Attribute::AlwaysInline);
      }
    }
    legacy::PassManager MPM;
    MPM.add(createAlwaysInlinerLegacyPass());
    MPM.run(*RTM_);
    // "Internalize" remaining functions
    for (Function& F: *RTM_) {
      if (!F.empty()) {
        F.setLinkage(GlobalValue::InternalLinkage);
      }
    }
  }

  ~LLVMJitCtx() {
    JIT_.reset();
  }

  struct FuncBuilder
  {
    FuncBuilder(ThreadSafeModule&& TSM, Function* F, LLVMJitCtx& JCtx):
      TSM_(std::move(TSM)),
      IRB(BasicBlock::Create(LCtx(), "entry", F)),
      JCtx_(JCtx)
    {
      auto& Ctx = LCtx();
      // Declare runtime functions
      // CommonCallback = void(u32)
      // ConditionalCallback = bool(u32)
      FCommonCBTy = FunctionType::get(
        Type::getVoidTy(Ctx), {Type::getInt32Ty(Ctx)}, false);
      FCondCBTy = FunctionType::get(
        Type::getInt1Ty(Ctx), {Type::getInt32Ty(Ctx)}, false);

      auto* EntryBB = IRB.GetInsertBlock();
      ExitBB = BasicBlock::Create(Ctx, "exit", F);
      IRB.SetInsertPoint(ExitBB);
      IRB.CreateRetVoid();

      IRB.SetInsertPoint(EntryBB);

      auto& M = LM();
      for (Function const& RF: RTM()) {
        if (RF.empty()) {
          Function *NF =
            Function::Create(RF.getFunctionType(), RF.getLinkage(),
                RF.getAddressSpace(), RF.getName(), &M);
          NF->copyAttributesFrom(&RF);
          VMap_[&RF] = NF;
        }
      }
      for (Module::const_global_iterator I = RTM().global_begin(), E = RTM().global_end();
          I != E; ++I) {
        GlobalVariable *GV = new GlobalVariable(M,
            I->getValueType(),
            I->isConstant(), I->getLinkage(),
            (Constant*) nullptr, I->getName(),
            (GlobalVariable*) nullptr,
            I->getThreadLocalMode(),
            I->getType()->getAddressSpace());
        GV->copyAttributesFrom(&*I);
        VMap_[&*I] = GV;
      }
      for (Module::const_global_iterator I = RTM().global_begin(), E = RTM().global_end();
          I != E; ++I) {
        if (I->isDeclaration())
          continue;

        GlobalVariable *GV = cast<GlobalVariable>(VMap_[&*I]);
        if (I->hasInitializer())
          GV->setInitializer(MapValue(I->getInitializer(), VMap_));
      }
    }

    void CallDirectFunc(CommonCallback const CB, UGeckoInstruction const I);
    void CallInstrFunc(UGeckoInstruction const I);
    void CallFunc(Constant* F, UGeckoInstruction const I);
    void CallCondFunc(Function* F, u32 const V);
    bool HandleFunctionHooking(u32 address, JitState& js);

    void CallFunc(const char* Name, UGeckoInstruction const I)
    {
      return CallFunc(GetRTFunc(Name), I);
    }

    void CallCondFunc(const char* Name, u32 const V)
    {
      return CallCondFunc(GetRTFunc(Name), V);
    }

    Function* GetRTFunc(const char* Name) {
      auto& M = LM();
      Function* Ret = M.getFunction(Name);
      if (Ret) {
        return Ret;
      }

      //errs() << "GetRTFunc " << Name << "\n";
      Function* RTF = RTM().getFunction(Name);
      assert(RTF && "unable to find runtime function in RT imported module!");
      Ret = Function::Create(RTF->getFunctionType(), Function::InternalLinkage, RTF->getName(), M);
      auto ItArgOrg = RTF->arg_begin();
      auto ItArgNew = Ret->arg_begin();
      auto const ItArgNewEnd = Ret->arg_end();
      for (; ItArgNew != ItArgNewEnd; ++ItArgNew, ++ItArgOrg) {
        ItArgNew->setName(ItArgOrg->getName());
        VMap_[&*ItArgOrg] = &*ItArgNew;
      }
      SmallVector<ReturnInst*, 8> Rets;
      CloneFunctionInto(Ret, RTF, VMap_, true, Rets);
      if (RTF->hasPersonalityFn())
        Ret->setPersonalityFn(llvm::MapValue(RTF->getPersonalityFn(), VMap_));

      ItArgOrg = RTF->arg_begin();
      for (; ItArgOrg != RTF->arg_end(); ++ItArgOrg) {
        VMap_.erase(&*ItArgOrg);
      }
      return Ret;
    }

    void Return() {
      IRB.CreateBr(ExitBB);
    }

    Module const& RTM() const { return JCtx_.RTM(); }
    Module& LM() { return *TSM_.getModule(); }
    LLVMContext& LCtx() { return LM().getContext(); }
    Function* LF() { return IRB.GetInsertBlock()->getParent(); }

    ThreadSafeModule TSM_;
    // Function types within modules.
    // TODO: move that to LLVMJitCtx!
    FunctionType* FCommonCBTy;
    FunctionType* FCondCBTy;
    FunctionType* FRunTy;

    BasicBlock* ExitBB;
    IRBuilder<> IRB;
    LLVMJitCtx& JCtx_;
    ValueToValueMapTy VMap_;
  };

  FuncBuilder NewFunc(u32 address)
  {
    SMDiagnostic Err;
    auto& Ctx = LCtx();
    auto M = std::make_unique<Module>("", Ctx);
    M->setDataLayout(JIT_->getDataLayout());
    M->setTargetTriple(TM_->getTargetTriple().str());

    auto* FRunTy = FunctionType::get(
      Type::getVoidTy(Ctx), {}, false);
    auto* F = Function::Create(FRunTy, Function::ExternalLinkage, "F." + std::to_string(CurId_++), M.get());
    return FuncBuilder{ThreadSafeModule{std::move(M), TSC_}, F, *this};
  }

  uint8_t* Generate(FuncBuilder& FB) {
    auto& M = FB.LM();
    if (verifyModule(M, &errs())) {
      errs() << M << "\n";
      report_fatal_error("invalid module!");
    }
    legacy::PassManager MPM;
    // Add an appropriate TargetLibraryInfo pass for the module's triple.
    TargetLibraryInfoImpl TLII(Triple{M.getTargetTriple()});
    MPM.add(new TargetLibraryInfoWrapperPass(TLII));
    // Add internal analysis passes from the target machine.
    MPM.add(createTargetTransformInfoWrapperPass(TM_->getTargetIRAnalysis()));

#if 0
    PassManagerBuilder Builder;
    Builder.OptLevel = 3;
    Builder.SizeLevel = 0;
    Builder.LoopVectorize = true;
    Builder.Inliner = createAlwaysInlinerLegacyPass();
    Builder.populateModulePassManager(MPM);
#else
    MPM.add(createAlwaysInlinerLegacyPass());
    MPM.add(createGlobalDCEPass());
    MPM.add(createSROAPass());
    MPM.add(createInstructionCombiningPass(true /* ExpensiveCombines */));
    MPM.add(createCFGSimplificationPass());
#endif
    MPM.add(createVerifierPass());
    MPM.run(M);
    StringRef Name = FB.LF()->getName();
    ExitOnErr(JIT_->addIRModule(std::move(FB.TSM_)));
    auto S = ExitOnErr(JIT_->lookup(Name));
    return (uint8_t*)S.getAddress();
  }

  Module const& RTM() const { return *RTM_; }

private:
  LLVMContext& LCtx() { return *TSC_.getContext(); }

  ThreadSafeContext TSC_;
  std::unique_ptr<Module> RTM_;
  // LLVM jit context
  std::unique_ptr<llvm::orc::LLJIT> JIT_;
  std::unique_ptr<TargetMachine> TM_;
  unsigned CurId_ = 0;
};

LLVMJit::LLVMJit() = default;

LLVMJit::~LLVMJit() = default;

void LLVMJit::Init()
{
  jo.enableBlocklink = false;

  m_block_cache.Init();
  UpdateMemoryOptions();

  code_block.m_stats = &js.st;
  code_block.m_gpa = &js.gpa;
  code_block.m_fpa = &js.fpa;

  JitCtx_.reset(new LLVMJitCtx{});
}

void LLVMJit::Shutdown()
{
  m_block_cache.Shutdown();
  JitCtx_.reset();
}

void LLVMJit::ExecuteOneBlock()
{
  const u8* normal_entry = m_block_cache.Dispatch();
  if (!normal_entry)
  {
    Jit(PC);
    return;
  }

  typedef void(*RunFunc)();
  RunFunc F = (RunFunc)(normal_entry);
  F();
}

void LLVMJit::Run()
{
  const CPU::State* state_ptr = CPU::GetStatePtr();
  while (CPU::GetState() == CPU::State::Running)
  {
    // Start new timing slice
    // NOTE: Exceptions may change PC
    CoreTiming::Advance();

    do
    {
      ExecuteOneBlock();
    } while (PowerPC::ppcState.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void LLVMJit::SingleStep()
{
  // Enter new timing slice
  CoreTiming::Advance();
  ExecuteOneBlock();
}

void LLVMJit::LLVMJitCtx::FuncBuilder::CallDirectFunc(CommonCallback const CB, UGeckoInstruction const I)
{
  auto& Ctx = LCtx();
  Constant* F = ConstantExpr::getIntToPtr(
      ConstantInt::get(Type::getInt64Ty(Ctx), (uintptr_t)CB),
      PointerType::get(FCommonCBTy, 0));
  CallFunc(F, I);
}

void LLVMJit::LLVMJitCtx::FuncBuilder::CallInstrFunc(UGeckoInstruction const I)
{
  // Hackery things happening here... (well, not only here to be fair).
  auto* Info = PPCTables::GetOpInfo(I);
  const char* Name = Info->opname;
  // Mangle name by hand...
  std::string MangledName;
  {
    std::stringstream ss;
    // _ZN11Interpreter5mffsxE17UGeckoInstruction
    ss << "_ZN11Interpreter" << strlen(Name) << Name << "E17UGeckoInstruction";
    MangledName = ss.str();
  }
  //Function* F = LM().getFunction(MangledName);
  //assert(F && "unable to find instruction in the runtime IR");
  //CallFunc(F, I);
  CallFunc(MangledName.c_str(), I);
}

void LLVMJit::LLVMJitCtx::FuncBuilder::CallFunc(Constant* F, UGeckoInstruction const I)
{
  IRB.CreateCall(FCommonCBTy, F,
    {ConstantInt::get(Type::getInt32Ty(LCtx()), I.hex)});
}

void LLVMJit::LLVMJitCtx::FuncBuilder::CallCondFunc(Function* F, u32 const V)
{
  auto& Ctx = LCtx();
  Value* Ret = IRB.CreateCall(F,
    {ConstantInt::get(Type::getInt32Ty(Ctx), V)});
  BasicBlock* ContBB = BasicBlock::Create(Ctx, "", ExitBB->getParent());
  IRB.CreateCondBr(Ret, ExitBB, ContBB);
  IRB.SetInsertPoint(ContBB);
}

bool LLVMJit::LLVMJitCtx::FuncBuilder::HandleFunctionHooking(u32 address, JitState& js)
{
  return HLE::ReplaceFunctionIfPossible(address, [&](u32 function, HLE::HookType type) {
    CallFunc("WritePC", address);
    CallDirectFunc(Interpreter::HLEFunction, function);

    if (type != HLE::HookType::Replace)
      return false;

    CallFunc("EndBlock", js.downcountAmount);
    IRB.CreateBr(ExitBB);
    return true;
  });
}

void LLVMJit::Jit(u32 address)
{
  const u32 nextPC = analyzer.Analyze(PC, &code_block, &m_code_buffer, m_code_buffer.size());
  if (code_block.m_memory_exception)
  {
    // Address of instruction could not be translated
    NPC = nextPC;
    PowerPC::ppcState.Exceptions |= EXCEPTION_ISI;
    PowerPC::CheckExceptions();
    WARN_LOG(POWERPC, "ISI exception at 0x%08x", nextPC);
    return;
  }

  //auto start = std::chrono::steady_clock::now();
  JitBlock* b = m_block_cache.AllocateBlock(PC);

  js.blockStart = PC;
  js.firstFPInstructionFound = false;
  js.fifoBytesSinceCheck = 0;
  js.downcountAmount = 0;
  js.curBlock = b;

  auto FB = JitCtx_->NewFunc(address);

  for (u32 i = 0; i < code_block.m_num_instructions; i++)
  {
    PPCAnalyst::CodeOp& op = m_code_buffer[i];

    js.downcountAmount += op.opinfo->numCycles;

    if (FB.HandleFunctionHooking(op.address, js))
      break;

    if (!op.skip)
    {
      const bool breakpoint = SConfig::GetInstance().bEnableDebugging &&
                              PowerPC::breakpoints.IsAddressBreakPoint(op.address);
      const bool check_fpu = (op.opinfo->flags & FL_USE_FPU) && !js.firstFPInstructionFound;
      const bool endblock = (op.opinfo->flags & FL_ENDBLOCK) != 0;
      const bool memcheck = (op.opinfo->flags & FL_LOADSTORE) && jo.memcheck;
      const bool idle_loop = op.branchIsIdleLoop;

      if (breakpoint)
      {
        FB.CallFunc("WritePC", op.address);
        FB.CallCondFunc("CheckBreakpoint", js.downcountAmount);
      }

      if (check_fpu)
      {
        FB.CallFunc("WritePC", op.address);
        FB.CallCondFunc("CheckFPU", js.downcountAmount);
        js.firstFPInstructionFound = true;
      }

      if (endblock || memcheck)
        FB.CallFunc("WritePC", op.address);
      //FB.CallDirectFunc(PPCTables::GetInterpreterOp(op.inst), op.inst);
      FB.CallInstrFunc(op.inst);
      if (memcheck)
        FB.CallCondFunc("CheckDSI", js.downcountAmount);
      if (idle_loop)
        FB.CallCondFunc("CheckIdle", js.blockStart);
      if (endblock)
        FB.CallFunc("EndBlock", js.downcountAmount);
    }
  }
  if (code_block.m_broken)
  {
    FB.CallFunc("WriteBrokenBlockNPC", nextPC);
    FB.CallFunc("EndBlock", js.downcountAmount);
  }
  FB.Return();
  //auto end = std::chrono::steady_clock::now();
  //auto elapsed =
    //std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  //std::cerr << "IR generation time: " << elapsed.count() << "µs\n";

  //start = std::chrono::steady_clock::now();
  b->checkedEntry = JitCtx_->Generate(FB);
  //end = std::chrono::steady_clock::now();
  //elapsed =
    //std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  //std::cerr << "IR opt+compilation time: " << elapsed.count() << "µs\n";
  b->normalEntry = b->checkedEntry;

  b->codeSize = 0;
  b->originalSize = code_block.m_num_instructions;

  m_block_cache.FinalizeBlock(*b, jo.enableBlocklink, code_block.m_physical_addresses);
}

void LLVMJit::ClearCache()
{
  JitCtx_.reset(new LLVMJitCtx{});
  m_block_cache.Clear();
  UpdateMemoryOptions();
}
