// SPDX-License-Identifier: GPL-2.0-or-later
// gport_guest_cpu.h through FEXCore: the game's x86-64 code runs in FEX's JIT, each host thread with its own
// guest CPU state. Host functions are reached from the guest through stubs that end in `syscall`
// (dispatched by HandleSyscall below); guest functions are called with Context::HandleCallback.
#include "gport_guest_cpu.h"

#include <Common/Config.h>
#include <Common/HostFeatures.h>
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

extern char** environ;

// cpu/host_call_arm64.S: calls `fn` with AAPCS64 arguments from `args`, stores x0, x1, v0, v1.
struct HostArgs {
    uint64_t x[8];
    uint64_t stack_count;
    const uint64_t* stack;
    __uint128_t v[8];
};
struct HostResult {
    uint64_t x[2];
    __uint128_t v[2];
};
extern "C" void gportcpu_host_call(void* fn, const HostArgs* args, HostResult* result);
static_assert(offsetof(HostArgs, stack_count) == 64 && offsetof(HostArgs, stack) == 72 && offsetof(HostArgs, v) == 80);
static_assert(offsetof(HostResult, v) == 16);

namespace {
using FEXCore::Core::CPUState;
using FEXCore::Core::InternalThreadState;

[[noreturn]] void Fatal(const char* message, uint64_t a = 0, uint64_t b = 0) {
    std::fprintf(stderr, "STOP: guest CPU: %s (%#llx, %#llx)\n", message, (unsigned long long)a, (unsigned long long)b);
    std::fflush(nullptr);
    _exit(21);
}

FEXCore::Context::Context* ctx;
void* (*low_map)(size_t, int);
bool wide_xmm;
unsigned tsc_shift;
FEXCore::ArchHelpers::Arm64::UnalignedHandlerType unaligned_type;

// Stubs: 16 bytes each in one region; the first holds CALLBACKRET (0f 3e), the end of guest code
// called by HandleCallback. A stub is `mov r10, rcx; syscall; ret`: syscall replaces RCX (the
// fourth argument) with the return RIP before HandleSyscall sees the registers.
constexpr size_t StubSize = 16;
constexpr size_t StubBytes = 1 << 20;
constexpr size_t SyscallOffset = 3;
unsigned char* stubs;
uintptr_t callback_ret;
void* stub_functions[StubBytes / StubSize];
std::atomic<size_t> stub_count;
std::mutex stub_lock;
std::unordered_map<void*, uintptr_t> stub_of;

struct Hook {
    uintptr_t address;
    GuestHook hook;
};
std::array<Hook, 64> hooks;
std::atomic<size_t> hook_count;

std::shared_mutex code_lock;
std::vector<std::pair<uintptr_t, size_t>> code_ranges;

constexpr size_t CallRetAlloc = InternalThreadState::CALLRET_STACK_SIZE + 2 * FEXCore::Utils::FEX_PAGE_SIZE;
constexpr size_t DefaultStack = 1024 * 1024;
// Above the stack top for calls not nested in guest code: host calls read a few stack argument
// slots beyond the caller's frame.
constexpr size_t StackHeadroom = 256;
// Stack argument slots copied for host functions: x6, x7 and then AAPCS64 stack slots.
constexpr unsigned HostStackSlots = 10;

struct GuestThread {
    InternalThreadState* thread;
    uintptr_t callret_alloc;
    uintptr_t stack_top;
    unsigned depth;
};
thread_local GuestThread* self;
std::mutex threads_lock;
std::vector<GuestThread*> threads;

uint64_t* Xmm(CPUState& s, unsigned i) {
    return wide_xmm ? s.xmm.avx.data[i] : s.xmm.sse.data[i];
}
uintptr_t DefaultCallRet(const GuestThread* t) {
    return reinterpret_cast<uintptr_t>(t->thread->CallRetStackBase) + InternalThreadState::CALLRET_STACK_SIZE / 4;
}

GuestThread* Current() {
    if (self) {
        return self;
    }
    FEXCore::Allocator::InitializeThread();
    auto* t = new GuestThread {};
    t->thread = ctx->CreateThread(nullptr);
    auto* frame = t->thread->CurrentFrame;
    void* alloc = mmap(nullptr, CallRetAlloc, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (alloc == MAP_FAILED) {
        Fatal("cannot allocate the call-return stack");
    }
    t->callret_alloc = reinterpret_cast<uintptr_t>(alloc);
    t->thread->CallRetStackBase = static_cast<char*>(alloc) + FEXCore::Utils::FEX_PAGE_SIZE;
    mprotect(t->thread->CallRetStackBase, InternalThreadState::CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE);
    auto& s = frame->State;
    s.callret_sp = DefaultCallRet(t);
    // A flat 64-bit code segment, as the Linux kernel sets up.
    s.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = s.private_gdt;
    s.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = s.private_gdt;
    s.cs_idx = CPUState::DEFAULT_USER_CS << 3;
    auto* code = CPUState::GetSegmentFromIndex(s, s.cs_idx);
    CPUState::SetGDTBase(code, 0);
    CPUState::SetGDTLimit(code, 0xF'FFFFU);
    code->L = 1;
    s.cs_cached = 0;
    frame->Pointers.ThunkCallbackRet = callback_ret;
    {
        std::lock_guard lock(threads_lock);
        threads.push_back(t);
    }
    self = t;
    return t;
}

void InvalidateCode(uint64_t start, uint64_t length) {
    std::lock_guard lock(threads_lock);
    auto guard = FEXCore::GuardSignalDeferringSectionWithFallback(ctx->GetCodeInvalidationMutex(), self ? self->thread : nullptr);
    ctx->InvalidateCodeBuffersCodeRange(start, length);
    for (auto* t : threads) {
        ctx->InvalidateThreadCachedCodeRange(t->thread, start, length);
    }
}

// A guest call of a host function: SysV registers in, AAPCS64 call, SysV registers out.
void CallHost(CPUState& s, void* fn) {
    using namespace FEXCore::X86State;
    HostArgs args;
    args.x[0] = s.gregs[REG_RDI];
    args.x[1] = s.gregs[REG_RSI];
    args.x[2] = s.gregs[REG_RDX];
    args.x[3] = s.gregs[REG_R10];
    args.x[4] = s.gregs[REG_R8];
    args.x[5] = s.gregs[REG_R9];
    const auto* stack = reinterpret_cast<const uint64_t*>(s.gregs[REG_RSP] + 8);
    args.x[6] = stack[0];
    args.x[7] = stack[1];
    args.stack_count = HostStackSlots - 2;
    args.stack = stack + 2;
    for (unsigned i = 0; i < 8; ++i) {
        std::memcpy(&args.v[i], Xmm(s, i), sizeof(args.v[i]));
    }
    HostResult result;
    gportcpu_host_call(fn, &args, &result);
    s.gregs[REG_RAX] = result.x[0];
    s.gregs[REG_RDX] = result.x[1];
    std::memcpy(Xmm(s, 0), &result.v[0], sizeof(result.v[0]));
    std::memcpy(Xmm(s, 1), &result.v[1], sizeof(result.v[1]));
}

class Handler final : public FEXCore::HLE::SyscallHandler {
public:
    void HandleSyscall(FEXCore::Core::CpuStateFrame* frame) override {
        auto& s = frame->State;
        const uint64_t rip = s.rip;
        const uint64_t offset = rip - reinterpret_cast<uintptr_t>(stubs);
        if (offset < StubBytes && offset % StubSize == SyscallOffset) {
            void* fn = stub_functions[offset / StubSize];
            if (!fn) {
                Fatal("call of an unregistered host stub", rip);
            }
            CallHost(s, fn);
            s.rip = rip + 2;
            return;
        }
        for (size_t i = 0, n = hook_count.load(std::memory_order_acquire); i < n; ++i) {
            if (hooks[i].address != rip) {
                continue;
            }
            GuestRegs regs;
            std::memcpy(regs.gpr, s.gregs, sizeof(regs.gpr));
            regs.rip = rip;
            hooks[i].hook(&regs);
            std::memcpy(s.gregs, regs.gpr, sizeof(regs.gpr));
            s.rip = regs.rip;
            return;
        }
        Fatal("the game executed a syscall instruction (rip, rax)", rip, s.gregs[FEXCore::X86State::REG_RAX]);
    }
    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(InternalThreadState*, uint64_t address) override {
        std::shared_lock lock(code_lock);
        for (const auto& [base, size] : code_ranges) {
            if (address - base < size) {
                return {base, size, false};
            }
        }
        return {0, 0, false};
    }
    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(InternalThreadState*, uint64_t) override {
        return std::nullopt;
    }
    void InvalidateGuestCodeRange(InternalThreadState*, uint64_t start, uint64_t length) override {
        InvalidateCode(start, length);
    }
};

class Delegator final : public FEXCore::SignalDelegator {
public:
    uintptr_t GetThunkCallbackRET() const override {
        return callback_ret;
    }
};

Handler handler;
Delegator delegator;

void LogMessage(LogMan::DebugLevels level, const char* message) {
    static const bool verbose = std::getenv("GPORT_FEX_LOG") != nullptr;
    if (level <= LogMan::ERROR || verbose) {
        std::fprintf(stderr, "FEX %s: %s\n", LogMan::DebugLevelStr(level), message);
    }
}
void LogAssert(const char* message) {
    Fatal(message);
}

// Linux arm64 signal frame: records in uc_mcontext.__reserved; the FP/SIMD one holds v0-v31.
struct FpsimdRecord {
    uint32_t magic, size;
    uint32_t fpsr, fpcr;
    __uint128_t v[32];
};
constexpr uint32_t FpsimdMagic = 0x46508001;
FpsimdRecord* Fpsimd(ucontext_t* uc) {
    auto* at = reinterpret_cast<unsigned char*>(uc->uc_mcontext.__reserved);
    auto* end = at + sizeof(uc->uc_mcontext.__reserved);
    while (at + 8 <= end) {
        uint32_t magic, size;
        std::memcpy(&magic, at, 4);
        std::memcpy(&size, at + 4, 4);
        if (magic == FpsimdMagic) {
            return reinterpret_cast<FpsimdRecord*>(at);
        }
        if (!magic || size < 8) {
            break;
        }
        at += size;
    }
    return nullptr;
}

uint64_t ReadGuest(uintptr_t address) {
    uint64_t value = 0;
    iovec local {&value, sizeof(value)}, remote {reinterpret_cast<void*>(address), sizeof(value)};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == sizeof(value) ? value : 0;
}
} // namespace

extern "C" {
void gport_guest_cpu_init(void* (*map)(size_t size, int prot)) {
    low_map = map;
    // FEXCore's allocator maps its first arena on first use: before the free address space
    // above 47 bits is listed and claimed, so that the arena does not land in a listed gap.
    FEXCore::Allocator::free(FEXCore::Allocator::malloc(64));
    FEXCore::Allocator::Setup48BitAllocatorIfExists(sysconf(_SC_PAGESIZE));
    // FEXCore's allocator must outlive the static destructors that would free it.
    std::atexit(FEXCore::Allocator::ClearHooks);
    LogMan::Throw::InstallHandler(LogAssert);
    LogMan::Msg::InstallHandler(LogMessage);
    FEX::Config::LoadConfig("bb-probe", environ);
    FEXCore::Config::ReloadMetaLayer();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    const auto features = FEX::FetchHostFeatures();
    wide_xmm = features.SupportsAVX && features.SupportsSVE256;
    ctx = FEXCore::Context::Context::CreateNewContext(features).release();
    ctx->SetSignalDelegator(&delegator);
    ctx->SetSyscallHandler(&handler);
    if (!ctx->InitCore()) {
        Fatal("FEXCore initialization failed");
    }
    unaligned_type = FEXCore::Config::Get_HALFBARRIERTSOENABLED()() ? FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier :
                                                                       FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic;
    uint64_t frequency;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (FEXCore::Config::Get_SMALLTSCSCALE()()) {
        // As FEXCore's ContextImpl: the counter scaled up to at least 1 GHz.
        while (frequency && (frequency << tsc_shift) < 1'000'000'000) {
            ++tsc_shift;
        }
    }
    stubs = static_cast<unsigned char*>(map(StubBytes, PROT_READ | PROT_WRITE));
    if (!stubs) {
        Fatal("cannot allocate host function stubs");
    }
    std::memset(stubs, 0xcc, StubBytes);
    stubs[0] = 0x0f;
    stubs[1] = 0x3e;
    callback_ret = reinterpret_cast<uintptr_t>(stubs);
    stub_count = 1;
    gport_guest_cpu_code(reinterpret_cast<uintptr_t>(stubs), StubBytes);
    std::printf("Guest CPU: FEXCore JIT (x86-64 guest on this host), TSC x%u\n", 1u << tsc_shift);
}

void gport_guest_cpu_code(uintptr_t base, size_t size) {
    std::unique_lock lock(code_lock);
    code_ranges.emplace_back(base, size);
}

uintptr_t gport_guest_cpu_host_function(void* fn) {
    std::lock_guard lock(stub_lock);
    if (auto found = stub_of.find(fn); found != stub_of.end()) {
        return found->second;
    }
    const size_t index = stub_count.load(std::memory_order_relaxed);
    if (index >= StubBytes / StubSize) {
        Fatal("too many host functions");
    }
    unsigned char* stub = stubs + index * StubSize;
    static constexpr unsigned char code[] = {0x49, 0x89, 0xca, 0x0f, 0x05, 0xc3}; // mov r10, rcx; syscall; ret
    stub_functions[index] = fn;
    std::memcpy(stub, code, sizeof(code));
    stub_count.store(index + 1, std::memory_order_release);
    const auto address = reinterpret_cast<uintptr_t>(stub);
    stub_of.emplace(fn, address);
    return address;
}

uint64_t gport_guest_cpu_call(uintptr_t fn, unsigned count, const uint64_t* args) {
    using namespace FEXCore::X86State;
    static constexpr int arg_regs[6] = {REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9};
    GuestThread* t = Current();
    auto& s = t->thread->CurrentFrame->State;
    uint64_t saved[16];
    std::memcpy(saved, s.gregs, sizeof(saved));
    const uint64_t saved_rip = s.rip;
    uintptr_t sp;
    if (t->depth) {
        sp = s.gregs[REG_RSP] - 128; // below the red zone of the guest code that called the host
    } else {
        if (!t->stack_top) {
            auto* stack = static_cast<unsigned char*>(low_map(DefaultStack, PROT_READ | PROT_WRITE));
            if (!stack) {
                Fatal("cannot allocate a guest stack");
            }
            t->stack_top = reinterpret_cast<uintptr_t>(stack) + DefaultStack;
        }
        sp = t->stack_top - StackHeadroom;
    }
    const unsigned stack_args = count > 6 ? count - 6 : 0;
    sp = (sp - stack_args * 8) & ~uintptr_t(15);
    for (unsigned i = 0; i < stack_args; ++i) {
        reinterpret_cast<uint64_t*>(sp)[i] = args[6 + i];
    }
    // HandleCallback pushes its return trampoline at RSP - 16: the callee finds its return
    // address at sp - 8 and its stack arguments from sp, as after a call.
    s.gregs[REG_RSP] = sp + 8;
    for (unsigned i = 0; i < count && i < 6; ++i) {
        s.gregs[arg_regs[i]] = args[i];
    }
    s.gregs[REG_RAX] = 0;
    ++t->depth;
    ctx->HandleCallback(t->thread, fn);
    --t->depth;
    const uint64_t result = s.gregs[REG_RAX];
    std::memcpy(s.gregs, saved, sizeof(saved));
    s.rip = saved_rip;
    return result;
}

void gport_guest_cpu_thread_stack(void* top, size_t size) {
    (void)size;
    Current()->stack_top = reinterpret_cast<uintptr_t>(top) & ~uintptr_t(15);
}

void gport_guest_cpu_set_gs(void* base) {
    auto& s = Current()->thread->CurrentFrame->State;
    s.gs_cached = reinterpret_cast<uint64_t>(base);
    s.fs_cached = reinterpret_cast<uint64_t>(base);
}

void gport_guest_cpu_abandon(void) {
    if (self) {
        self->depth = 0;
    }
}

void gport_guest_cpu_thread_end(void) {
    GuestThread* t = self;
    if (!t) {
        return;
    }
    {
        std::lock_guard lock(threads_lock);
        std::erase(threads, t);
    }
    self = nullptr;
    ctx->DestroyThread(t->thread);
    munmap(reinterpret_cast<void*>(t->callret_alloc), CallRetAlloc);
    delete t;
}

uintptr_t gport_guest_cpu_return_address(uintptr_t native) {
    (void)native;
    if (!self || !self->depth) {
        return 0;
    }
    return ReadGuest(self->thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP]);
}

uintptr_t gport_guest_cpu_stack_pointer(void) {
    return self && self->depth ? self->thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP] : 0;
}

int gport_guest_cpu_signal_regs(const void* ucontext, GuestRegs* regs) {
    const auto& m = static_cast<const ucontext_t*>(ucontext)->uc_mcontext;
    GuestThread* t = self;
    if (t) {
        const auto& s = t->thread->CurrentFrame->State;
        if (ctx->IsAddressInCodeBuffer(t->thread, m.pc)) {
            std::memcpy(regs->gpr, s.gregs, sizeof(regs->gpr));
            const auto& config = delegator.GetConfig();
            for (unsigned i = 0; i < config.SRAGPRCount; ++i) {
                regs->gpr[i] = m.regs[config.SRAGPRMapping[i]];
            }
            regs->rip = ctx->RestoreRIPFromHostPC(t->thread, m.pc);
            return GUEST_CPU_IN_GUEST;
        }
        if (t->depth) {
            std::memcpy(regs->gpr, s.gregs, sizeof(regs->gpr));
            regs->rip = ReadGuest(s.gregs[FEXCore::X86State::REG_RSP]);
            return GUEST_CPU_IN_HOST_CALL;
        }
    }
    std::memset(regs, 0, sizeof(*regs));
    regs->rip = m.pc;
    regs->gpr[GUEST_RSP] = m.sp;
    return 0;
}

uintptr_t gport_guest_cpu_host_pc(const void* ucontext) {
    return static_cast<const ucontext_t*>(ucontext)->uc_mcontext.pc;
}

int gport_guest_cpu_handle_fault(int sig, siginfo_t* info, void* ucontext) {
    GuestThread* t = self;
    if (!t) {
        return 0;
    }
    auto* uc = static_cast<ucontext_t*>(ucontext);
    auto& m = uc->uc_mcontext;
    const auto address = reinterpret_cast<uintptr_t>(info->si_addr);
    InternalThreadState* thread = t->thread;
    if (sig == SIGSEGV) {
        // Call-return stack overflow or underflow: the stack is only a prediction, start it again.
        if (address - t->callret_alloc < CallRetAlloc) {
            m.regs[25] = DefaultCallRet(t);
            return 1;
        }
        // The JIT's code buffer filled up while compiling: FEXCore restarts the compilation.
        if (thread->JITGuardPage && address - thread->JITGuardPage < FEXCore::Utils::FEX_PAGE_SIZE) {
            FpsimdRecord* fp = Fpsimd(uc);
            if (!fp) {
                return 0;
            }
            FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(thread->RestartJump, thread->JITGuardOverflowArgument, reinterpret_cast<uint64_t*>(m.regs), fp->v,
                                                            reinterpret_cast<uint64_t*>(&m.pc));
            return 1;
        }
    }
    // Unaligned atomics of guest code (x86 allows them): FEXCore rewrites the access.
    if (sig == SIGBUS && info->si_code == BUS_ADRALN && ctx->IsAddressInCodeBuffer(thread, m.pc)) {
        if (const auto skip = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(thread, unaligned_type, m.pc,
                                                                                   reinterpret_cast<uint64_t*>(m.regs))) {
            m.pc += *skip;
            return 1;
        }
    }
    return 0;
}

int gport_guest_cpu_hook(uintptr_t address, size_t size, GuestHook hook) {
    if (size < 2) {
        return 0;
    }
    const size_t index = hook_count.load(std::memory_order_relaxed);
    if (index >= hooks.size()) {
        return 0;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    auto* page = reinterpret_cast<void*>(address & ~uintptr_t(page_size - 1));
    const size_t span = address + 2 - reinterpret_cast<uintptr_t>(page);
    if (mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return 0;
    }
    hooks[index] = {address, hook};
    hook_count.store(index + 1, std::memory_order_release);
    auto* code = reinterpret_cast<unsigned char*>(address);
    code[0] = 0x0f; // syscall
    code[1] = 0x05;
    mprotect(page, span, PROT_READ | PROT_EXEC);
    InvalidateCode(address, size);
    return 1;
}

uint64_t gport_guest_cpu_tsc(void) {
    uint64_t value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value << tsc_shift;
}

uint64_t gport_guest_cpu_tsc_frequency(void) {
    uint64_t frequency;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    return frequency << tsc_shift;
}
} // extern "C"
