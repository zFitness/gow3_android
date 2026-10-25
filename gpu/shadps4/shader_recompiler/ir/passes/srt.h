// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/arch.h"
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

#ifdef ARCH_X86_64
using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/);
inline const void* SrtWalkerCode(PFN_SrtWalker walker) {
    return reinterpret_cast<const void*>(walker);
}
#else
/// On non-x86 hosts the walker is portable SRT bytecode, not executable machine code.
void RunSrtProgram(const u32* program, const u32* user_data, u32* flat_dst);
struct SrtWalker {
    const u32* program{};
    void operator()(const u32* user_data, u32* flat_dst) const {
        RunSrtProgram(program, user_data, flat_dst);
    }
    explicit operator bool() const { return program != nullptr; }
};
using PFN_SrtWalker = SrtWalker;
inline const void* SrtWalkerCode(PFN_SrtWalker walker) {
    return walker.program;
}
#endif
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
