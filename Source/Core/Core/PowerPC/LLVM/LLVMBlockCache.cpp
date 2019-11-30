// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/PowerPC/LLVM/LLVMBlockCache.h"

#include "Core/PowerPC/JitCommon/JitBase.h"

LLVMBlockCache::LLVMBlockCache(JitBase& jit) : JitBaseBlockCache{jit}
{
}

void LLVMBlockCache::WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest)
{
}
