// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARCH_ARM64 1
#endif

// A short spin hint for busy-wait loops: on x86 PAUSE yields the pipeline to a sibling SMT
// thread; on arm64 YIELD hints the core. Kept as a macro (no <x86intrin.h>: the builtin does not
// need the header, and that header does not exist for arm64).
#if defined(ARCH_X86_64)
#define COMMON_CPU_RELAX() __builtin_ia32_pause()
#elif defined(ARCH_ARM64)
#define COMMON_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
#define COMMON_CPU_RELAX() ((void)0)
#endif
