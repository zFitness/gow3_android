/* The CPU that runs the game's x86-64 code: the host CPU on x86-64 (src/runtime_cpu.c), FEXCore's
 * JIT elsewhere (cpu/, libbbcpu.so). Guest and host share one address space either way; what
 * differs is how control crosses between them, which goes through these calls:
 *  - host functions handed to the guest (imports) are wrapped by gport_guest_cpu_host_function;
 *  - guest functions called by the host (thread entries, callbacks) go through gport_guest_cpu_call;
 *  - guest register state at a signal is read with gport_guest_cpu_signal_regs. */
#ifndef GPORT_GUEST_CPU_H
#define GPORT_GUEST_CPU_H
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#if defined(__x86_64__)
#define GUEST_CPU_NATIVE 1
#endif
#ifdef __cplusplus
extern "C" {
#endif
#pragma GCC visibility push(default)

/* Guest registers in x86 encoding order. */
enum {
    GUEST_RAX, GUEST_RCX, GUEST_RDX, GUEST_RBX, GUEST_RSP, GUEST_RBP, GUEST_RSI, GUEST_RDI,
    GUEST_R8, GUEST_R9, GUEST_R10, GUEST_R11, GUEST_R12, GUEST_R13, GUEST_R14, GUEST_R15,
    GUEST_GPRS
};
typedef struct { uint64_t gpr[GUEST_GPRS]; uint64_t rip; } GuestRegs;

/* Once, before any other call. `map` allocates memory the guest may hold pointers to (below
 * 1 TiB: the game packs pointers into 40 bits), as runtime_low_map. */
void gport_guest_cpu_init(void *(*map)(size_t size, int prot));
/* Guest code lives in [base, base + size) (the image, import traps). */
void gport_guest_cpu_code(uintptr_t base, size_t size);
/* The guest-callable address of host function `fn` (SysV ABI on the guest side, at most 14
 * integer arguments and 8 vector arguments; no structures by value, no va_list). */
uintptr_t gport_guest_cpu_host_function(void *fn);
/* Calls guest function `fn` with `count` integer arguments; returns RAX. On this thread's current
 * guest stack when the host was called by guest code, else on the stack set by
 * gport_guest_cpu_thread_stack (or one allocated for the thread). */
uint64_t gport_guest_cpu_call(uintptr_t fn, unsigned count, const uint64_t *args);
/* The guest stack of this host thread for calls not nested in guest code: [top - size, top). */
void gport_guest_cpu_thread_stack(void *top, size_t size);
/* Guest GS base (the TCB) of this host thread. */
void gport_guest_cpu_set_gs(void *base);
/* This thread left guest code with longjmp (pthread_exit): nested calls are over. */
void gport_guest_cpu_abandon(void);
/* This host thread is ending: its guest CPU state is released. */
void gport_guest_cpu_thread_end(void);
/* The return address of the guest call that entered the current host function, for diagnostics.
 * `native` is __builtin_return_address(0) of that function (the answer on x86-64 hosts). */
uintptr_t gport_guest_cpu_return_address(uintptr_t native);
/* The guest stack pointer of the innermost guest call into the host on this thread, or 0. */
uintptr_t gport_guest_cpu_stack_pointer(void);
/* The guest registers at a signal. GUEST_CPU_IN_GUEST: the signal interrupted guest code, `regs`
 * as there (always the answer on x86-64 hosts, where host code is not told apart).
 * GUEST_CPU_IN_HOST_CALL: host code called by the guest, `regs` as at that call (rip: the return
 * address into the guest). 0: no guest code below; `regs` holds the host PC and stack pointer. */
enum { GUEST_CPU_IN_GUEST=1, GUEST_CPU_IN_HOST_CALL=2 };
int gport_guest_cpu_signal_regs(const void *ucontext, GuestRegs *regs);
/* The host program counter at a signal. */
uintptr_t gport_guest_cpu_host_pc(const void *ucontext);
/* Faults raised by the guest CPU itself (JIT bookkeeping): handled and to be resumed when 1. */
int gport_guest_cpu_handle_fault(int sig, siginfo_t *info, void *ucontext);
/* Replaces the guest instruction at `address` (at least `size` >= 2 bytes) with a call of
 * `hook`, which sees and edits the registers and must set rip. RCX and R11 hold garbage when the
 * hook runs, so the original code must not read them there. Not available on x86-64 hosts
 * (bbport_guest_hooks.cpp uses int3 there): returns 0. */
typedef void (*GuestHook)(GuestRegs *regs);
int gport_guest_cpu_hook(uintptr_t address, size_t size, GuestHook hook);
/* The guest's time stamp counter (RDTSC) and its frequency in Hz. */
uint64_t gport_guest_cpu_tsc(void);
uint64_t gport_guest_cpu_tsc_frequency(void);

#pragma GCC visibility pop
#ifdef __cplusplus
}
#endif
#endif
