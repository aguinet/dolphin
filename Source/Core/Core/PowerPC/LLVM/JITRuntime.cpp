#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/HW/CPU.h"
#include "Core/CoreTiming.h"

#define RTPROPS __attribute__((always_inline)) extern "C"

RTPROPS void EndBlock(u32 v)
{
  PC = NPC;
  PowerPC::ppcState.downcount -= v;
}

RTPROPS void WritePC(u32 addr)
{
  PC = addr;
  NPC = addr + 4;
}

RTPROPS void WriteBrokenBlockNPC(u32 npc)
{
  NPC = npc;
}

RTPROPS bool CheckFPU(u32 data)
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

RTPROPS bool CheckDSI(u32 data)
{
  if (PowerPC::ppcState.Exceptions & EXCEPTION_DSI)
  {
    PowerPC::CheckExceptions();
    PowerPC::ppcState.downcount -= data;
    return true;
  }
  return false;
}

RTPROPS bool CheckBreakpoint(u32 data)
{
  PowerPC::CheckBreakPoints();
  if (CPU::GetState() != CPU::State::Running)
  {
    PowerPC::ppcState.downcount -= data;
    return true;
  }
  return false;
}

RTPROPS bool CheckIdle(u32 idle_pc)
{
  if (PowerPC::ppcState.npc == idle_pc)
  {
    CoreTiming::Idle();
  }
  return false;
}

#include "Core/ConfigManager.h"
#undef MAX_LOGLEVEL
#define MAX_LOGLEVEL -1
#include "Core/PowerPC/Interpreter/Interpreter_Branch.cpp"
#include "Core/PowerPC/Interpreter/Interpreter_FloatingPoint.cpp"
#include "Core/PowerPC/Interpreter/Interpreter_Integer.cpp"
#include "Core/PowerPC/Interpreter/Interpreter_LoadStore.cpp"
#include "Core/PowerPC/Interpreter/Interpreter_LoadStorePaired.cpp"
#include "Core/PowerPC/Interpreter/Interpreter_Paired.cpp"
#include "Core/PowerPC/Interpreter/Interpreter_SystemRegisters.cpp"
