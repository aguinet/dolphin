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
#include "llvm/IR/LLVMContext.h"

#include "Core/PowerPC/LLVM/LLVM.h"

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"

using namespace llvm;
using namespace llvm::orc;

static ExitOnError ExitOnErr;

struct LLVMJit::LLVMJitCtx
{
  using CommonCallback = void (*)(UGeckoInstruction);
  using ConditionalCallback = bool (*)(u32);

  LLVMJitCtx():
    TSM_(std::make_unique<LLVMContext>())
  {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    auto& Ctx = LCtx();
    // Declare runtime functions types
    // CommonCallback = void(u32)
    // ConditionalCallback = bool(u32)
    FRunTy = FunctionType::get(
      Type::getVoidTy(Ctx), {}, false);
    FCommonCBTy = FunctionType::get(
      Type::getVoidTy(Ctx), {Type::getInt32Ty(Ctx)}, false);
    FCondCBTy = FunctionType::get(
      Type::getInt1Ty(Ctx), {Type::getInt32Ty(Ctx)}, false);

    // TODO: proper error handling
    JIT_ = ExitOnErr(LLJITBuilder().create());
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

      auto* EntryBB = IRB.GetInsertBlock();
      ExitBB = BasicBlock::Create(Ctx, "exit", F);
      IRB.SetInsertPoint(ExitBB);
      IRB.CreateRetVoid();

      IRB.SetInsertPoint(EntryBB);
    }

    void CallFunc(CommonCallback const CB, UGeckoInstruction const I);
    void CallFunc(ConditionalCallback const CB, u32 const V);
    bool HandleFunctionHooking(u32 address, JitState& js);

    void Return() {
      IRB.CreateBr(ExitBB);
    }

    Module& LM() { return *TSM_.getModule(); }
    LLVMContext& LCtx() { return LM().getContext(); }
    Function* LF() { return IRB.GetInsertBlock()->getParent(); }
    LLVMJitCtx& JCtx() { return JCtx_; }

    ThreadSafeModule TSM_;
    // Function types within modules
    BasicBlock* ExitBB;
    IRBuilder<> IRB;
    LLVMJitCtx& JCtx_;
  };

  FuncBuilder NewFunc(u32 address)
  {
    auto& Ctx = LCtx();
    auto M = std::make_unique<Module>("", Ctx);
    Function* F = Function::Create(FRunTy, Function::ExternalLinkage,
      "F." + std::to_string(++CurId_), M.get());
    return FuncBuilder{ThreadSafeModule{std::move(M), TSM_}, F, *this};
  }

  uint8_t* Generate(FuncBuilder& FB) {
    StringRef Name = FB.LF()->getName();
    ExitOnErr(JIT_->addIRModule(ThreadSafeModule{std::move(FB.TSM_)}));
    auto S = ExitOnErr(JIT_->lookup(Name));
    return (uint8_t*)S.getAddress();
  }

  llvm::FunctionType* FCommonCBTy;
  llvm::FunctionType* FCondCBTy;
  llvm::FunctionType* FRunTy;

private:
  LLVMContext& LCtx() { return *TSM_.getContext(); }

  // LLVM jit context
  ThreadSafeContext TSM_;
  std::unique_ptr<llvm::orc::LLJIT> JIT_;
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

static void EndBlock(UGeckoInstruction data)
{
  PC = NPC;
  PowerPC::ppcState.downcount -= data.hex;
}

static void WritePC(UGeckoInstruction data)
{
  PC = data.hex;
  NPC = data.hex + 4;
}

static void WriteBrokenBlockNPC(UGeckoInstruction data)
{
  NPC = data.hex;
}

static bool CheckFPU(u32 data)
{
  if (!MSR.FP)
  {
    PowerPC::ppcState.Exceptions |= EXCEPTION_FPU_UNAVAILABLE;
    PowerPC::CheckExceptions();
    PowerPC::ppcState.downcount -= data;
    return true;
  }
  return false;
}

static bool CheckDSI(u32 data)
{
  if (PowerPC::ppcState.Exceptions & EXCEPTION_DSI)
  {
    PowerPC::CheckExceptions();
    PowerPC::ppcState.downcount -= data;
    return true;
  }
  return false;
}

static bool CheckBreakpoint(u32 data)
{
  PowerPC::CheckBreakPoints();
  if (CPU::GetState() != CPU::State::Running)
  {
    PowerPC::ppcState.downcount -= data;
    return true;
  }
  return false;
}

static bool CheckIdle(u32 idle_pc)
{
  if (PowerPC::ppcState.npc == idle_pc)
  {
    CoreTiming::Idle();
  }
  return false;
}

void LLVMJit::LLVMJitCtx::FuncBuilder::CallFunc(CommonCallback const CB, UGeckoInstruction const I)
{
  auto& Ctx = LCtx();
  Constant* F = ConstantExpr::getBitCast(
      ConstantInt::get(Type::getInt64Ty(Ctx), (uintptr_t)CB),
      JCtx().FCommonCBTy);
  IRB.CreateCall(JCtx().FCommonCBTy, F,
    {ConstantInt::get(Type::getInt32Ty(Ctx), I.hex)});
}

void LLVMJit::LLVMJitCtx::FuncBuilder::CallFunc(ConditionalCallback const CB, u32 const V)
{
  auto& Ctx = LCtx();
  Constant* F = ConstantExpr::getBitCast(
      ConstantInt::get(Type::getInt64Ty(Ctx), (uintptr_t)CB),
      JCtx().FCondCBTy);
  Value* Ret = IRB.CreateCall(JCtx().FCondCBTy, F,
    {ConstantInt::get(Type::getInt32Ty(Ctx), V)});
  BasicBlock* ContBB = BasicBlock::Create(Ctx, "", ExitBB->getParent());
  IRB.CreateCondBr(Ret, ExitBB, ContBB);
  IRB.SetInsertPoint(ContBB);
}

bool LLVMJit::LLVMJitCtx::FuncBuilder::HandleFunctionHooking(u32 address, JitState& js)
{
  return HLE::ReplaceFunctionIfPossible(address, [&](u32 function, HLE::HookType type) {
    CallFunc(WritePC, address);
    CallFunc(Interpreter::HLEFunction, function);

    if (type != HLE::HookType::Replace)
      return false;

    CallFunc(EndBlock, js.downcountAmount);
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
        FB.CallFunc(WritePC, op.address);
        FB.CallFunc(CheckBreakpoint, js.downcountAmount);
      }

      if (check_fpu)
      {
        FB.CallFunc(WritePC, op.address);
        FB.CallFunc(CheckFPU, js.downcountAmount);
        js.firstFPInstructionFound = true;
      }

      if (endblock || memcheck)
        FB.CallFunc(WritePC, op.address);
      FB.CallFunc(PPCTables::GetInterpreterOp(op.inst), op.inst);
      if (memcheck)
        FB.CallFunc(CheckDSI, js.downcountAmount);
      if (idle_loop)
        FB.CallFunc(CheckIdle, js.blockStart);
      if (endblock)
        FB.CallFunc(EndBlock, js.downcountAmount);
    }
  }
  if (code_block.m_broken)
  {
    FB.CallFunc(WriteBrokenBlockNPC, nextPC);
    FB.CallFunc(EndBlock, js.downcountAmount);
  }
  FB.Return();

  b->checkedEntry = JitCtx_->Generate(FB);
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
