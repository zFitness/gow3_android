/* gport_guest_cpu.h on x86-64 hosts: the game's code runs on the host CPU, so crossings are plain calls. */
#define _GNU_SOURCE
#include "gport_guest_cpu.h"
#if defined(GUEST_CPU_NATIVE)
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <x86intrin.h>
#ifndef _WIN32
#include <unistd.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#endif

void gport_guest_cpu_init(void *(*map)(size_t size, int prot)) { (void)map; }
void gport_guest_cpu_code(uintptr_t base, size_t size) { (void)base; (void)size; }
uintptr_t gport_guest_cpu_host_function(void *fn) { return (uintptr_t)fn; }
uint64_t gport_guest_cpu_call(uintptr_t fn, unsigned count, const uint64_t *args) {
    typedef uint64_t (ABI *Fn)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
    uint64_t a[6]={0};
    if (count>6) { fputs("STOP: gport_guest_cpu_call with more than 6 arguments\n",stderr); exit(21); }
    for (unsigned i=0;i<count;++i) a[i]=args[i];
    return ((Fn)fn)(a[0],a[1],a[2],a[3],a[4],a[5]);
}
void gport_guest_cpu_thread_stack(void *top, size_t size) { (void)top; (void)size; }
void gport_guest_cpu_abandon(void) {}
void gport_guest_cpu_thread_end(void) {}
uintptr_t gport_guest_cpu_return_address(uintptr_t native) { return native; }
uintptr_t gport_guest_cpu_stack_pointer(void) { return (uintptr_t)__builtin_frame_address(0); }
int gport_guest_cpu_hook(uintptr_t address, size_t size, GuestHook hook) { (void)address; (void)size; (void)hook; return 0; }
uint64_t gport_guest_cpu_tsc(void) { return __rdtsc(); }
#ifndef _WIN32
void gport_guest_cpu_set_gs(void *base) {
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); exit(21); }
}
int gport_guest_cpu_signal_regs(const void *ucontext, GuestRegs *regs) {
    static const int order[GUEST_GPRS]={REG_RAX,REG_RCX,REG_RDX,REG_RBX,REG_RSP,REG_RBP,REG_RSI,REG_RDI,
                                        REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15};
    const greg_t *g=((const ucontext_t *)ucontext)->uc_mcontext.gregs;
    for (int i=0;i<GUEST_GPRS;++i) regs->gpr[i]=(uint64_t)g[order[i]];
    regs->rip=(uint64_t)g[REG_RIP];
    return GUEST_CPU_IN_GUEST;
}
uintptr_t gport_guest_cpu_host_pc(const void *ucontext) { return (uintptr_t)((const ucontext_t *)ucontext)->uc_mcontext.gregs[REG_RIP]; }
int gport_guest_cpu_handle_fault(int sig, siginfo_t *info, void *ucontext) { (void)sig; (void)info; (void)ucontext; return 0; }
uint64_t gport_guest_cpu_tsc_frequency(void) {
    static uint64_t hz;
    if (!hz) {
        struct timespec a,b,nap={0,20000000};
        clock_gettime(CLOCK_MONOTONIC,&a); uint64_t t0=__rdtsc();
        nanosleep(&nap,NULL);
        clock_gettime(CLOCK_MONOTONIC,&b); uint64_t t1=__rdtsc();
        uint64_t ns=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000+(uint64_t)(b.tv_nsec-a.tv_nsec);
        hz=(t1-t0)*1000000000/(ns ? ns : 1);
    }
    return hz;
}
#endif
#else
typedef int runtime_cpu_unused; /* gport_guest_cpu.h is implemented by libbbcpu.so (cpu/) */
#endif
