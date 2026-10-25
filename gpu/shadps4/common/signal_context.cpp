// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "common/arch.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#elif defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.pc;
#else
#error "Unsupported architecture"
#endif
}

#if defined(__linux__) && defined(ARCH_ARM64)
// gport: the kernel stores the data abort's ESR in a record of the signal frame's reserved area
// (struct esr_context of <asm/sigcontext.h>, which conflicts with glibc's signal headers).
static uint64_t FaultEsr(void* ctx) {
    struct Record {
        uint32_t magic, size;
        uint64_t esr;
    };
    constexpr uint32_t EsrMagic = 0x45535201;
    const auto& mcontext = ((ucontext_t*)ctx)->uc_mcontext;
    const auto* reserved = reinterpret_cast<const unsigned char*>(mcontext.__reserved);
    for (size_t offset = 0; offset + 8 <= sizeof(mcontext.__reserved);) {
        Record record;
        std::memcpy(&record, reserved + offset, std::min(sizeof(record), sizeof(mcontext.__reserved) - offset));
        if (record.magic == 0 || record.size == 0) {
            break;
        }
        if (record.magic == EsrMagic) {
            return record.esr;
        }
        offset += record.size;
    }
    return 0;
}
#endif

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#elif defined(__linux__) && defined(ARCH_ARM64)
    return FaultEsr(ctx) & 0x40; // ESR_ELx.WnR
#else
#error "Unsupported architecture"
#endif
}

} // namespace Common
