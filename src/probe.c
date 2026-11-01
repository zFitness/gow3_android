/* Loader for the game's x86-64 image.
 *
 * The CPU side is architecture-independent: on x86-64 the guest code runs on the host CPU
 * (src/gport_runtime_cpu.c), elsewhere it runs under FEXCore (cpu/, libgportcpu.so). Everything that
 * crosses between host and guest goes through the boundary in src/gport_guest_cpu.h, so this file has
 * no x86-64 machine-code assumptions of its own. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "runtime.h"
#include "gport_guest_cpu.h"
#include "orb_hook.h"
#include "cheat_hook.h"
#include "qte_hook.h"
#include "switch_hook.h"
#include "actor_hook.h"
#include "player_hook.h"
#include "ledge_hook.h"
#include "damage_hook.h"
#include "menu_hook.h"
#include "graphics_hook.h"
#include "gpu/spartanportgpu.h"
#if !defined(__GNUC__)
#error This loader requires GCC or Clang.
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           gport_guest_cpu_return_address((uintptr_t)__builtin_return_address(0)) - (uintptr_t)image, argument);
    uintptr_t caller=gport_guest_cpu_return_address((uintptr_t)__builtin_return_address(0))-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#if defined(GUEST_CPU_NATIVE) && !defined(_WIN32)
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack.
 * x86-64 hosts only: elsewhere (libgportcpu.so) guest calls go through gport_guest_cpu_call, which
 * switches to the guest stack itself. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    if (image && address-(uintptr_t)image<0x20000000) { snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    if (gpu_enabled) spartanportgpu_dump_guest_writes(info);
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && spartanportgpu_handle_fault(info,(void *)record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in spartanport_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)spartanport_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* SPARTAN_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov rax, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
#endif
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && spartanportgpu_handle_fault(context, info->si_addr)) return;
    /* Faults the guest CPU itself raises (JIT bookkeeping) are handled and resumed. */
    if (gport_guest_cpu_handle_fault(sig, info, context)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    GuestRegs regs;
    const int in_guest = gport_guest_cpu_signal_regs(context, &regs);
    uintptr_t rip = in_guest == GUEST_CPU_IN_GUEST ? (uintptr_t)regs.rip : gport_guest_cpu_host_pc(context);
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000) {
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
        { ssize_t w=write(2, line, strlen(line)); (void)w; }
        /* 客机寄存器：野指针（哪条基址算错）只有寄存器能说明。 */
        static const char *const names[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                              "r8","r9","r10","r11","r12","r13","r14","r15"};
        for (unsigned r = 0; r < 16; ++r) {
            snprintf(line, sizeof(line), "  guest %s = 0x%llx\n", names[r], (unsigned long long)regs.gpr[r]);
            { ssize_t w=write(2, line, strlen(line)); (void)w; }
        }
    }
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) spartanportgpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    /* Guest registers come from the boundary: on x86-64 hosts the signal interrupted guest code;
     * with the JIT the signal may be in host code between guest calls (gport_guest_cpu_host_pc). */
    GuestRegs regs;
    const int in_guest = gport_guest_cpu_signal_regs(uc, &regs);
    uintptr_t rip = in_guest ? (uintptr_t)regs.rip : gport_guest_cpu_host_pc(uc);
    uintptr_t rbp = in_guest ? (uintptr_t)regs.gpr[GUEST_RBP] : 0;
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)(in_guest ? regs.gpr[GUEST_RDI] : 0));
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
extern ABI float spartanport_orbs_gain(void *player, float requested);
extern void spartanport_orbs_status(int supported);
extern uintptr_t spartanport_cheat_flag(unsigned id);
extern uintptr_t spartanport_actor_slots_address(void);
extern void spartanport_actor_status(int supported);
extern void spartanport_cheat_status(unsigned id);
extern uintptr_t spartanport_damage_multiplier(int taken);
extern uintptr_t spartanport_orb_pickup_multiplier(unsigned kind);
extern void spartanport_stat_multipliers_status(int damage, int orb_pickup);
extern uintptr_t spartanport_qte_state_address(void);
extern void spartanport_qte_status(int supported);
extern uintptr_t spartanport_auto_qte_flag(void);
extern void spartanport_engine_fps_globals(uintptr_t fps, uintptr_t rounded);
extern uintptr_t spartanport_switch_state_address(void);
extern void spartanport_switch_status(int supported);
extern ABI int spartanport_switch_input(const unsigned char *branch, uintptr_t caller);
extern ABI const unsigned char *spartanport_switch_queue(const unsigned char *branch);

/* 暂停菜单的 "Exit to Desktop" 行（src/menu_hook.h）：菜单脚本解释器的 builtin 调用位点。
 * 行本身由 scripts/pause_menu.py 打在 R_UI.WAD 里（挂载即出现），位点钩子负责把那一行
 * 点的 builtin 0xfff（游戏的 provider 表里没有这个 id）变成"结束进程"。 */

/* 结束进程的方式与 restart 一样：GPU、音频与 guest 线程都还在跑，不走析构。 */
static void menu_quit(void) {
    puts("Pause menu: Exit to Desktop");
    fflush(NULL);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(),0);
#endif
    _exit(0);
}
/* 与解释器原来的 vtable 调用同一个 ABI。没有 VM 参数被消费。 */
static void ABI menu_setting(void *self, uint16_t function, uint64_t *result, void *args) {
    (void)self;
    (void)args;
    *result = function >= 0xffd ? (uint64_t)spartanportgpu_enemy_health_bar(function == 0xffd)
                              : spartanportgpu_native_setting(function);
}

/* 金手指安装器向镜像写入 x86-64 机器码 stub：x86-64 host 上 guest 直接跑在宿主 CPU，位点改写为
 * 0xe9 跳转进 trampoline；FEX JIT（ARM64）下镜像被懒翻译，位点改走 gport_guest_cpu_hook 的 syscall
 * 陷阱，host 侧 hook 把 rip 重定向到同一 trampoline。两路径共用下方签名定位，故任意构建只要代码
 * 匹配即可生效。 */
static const char *const spartanport_cheat_names[] = {"health", "magic", "item meter", "Rage of Sparta",
                                                      "red orbs", "Very Hard unlock", "infinite double jumps"};

/* 某个作弊在本构建中找到的 hook 位点（已验证地址）。 */
typedef struct { uint64_t at; int valid; } SpartanCheatSite;

/* 在镜像中按签名定位一个作弊的全部 hook 位点（适用任何构建）；返回命中的位点数。 */
static int spartanport_cheat_locate(SpartanCheatSite *sites, unsigned id, const unsigned char *image,
                                    uint64_t size, Segment *segments, uint64_t count) {
    int found = 0;
    for (unsigned i = 0; i < SPARTAN_CHEAT_HOOK_COUNT; ++i) {
        const SpartanCheatHook *hook = &spartanport_cheat_hooks[i];
        if (hook->id != id) continue;
        sites[i].valid = 0;
        const uint64_t at = spartanport_cheat_find(image, size, hook);
        if (at == UINT64_MAX || !mapped(segments, count, at, hook->length)) continue;
        sites[i].at = at;
        sites[i].valid = 1;
        ++found;
    }
    return found;
}

/* 作弊身份门禁：候选游戏与补丁系统同一 serial/版本集合（scripts/patches.py::GAMES）。某个作弊
 * 是否真的可用由本构建中各 hook 的签名命中决定，不由身份决定：签名不命中的逐项 unavailable，
 * 其余照常安装。 */
static int cheat_identity(const char *app0) {
    static const char *const candidates[][2] = {
        {"CUSA01623", "01.02"}, {"CUSA01715", "01.02"}, {"CUSA01720", "01.02"},
        {"CUSA01740", "01.00"}, {"CUSA01740", "01.02"},
    };
    char path[4096], serial[32] = "", version[32] = "";
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
    sfo_value(path, "TITLE_ID", serial, sizeof(serial), NULL);
    sfo_value(path, "APP_VER", version, sizeof(version), NULL);
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
        if (!strcmp(serial, candidates[i][0]) && !strcmp(version, candidates[i][1])) return 1;
    /* 不静默：身份不在候选集是唯一"一个作弊都不装"的路径，日志必须说明看到了哪个游戏。 */
    printf("FEX Cheats: OFF - unsupported game identity %s %s (no cheats installed)\n",
           serial[0] ? serial : "<none>", version[0] ? version : "<none>");
    return 0;
}

/* Automatic QTE 的四个提示位点（定时按键、连打、蓄力长按、圆圈狂按），两条路径共用同一组签名。 */
enum { AUTO_QTE_SITES = 4 };

/* 游戏的实时帧率全局（上游 windows-v2.7.0 的 find_engine_fps）：引擎在启动时以 59.94 调用一次
 * 帧率 setter，把上限帧率存成 float 与取整值两个全局，每帧的帧时都读它们。客机的
 * SyncEngineFps 写这两个地址，"引擎帧率"菜单项才是活的（帧率补丁改的是那个常量本身）。
 * 两条路径共用：写的是镜像里的数据，JIT 与原生 CPU 看到的都是同一块内存。 */
static const SpartanSignature engine_fps_setter={
    "48 8d 05 ?? ?? ?? ?? c5 fa 11 00 c5 fa 58 0d ?? ?? ?? ?? c5 fa 2c c1 48 8d 0d ?? ?? ?? ?? 89 01",0};
static uint64_t engine_fps_at, engine_rounded_at; /* image offsets; 0: not found */
/* 镜像受保护之前：签名搜索会连段间空隙一起读。 */
static void find_engine_fps(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t at=spartanport_signature_find(image,size,&engine_fps_setter);
    if (at==UINT64_MAX) {
        puts("Engine FPS: live frame rate unavailable (code not found in this eboot.bin)");
        return;
    }
    int32_t fps_displacement, rounded_displacement;
    memcpy(&fps_displacement,image+at+3,4);      /* lea rax, [rip+fps] */
    memcpy(&rounded_displacement,image+at+26,4); /* lea rcx, [rip+rounded] */
    const uint64_t fps=(uint64_t)((int64_t)at+7+fps_displacement);
    const uint64_t rounded=(uint64_t)((int64_t)at+30+rounded_displacement);
    if (!mapped(segments,count,fps,4) || !mapped(segments,count,rounded,4)) {
        puts("Engine FPS: live frame rate unavailable (globals outside the image)");
        return;
    }
    engine_fps_at=fps;
    engine_rounded_at=rounded;
}
/* 镜像受保护之后：present 线程每帧读那两个全局，两次 protect 之间镜像不可访问。 */
static void publish_engine_fps(void) {
    if (!engine_fps_at) return;
    spartanport_engine_fps_globals((uintptr_t)(image+engine_fps_at),(uintptr_t)(image+engine_rounded_at));
    puts("Engine FPS: live frame rate ready (menu, R3 + L1 + R1 or F9)");
}

#if defined(GUEST_CPU_NATIVE)
static void install_cheats(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    if (!cheat_identity(app0)) return;
    for (unsigned id = 0; id < SPARTAN_CHEAT_COUNT; ++id) {
        SpartanCheatSite sites[SPARTAN_CHEAT_HOOK_COUNT];
        if (spartanport_cheat_locate(sites, id, image, size, segments, count) < 1) {
            printf("Cheats: %s unavailable (code signature mismatch)\n", spartanport_cheat_names[id]);
            continue;
        }
        unsigned char *stubs = allocate(page_size);
        int32_t relative[SPARTAN_CHEAT_HOOK_COUNT] = {0};
        int valid = 1;
        for (unsigned i = 0; i < SPARTAN_CHEAT_HOOK_COUNT; ++i) {
            const SpartanCheatHook *hook = &spartanport_cheat_hooks[i];
            if (hook->id != id) continue;
            if (!sites[i].valid) { valid = 0; break; }
            unsigned char *stub = stubs + i * 256;
            int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + sites[i].at + 5);
            if (delta < INT32_MIN || delta > INT32_MAX) { valid = 0; break; }
            relative[i] = (int32_t)delta;
            spartanport_cheat_build_stub(stub, hook, spartanport_cheat_flag(id),
                                         (uintptr_t)(image + sites[i].at + hook->length));
        }
        if (!valid) {
            printf("Cheats: %s unavailable (hook outside relative-jump range)\n", spartanport_cheat_names[id]);
            continue;
        }
        protect(stubs, page_size, 5);
        for (unsigned i = 0; i < SPARTAN_CHEAT_HOOK_COUNT; ++i) {
            const SpartanCheatHook *hook = &spartanport_cheat_hooks[i];
            if (hook->id != id) continue;
            image[sites[i].at] = 0xe9;
            memcpy(image + sites[i].at + 1, &relative[i], 4);
            memset(image + sites[i].at + 5, 0x90, hook->length - 5);
        }
        spartanport_cheat_status(id);
        printf("Cheats: %s ready (live toggle, initially disabled unless saved)\n", spartanport_cheat_names[id]);
    }
}
/* The upgrade menu's open flag (src/graphics_hook.h): Texture Quality skips its LOD bias while it
 * is set (texture_cache.cpp), so the red orb medallion stays sharp. */
extern void spartanport_power_up_menu_global(uintptr_t flag);
static uint64_t power_up_menu_at; /* image offset; 0: not found */
/* Before the image is protected, like find_engine_fps. */
static void find_power_up_menu(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t at=spartanport_signature_find(image,size,&spartanport_power_up_menu_site);
    if (at==UINT64_MAX) {
        puts("Texture Quality: upgrade menu exception unavailable (code not found in this eboot.bin)");
        return;
    }
    const uint64_t flag=spartanport_rip_target(image,at+SPARTAN_POWER_UP_MENU_LEA);
    if (!mapped(segments,count,flag,4)) {
        puts("Texture Quality: upgrade menu exception unavailable (global outside the image)");
        return;
    }
    power_up_menu_at=flag;
}
static void publish_power_up_menu(void) {
    if (!power_up_menu_at) return;
    spartanport_power_up_menu_global((uintptr_t)(image+power_up_menu_at));
    puts("Texture Quality: upgrade menu keeps full detail");
}

/* Damage hook for the enemy health bar (src/actor_hook.h). Always
 * installed when the code matches: it only records values; the overlay decides what to show. */
static void install_actor_watch(Segment *segments, uint64_t count, uint64_t size) {
    /* 按签名定位（上游做法）：固定偏移只在参考构建上成立，别的构建要靠签名找。 */
    const uint64_t at = spartanport_signature_find(image, size, &spartanport_actor_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, SPARTAN_ACTOR_HOOK_LENGTH) ||
        !spartanport_actor_signature_match(image + at, size - at)) {
        printf("Health display: unavailable (code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = allocate(page_size);
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        printf("Health display: unavailable (hook outside relative-jump range)\n");
        return;
    }
    spartanport_actor_build_stub(stub, spartanport_actor_slots_address(), (uintptr_t)(image + at + SPARTAN_ACTOR_HOOK_LENGTH));
    protect(stub, page_size, 5);
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, SPARTAN_ACTOR_HOOK_LENGTH - 5);
    spartanport_actor_status(1);
    printf("Health display: damage hook ready (enemy health bar in the menu)\n");
}

static void install_orb_multiplier(Segment *segments, uint64_t count, uint64_t size) {
    /* setter 与三个 call 位点都由签名定位（上游做法），并校验每个 call 仍指向 setter。 */
    uint64_t calls[3];
    const uint64_t setter_at = spartanport_orb_find(image, size, calls);
    if (setter_at == UINT64_MAX) {
        printf("Red orbs: multiplier unavailable (code not found in this eboot.bin)\n");
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!mapped(segments, count, calls[i], 5)) return;
    }
    /* SysV call sites: preserve the receiver across the helper, then tail-call the original
     * setter. Its cap and upgrade notifications remain in the game's own code. */
    unsigned char *stub = allocate(page_size);
    uintptr_t helper = (uintptr_t)spartanport_orbs_gain, setter = (uintptr_t)(image + setter_at);
    unsigned char code[26];
    spartanport_orb_build_stub(code, helper, setter);
    int32_t displacements[3];
    for (unsigned i = 0; i < 3; ++i) {
        int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + calls[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Red orbs: multiplier unavailable (hook outside relative-call range)\n");
            return;
        }
        displacements[i] = (int32_t)delta;
    }
    memcpy(stub, code, sizeof(code));
    protect(stub, page_size, 5);
    for (unsigned i = 0; i < 3; ++i)
        memcpy(image + calls[i] + 1, &displacements[i], 4);
    spartanport_orbs_status(1);
    printf("Red orbs: live multiplier ready (3 validated gain paths, 0.1x to 100x)\n");
}
/* Writes a jump from image+at to `stub`; the rest of the replaced bytes become nops (the stub
 * replays them). 0 when the stub is out of relative-jump range. */
static int install_jump(uint64_t at, unsigned length, unsigned char *stub) {
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, length - 5);
    return 1;
}

/* Options > Graphics (src/graphics_hook.h): bloom and motion blur, toggled live through bytes the
 * settings own. A site whose code does not match leaves its row "Unavailable". */
extern uintptr_t spartanport_graphics_flag(unsigned id);
extern void spartanport_graphics_status(unsigned id);
static void install_graphics(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t blur_at = spartanport_signature_find(image, size, &spartanport_motion_blur_site);
    const uint64_t bloom_at = spartanport_signature_find(image, size, &spartanport_bloom_site);
    const uint64_t shadows_at = spartanport_signature_find(image, size, &spartanport_shadows_site);
    /* Same prologue bytes as the motion blur site. */
    int shadows = shadows_at != UINT64_MAX && mapped(segments, count, shadows_at, SPARTAN_SHADOWS_HOOK_LENGTH) &&
                  spartanport_motion_blur_signature_match(image + shadows_at, size - shadows_at);
    int blur = blur_at != UINT64_MAX && mapped(segments, count, blur_at, 48) &&
               spartanport_motion_blur_signature_match(image + blur_at, size - blur_at);
    int bloom = bloom_at != UINT64_MAX && mapped(segments, count, bloom_at, SPARTAN_BLOOM_HOOK_LENGTH) &&
                spartanport_bloom_signature_match(image + bloom_at, size - bloom_at);
    uint64_t gate = 0, queued = 0;
    if (blur) {
        gate = spartanport_rip_target(image, blur_at + SPARTAN_MOTION_BLUR_GATE);
        queued = spartanport_rip_target(image, blur_at + SPARTAN_MOTION_BLUR_QUEUED);
        blur = mapped(segments, count, gate, 4) && mapped(segments, count, queued, 4);
    }
    if (!blur && !bloom && !shadows) {
        puts("Graphics: bloom, motion blur and shadows toggles unavailable (code not found in this eboot.bin)");
        return;
    }
    unsigned char *stubs = allocate(page_size);
    if (blur)
        spartanport_motion_blur_build_stub(stubs, spartanport_graphics_flag(SPARTAN_GRAPHICS_MOTION_BLUR),
                                           (uintptr_t)(image + gate), (uintptr_t)(image + queued),
                                           (uintptr_t)(image + blur_at + SPARTAN_MOTION_BLUR_HOOK_LENGTH));
    if (bloom)
        spartanport_bloom_build_stub(stubs + 256, spartanport_graphics_flag(SPARTAN_GRAPHICS_BLOOM),
                                     (uintptr_t)(image + bloom_at + SPARTAN_BLOOM_HOOK_LENGTH));
    if (shadows)
        spartanport_shadows_build_stub(stubs + 768, spartanport_graphics_flag(SPARTAN_GRAPHICS_SHADOWS),
                                       (uintptr_t)(image + shadows_at + SPARTAN_SHADOWS_HOOK_LENGTH));
    protect(stubs, page_size, 5);
    blur = blur && install_jump(blur_at, SPARTAN_MOTION_BLUR_HOOK_LENGTH, stubs);
    bloom = bloom && install_jump(bloom_at, SPARTAN_BLOOM_HOOK_LENGTH, stubs + 256);
    shadows = shadows && install_jump(shadows_at, SPARTAN_SHADOWS_HOOK_LENGTH, stubs + 768);
    if (blur) spartanport_graphics_status(SPARTAN_GRAPHICS_MOTION_BLUR);
    if (bloom) spartanport_graphics_status(SPARTAN_GRAPHICS_BLOOM);
    if (shadows) spartanport_graphics_status(SPARTAN_GRAPHICS_SHADOWS);
    printf("Graphics: bloom toggle %s, motion blur toggle %s, shadows toggle %s\n", blur ? "ready" : "unavailable",
           bloom ? "ready" : "unavailable", shadows ? "ready" : "unavailable");
}

/* Damage dealt/taken and green/blue/gold orb multipliers (src/damage_hook.h, src/orb_hook.h):
 * the stubs read the menu's floats directly, so changes apply on the next hit or pickup. */
static void install_stat_multipliers(Segment *segments, uint64_t count, uint64_t size) {
    /* 两个位点都按签名定位（上游做法）：固定偏移只在参考构建上成立。 */
    const uint64_t damage_at = spartanport_signature_find(image, size, &spartanport_damage_site);
    const uint64_t pickup_at = spartanport_signature_find(image, size, &spartanport_orb_pickup_site);
    int damage = damage_at != UINT64_MAX && mapped(segments, count, damage_at, SPARTAN_DAMAGE_HOOK_LENGTH) &&
                 spartanport_damage_signature_match(image + damage_at, size - damage_at);
    int pickup = pickup_at != UINT64_MAX && mapped(segments, count, pickup_at, SPARTAN_ORB_PICKUP_LENGTH) &&
                 spartanport_orb_pickup_signature_match(image + pickup_at, size - pickup_at);
    unsigned char *stubs = damage || pickup ? allocate(page_size) : NULL;
    if (damage) {
        spartanport_damage_build_stub(stubs, spartanport_damage_multiplier(1), spartanport_damage_multiplier(0),
                               (uintptr_t)(image + damage_at + SPARTAN_DAMAGE_HOOK_LENGTH));
    }
    if (pickup) {
        spartanport_orb_pickup_build_stub(stubs + 256, spartanport_orb_pickup_multiplier(0), spartanport_orb_pickup_multiplier(1),
                                   spartanport_orb_pickup_multiplier(2),
                                   (uintptr_t)(image + pickup_at + SPARTAN_ORB_PICKUP_LENGTH));
    }
    if (stubs) protect(stubs, page_size, 5);
    damage = damage && install_jump(damage_at, SPARTAN_DAMAGE_HOOK_LENGTH, stubs);
    pickup = pickup && install_jump(pickup_at, SPARTAN_ORB_PICKUP_LENGTH, stubs + 256);
    spartanport_stat_multipliers_status(damage, pickup);
    printf("Multipliers: damage %s, green/blue/gold orbs %s\n", damage ? "ready" : "unavailable",
           pickup ? "ready" : "unavailable");
}
/* QTE 提示（src/qte_hook.h）：提示的 update 里记心跳、旋转判定里作答——启动器的摇杆提示与
 * Automatic QTE 都靠它。两处要么都装上，要么都不装；装上之前它们什么都不做。返回是否装成。 */

/* Kratos' object (src/player_hook.h), for the Cronos blade pull cap in runtime_pad.c. */
static void install_player_hook(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t at = spartanport_signature_find(image, size, &spartanport_player_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, SPARTAN_PLAYER_HOOK_LENGTH) ||
        !spartanport_player_match(image + at, size - at)) {
        printf("Player hook: unavailable (code not found in this eboot.bin)\n");
        return;
    }
    int32_t displacement;
    memcpy(&displacement, image + at + 3, 4);
    const int64_t rate = (int64_t)at + SPARTAN_PLAYER_HOOK_LENGTH + displacement;
    unsigned char *stub = allocate(page_size);
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
    if (rate < 0 || !mapped(segments, count, (uint64_t)rate, 4) || delta < INT32_MIN || delta > INT32_MAX) {
        printf("Player hook: unavailable (operand or hook outside range)\n");
        return;
    }
    spartanport_player_stub(stub, (uintptr_t)&runtime_player_object, (uintptr_t)(image + rate),
                            (uintptr_t)(image + at + SPARTAN_PLAYER_HOOK_LENGTH));
    protect(stub, page_size, 5);
    const int32_t relative = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, 4);
    memset(image + at + 5, 0x90, SPARTAN_PLAYER_HOOK_LENGTH - 5);
    printf("Player hook: ready at image+0x%" PRIx64 "\n", at);
}

/* Wall seam fix (src/ledge_hook.h): present only when the launcher's "Wall Seam Fix" patch swapped
 * 31 c9 for 33 c9 after the ledge ray call. */
static _Alignas(64) SpartanLedgeCache ledge_cache;
static void install_wall_seam_fix(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t seam = spartanport_signature_find(image, size, &spartanport_ledge_site);
    if (seam == UINT64_MAX) return;
    int32_t call, lea;
    memcpy(&call, image + seam + 1, 4);
    memcpy(&lea, image + seam + 18, 4);
    const int64_t callee = (int64_t)seam + 5 + call, result = (int64_t)seam + 22 + lea;
    unsigned char *stub = allocate(page_size);
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + seam + 5);
    if (callee < 0 || result < 0 || !mapped(segments, count, (uint64_t)callee, 1) ||
        !mapped(segments, count, (uint64_t)result, SPARTAN_LEDGE_RESULT_WORDS * 4) ||
        delta < INT32_MIN || delta > INT32_MAX) {
        puts("Wall seam fix: skipped (call, result or hook outside range)");
        return;
    }
    ledge_cache.result = (uintptr_t)(image + result);
    spartanport_ledge_stub(stub, (uintptr_t)&ledge_cache, (uintptr_t)spartanport_ledge_ray,
                           (uintptr_t)(image + callee), (uintptr_t)(image + seam + 5));
    protect(stub, page_size, 5);
    const int32_t relative = (int32_t)delta;
    image[seam] = 0xe9;
    memcpy(image + seam + 1, &relative, 4);
    printf("Wall seam fix: ready at image+0x%" PRIx64 "\n", seam);
}

/* QTE prompts (src/qte_hook.h): a frame count in the prompt's update and an answer in its
 * rotation check, for the stick prompts in runtime_pad.c and Automatic QTE. Both or neither;
 * they do nothing until one of those answers a prompt. Returns 1 when installed. */
static int install_qte_hooks(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t sites[2] = {spartanport_signature_find(image, size, &spartanport_qte_update_site),
                               spartanport_signature_find(image, size, &spartanport_qte_rotation_site)};
    const unsigned char *originals[2] = {spartanport_qte_update_original, spartanport_qte_rotation_original};
    for (unsigned i = 0; i < 2; ++i) {
        if (sites[i] == UINT64_MAX || !mapped(segments, count, sites[i], SPARTAN_QTE_BLOCK_NEXT) ||
            memcmp(image + sites[i], originals[i], SPARTAN_QTE_HOOK_LENGTH)) {
            printf("Stick prompts: unavailable (code not found in this eboot.bin)\n");
            return 0;
        }
    }
    const uint64_t block = spartanport_qte_block(image + sites[1], sites[1]);
    if (!mapped(segments, count, block, SPARTAN_QTE_TYPE_OFFSET + 2)) {
        printf("Stick prompts: unavailable (prompt state outside the image)\n");
        return 0;
    }
    unsigned char *stubs = allocate(page_size);
    int32_t relative[2];
    for (unsigned i = 0; i < 2; ++i) {
        const int64_t delta = (int64_t)(uintptr_t)(stubs + i * 128) - (int64_t)(uintptr_t)(image + sites[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Stick prompts: unavailable (hook outside relative-jump range)\n");
            return 0;
        }
        relative[i] = (int32_t)delta;
    }
    SpartanQteState *state = (SpartanQteState *)spartanport_qte_state_address();
    spartanport_qte_build_update_stub(stubs, (uintptr_t)&state->heartbeat,
                               (uintptr_t)(image + sites[0] + SPARTAN_QTE_HOOK_LENGTH));
    spartanport_qte_build_rotation_stub(stubs + 128, (uintptr_t)&state->pass, spartanport_auto_qte_flag(),
                                 (uintptr_t)(image + sites[1] + SPARTAN_QTE_HOOK_LENGTH));
    protect(stubs, page_size, 5);
    for (unsigned i = 0; i < 2; ++i) {
        image[sites[i]] = 0xe9;
        memcpy(image + sites[i] + 1, &relative[i], 4);
        memset(image + sites[i] + 5, 0x90, SPARTAN_QTE_HOOK_LENGTH - 5);
    }
    state->block = (uint64_t)(uintptr_t)(image + block);
    printf("Stick prompts: rotation prompt hooks ready\n");
    return 1;
}

/* Automatic QTE（src/qte_hook.h）：定时按键的按键判定、连打的按键查询、蓄力长按的按住位、圆圈
 * 狂按；旋转提示走上面的旋转 stub。四个位点各自独立：找不到的那个把那类提示留给玩家自己按。 */
static void install_auto_qte_hooks(Segment *segments, uint64_t count, uint64_t size, int rotation) {
    const SpartanSignature *signatures[AUTO_QTE_SITES] = {&spartanport_qte_timed_site, &spartanport_qte_repeat_site,
                                                       &spartanport_qte_charged_site, &spartanport_qte_circle_site};
    const unsigned lengths[AUTO_QTE_SITES] = {SPARTAN_QTE_TIMED_LENGTH, 5, SPARTAN_QTE_CHARGED_LENGTH,
                                              SPARTAN_QTE_CIRCLE_LENGTH};
    const char *names[AUTO_QTE_SITES] = {"button", "repeated press", "held press", "Circle struggle"};
    const uintptr_t automatic = spartanport_auto_qte_flag();
    SpartanQteState *state = (SpartanQteState *)spartanport_qte_state_address();
    _Static_assert((int)AUTO_QTE_SITES == (int)SPARTAN_QTE_COUNTS, "one frame count per site");
    unsigned char *stubs = allocate(page_size);
    uint64_t sites[AUTO_QTE_SITES];
    int32_t relative[AUTO_QTE_SITES] = {0};
    int ready = 0;
    for (unsigned i = 0; i < AUTO_QTE_SITES; ++i) {
        sites[i] = spartanport_signature_find(image, size, signatures[i]);
        if (sites[i] == UINT64_MAX || !mapped(segments, count, sites[i], lengths[i])) {
            printf("Automatic QTE: %s prompts unavailable (code not found in this eboot.bin)\n", names[i]);
            sites[i] = UINT64_MAX;
            continue;
        }
        unsigned char *stub = stubs + i * 128;
        const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + sites[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Automatic QTE: %s prompts unavailable (hook outside relative-jump range)\n", names[i]);
            sites[i] = UINT64_MAX;
            continue;
        }
        relative[i] = (int32_t)delta;
        const uintptr_t resume = (uintptr_t)(image + sites[i] + lengths[i]);
        /* The sites are in SPARTAN_QTE_COUNT_* order: each stub counts its kind's frames. */
        const uintptr_t frames = (uintptr_t)&state->kinds[i];
        if (i == 0) spartanport_qte_build_timed_stub(stub, frames, automatic, resume);
        else if (i == 1) spartanport_qte_build_repeat_stub(stub, frames, automatic,
                                                    (uintptr_t)(image + spartanport_qte_call_target(image + sites[i], sites[i])));
        else if (i == 2) spartanport_qte_build_charged_stub(stub, frames, automatic, resume);
        else spartanport_qte_build_circle_stub(stub, frames, automatic, resume,
                                        (uintptr_t)(image + sites[i] + SPARTAN_QTE_CIRCLE_SKIP));
    }
    protect(stubs, page_size, 5);
    for (unsigned i = 0; i < AUTO_QTE_SITES; ++i) {
        if (sites[i] == UINT64_MAX) continue;
        image[sites[i]] = i == 1 ? 0xe8 : 0xe9;  /* the repeat site stays a call */
        memcpy(image + sites[i] + 1, &relative[i], 4);
        memset(image + sites[i] + 5, 0x90, lengths[i] - 5);
        ++ready;
    }
    if (!ready && !rotation) return;
    spartanport_qte_status(1);
    printf("Automatic QTE: ready for %d of %d prompt kinds (menu, Game)\n", ready + rotation, AUTO_QTE_SITES + 1);
}

/* 暂停菜单的 "Exit to Desktop"（src/menu_hook.h）：在菜单脚本解释器的 builtin 调用位点
 * （签名扫描定位）改写 11 字节为跳板，stub 里按 function id 分发（0xfff → 退出进程，
 * 0xf00..0xf60 / 0xffd / 0xffe → 我们的 settings provider，其余原样重放），再跳回原指令
 * 流。钩子装好之后才把打好补丁的 R_UI.WAD 挂上去（否则那行点了没人处理）。 */
static const uintptr_t menu_vtable[4] = {0, 0, 0, (uintptr_t)menu_setting};
static const uintptr_t *menu_provider = menu_vtable;
static void install_menu_exit(Segment *segments, uint64_t count, uint64_t size, const char *wad) {
    const uint64_t at = spartanport_signature_find(image, size, &spartanport_menu_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, SPARTAN_MENU_HOOK_LENGTH) ||
        !spartanport_menu_signature_match(image + at, size - at)) {
        printf("Pause menu: Exit to Desktop unavailable (code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = allocate(page_size);
    spartanport_menu_build_stub(stub, (uintptr_t)menu_quit, (uintptr_t)(image + at + SPARTAN_MENU_HOOK_LENGTH),
                               (uintptr_t)&menu_provider);
    protect(stub, page_size, 5);
    if (!install_jump(at, SPARTAN_MENU_HOOK_LENGTH, stub)) {
        puts("Pause menu: Exit to Desktop unavailable (hook outside relative-jump range)");
        return;
    }
    runtime_file_mount("/app0/EXEC/WAD/PS4_LE/R_UI.WAD", wad);
    puts("Pause menu: Exit to Desktop ready");
}

static void install_switch_hooks(Segment *segments, uint64_t count, uint64_t size) {
    const char *setting = getenv("SPARTAN_SWITCH_ATTACKS");
    if (!setting || strcmp(setting, "1")) return;
    uint64_t sites[SPARTAN_SWITCH_SITES];
    for (unsigned i = 0; i < SPARTAN_SWITCH_SITES; ++i) {
        const SpartanSwitchSite *site = &spartanport_switch_sites[i];
        sites[i] = spartanport_signature_find(image, size, &site->site);
        if (sites[i] == UINT64_MAX || !mapped(segments, count, sites[i], site->length) ||
            memcmp(image + sites[i], site->original, site->length)) {
            printf("Switch attacks: unavailable (%s not found in this eboot.bin)\n", site->name);
            return;
        }
    }
    const uint64_t pass_at = spartanport_signature_find(image, size, &spartanport_switch_pass_site);
    const uint64_t caller = spartanport_signature_find(image, size, &spartanport_switch_caller_site);
    if (pass_at == UINT64_MAX || caller == UINT64_MAX || !mapped(segments, count, pass_at - 1, 5)) {
        printf("Switch attacks: unavailable (input check not found in this eboot.bin)\n");
        return;
    }
    int32_t pass_rel;
    memcpy(&pass_rel, image + pass_at, 4);
    const uint64_t pass = pass_at + 4 + (int64_t)pass_rel;
    const size_t stub_size = round_page(SPARTAN_SWITCH_SITES * SPARTAN_SWITCH_SLOT);
    unsigned char *stubs = allocate(stub_size);
    int32_t relative[SPARTAN_SWITCH_SITES];
    for (unsigned i = 0; i < SPARTAN_SWITCH_SITES; ++i) {
        const int64_t delta = (int64_t)(uintptr_t)(stubs + i * SPARTAN_SWITCH_SLOT) -
                              (int64_t)(uintptr_t)(image + sites[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) {
            printf("Switch attacks: unavailable (hook outside relative-jump range)\n");
            return;
        }
        relative[i] = (int32_t)delta;
    }
    ((SpartanSwitchState *)spartanport_switch_state_address())->caller = (uint64_t)(uintptr_t)(image + caller);
    const uintptr_t calls[SPARTAN_SWITCH_SITES] = {(uintptr_t)&spartanport_switch_input, (uintptr_t)&spartanport_switch_queue, 0};
    for (unsigned i = 0; i < SPARTAN_SWITCH_SITES; ++i)
        spartanport_switch_build_stub(stubs + i * SPARTAN_SWITCH_SLOT, &spartanport_switch_sites[i], spartanport_switch_state_address(),
                               calls[i], (uintptr_t)(image + pass),
                               (uintptr_t)(image + sites[i] + spartanport_switch_sites[i].length));
    protect(stubs, stub_size, 5);
    for (unsigned i = 0; i < SPARTAN_SWITCH_SITES; ++i) {
        image[sites[i]] = 0xe9;
        memcpy(image + sites[i] + 1, &relative[i], 4);
        memset(image + sites[i] + 5, 0x90, spartanport_switch_sites[i].length - 5);
    }
    spartanport_switch_status(1);
    printf("Switch attacks: hooks ready (input check 0x%llx, input queue 0x%llx, d-pad select 0x%llx)\n",
           (unsigned long long)sites[0], (unsigned long long)sites[1], (unsigned long long)sites[2]);
}
/* G3PATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
#else
/* JIT 路径：每个 hook 位点变成长 `syscall` 陷阱（gport_guest_cpu_hook），重定向 hook 在表中查
 * 到该位点并把 rip 指向它的 trampoline stub。trampoline 复用 spartanport_cheat_build_stub 的
 * 字节语义（flag 检查 + 替换指令 + 跳回），故运行期经 SpartanSettings::cheats[] 的开关在每个
 * stub 入口依旧生效。
 * 金手指、QTE、暂停菜单、伤害/倍率、敌血条、红球倍率共用这一张表：位点 → trampoline。 */
typedef struct { uintptr_t addr, target; } SpartanTrapRedirect;
static SpartanTrapRedirect spartanport_trap_redirects[64];
static unsigned spartanport_trap_redirect_count;
static void trap_redirect(GuestRegs *regs) {
    for (unsigned i = 0; i < spartanport_trap_redirect_count; ++i)
        if (spartanport_trap_redirects[i].addr == regs->rip) {
            regs->rip = spartanport_trap_redirects[i].target;
            return;
        }
    regs->rip += 2;  /* unknown site: step past the syscall */
}
static int trap_redirect_add(uintptr_t addr, uintptr_t target) {
    if (spartanport_trap_redirect_count >= 64) return 0;
    if (!gport_guest_cpu_hook(addr, 2, trap_redirect)) return 0;
    spartanport_trap_redirects[spartanport_trap_redirect_count].addr = addr;
    spartanport_trap_redirects[spartanport_trap_redirect_count].target = target;
    ++spartanport_trap_redirect_count;
    return 1;
}
static void install_cheats(const char *app0, Segment *segments, uint64_t count, uint64_t size) {
    if (!cheat_identity(app0)) return;
    for (unsigned id = 0; id < SPARTAN_CHEAT_COUNT; ++id) {
        SpartanCheatSite sites[SPARTAN_CHEAT_HOOK_COUNT];
        if (spartanport_cheat_locate(sites, id, image, size, segments, count) < 1) {
            printf("FEX Cheats: %s OFF - no guest code patched\n", spartanport_cheat_names[id]);
            continue;
        }
        unsigned char *stubs = allocate(page_size);
        int ok = 1;
        for (unsigned i = 0; i < SPARTAN_CHEAT_HOOK_COUNT; ++i) {
            const SpartanCheatHook *hook = &spartanport_cheat_hooks[i];
            if (hook->id != id) continue;
            if (!sites[i].valid) { ok = 0; break; }
            spartanport_cheat_build_stub(stubs + i * 256, hook, spartanport_cheat_flag(id),
                                         (uintptr_t)(image + sites[i].at + hook->length));
        }
        if (!ok) {
            printf("FEX Cheats: %s unavailable (trampoline allocation)\n", spartanport_cheat_names[id]);
            continue;
        }
        protect(stubs, page_size, 5);
        /* trampoline 在游戏镜像之外，FEX 解码器只翻译 guest CPU 报告为可执行范围的地址，未注册
         * 范围会解码成非法指令（首次执行即 SIGILL/SIGSEGV）。把该页注册为 guest 代码让 JIT 翻译。 */
        gport_guest_cpu_code((uintptr_t)stubs, page_size);
        unsigned verified = 0;
        uint64_t first = 0;
        for (unsigned i = 0; i < SPARTAN_CHEAT_HOOK_COUNT; ++i) {
            const SpartanCheatHook *hook = &spartanport_cheat_hooks[i];
            if (hook->id != id) continue;
            if (!trap_redirect_add((uintptr_t)(image + sites[i].at), (uintptr_t)(stubs + i * 256))) {
                printf("FEX Cheats: %s unavailable (JIT hook rejected at 0x%lx)\n",
                       spartanport_cheat_names[id], (unsigned long)sites[i].at);
                ok = 0;
                break;
            }
            if (!verified) first = sites[i].at;
            ++verified;
        }
        if (!ok) continue;
        spartanport_cheat_status(id);
        /* flag 字节即菜单开关（SpartanSettings::cheats[id]）：上报是否已在本次启动开启。 */
        const uintptr_t flag = spartanport_cheat_flag(id);
        const int selected = flag ? *(const volatile unsigned char *)flag : 0;
        printf("FEX Cheats: %s READY, %u verified site(s), selected=%d, first=0x%lx\n",
               spartanport_cheat_names[id], verified, selected, (unsigned long)first);
    }
}

/* QTE 提示（src/qte_hook.h）的 JIT 路径：位点换成 2 字节 syscall 陷阱，陷阱 handler 把 rip
 * 重定向到 trampoline stub，stub 自己重放被替换的原始字节再跳回（与上面的金手指同款机制）。
 * 宿主侧的开关字节（菜单的 Automatic QTE）与计数跟客机同地址空间（actor slots 先例），所以
 * stub 能直接读写它们，不需要跨边界调用。 */

/* 摇杆提示：提示的 update 里记心跳、旋转判定里作答。两个位点都是函数入口（游戏的 call 已经
 * 把返回地址压栈），所以 stub 重放原始字节或直接 ret 都不必自己管栈。 */
static int install_qte_hooks_fex(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t sites[2] = {spartanport_signature_find(image, size, &spartanport_qte_update_site),
                               spartanport_signature_find(image, size, &spartanport_qte_rotation_site)};
    const unsigned char *originals[2] = {spartanport_qte_update_original, spartanport_qte_rotation_original};
    for (unsigned i = 0; i < 2; ++i) {
        if (sites[i] == UINT64_MAX || !mapped(segments, count, sites[i], SPARTAN_QTE_BLOCK_NEXT) ||
            memcmp(image + sites[i], originals[i], SPARTAN_QTE_HOOK_LENGTH)) {
            printf("FEX Stick prompts: unavailable (code not found in this eboot.bin)\n");
            return 0;
        }
    }
    const uint64_t block = spartanport_qte_block(image + sites[1], sites[1]);
    if (!mapped(segments, count, block, SPARTAN_QTE_TYPE_OFFSET + 2)) {
        printf("FEX Stick prompts: unavailable (prompt state outside the image)\n");
        return 0;
    }
    unsigned char *stubs = allocate(page_size);
    SpartanQteState *state = (SpartanQteState *)spartanport_qte_state_address();
    spartanport_qte_build_update_stub(stubs, (uintptr_t)&state->heartbeat,
                               (uintptr_t)(image + sites[0] + SPARTAN_QTE_HOOK_LENGTH));
    spartanport_qte_build_rotation_stub(stubs + 128, (uintptr_t)&state->pass, spartanport_auto_qte_flag(),
                                 (uintptr_t)(image + sites[1] + SPARTAN_QTE_HOOK_LENGTH));
    protect(stubs, page_size, 5);
    /* trampoline 在游戏镜像之外，FEX 解码器只翻译 guest CPU 报告为可执行范围的地址。 */
    gport_guest_cpu_code((uintptr_t)stubs, page_size);
    for (unsigned i = 0; i < 2; ++i) {
        if (!trap_redirect_add((uintptr_t)(image + sites[i]), (uintptr_t)(stubs + i * 128))) {
            printf("FEX Stick prompts: unavailable (JIT hook rejected at 0x%lx)\n", (unsigned long)sites[i]);
            return 0;
        }
    }
    state->block = (uint64_t)(uintptr_t)(image + block);
    printf("FEX Stick prompts: rotation prompt hooks ready\n");
    return 1;
}

/* Automatic QTE（src/qte_hook.h）的 JIT 路径：四个提示位点各自独立，找不到的那个把那类提示
 * 留给玩家自己按。repeat 位点是函数里的一次 call（不是函数入口），陷阱模型下没有 call 压下的
 * 返回地址，所以它用 qte_hook.h 里的 FEX 变体 stub：自动按下的路径不压栈直接跳回 call 之后，
 * 走游戏原函数的路径自己压一个返回地址。 */
static void install_auto_qte_hooks_fex(Segment *segments, uint64_t count, uint64_t size, int rotation) {
    const SpartanSignature *signatures[AUTO_QTE_SITES] = {&spartanport_qte_timed_site, &spartanport_qte_repeat_site,
                                                       &spartanport_qte_charged_site, &spartanport_qte_circle_site};
    const unsigned lengths[AUTO_QTE_SITES] = {SPARTAN_QTE_TIMED_LENGTH, 5, SPARTAN_QTE_CHARGED_LENGTH,
                                              SPARTAN_QTE_CIRCLE_LENGTH};
    const char *names[AUTO_QTE_SITES] = {"button", "repeated press", "held press", "Circle struggle"};
    const uintptr_t automatic = spartanport_auto_qte_flag();
    SpartanQteState *state = (SpartanQteState *)spartanport_qte_state_address();
    unsigned char *stubs = allocate(page_size);
    uint64_t sites[AUTO_QTE_SITES];
    int ready = 0;
    for (unsigned i = 0; i < AUTO_QTE_SITES; ++i) {
        sites[i] = spartanport_signature_find(image, size, signatures[i]);
        if (sites[i] == UINT64_MAX || !mapped(segments, count, sites[i], lengths[i])) {
            printf("FEX Automatic QTE: %s prompts unavailable (code not found in this eboot.bin)\n", names[i]);
            sites[i] = UINT64_MAX;
            continue;
        }
        unsigned char *stub = stubs + i * 128;
        const uintptr_t resume = (uintptr_t)(image + sites[i] + lengths[i]);
        /* The sites are in SPARTAN_QTE_COUNT_* order: each stub counts its kind's frames. */
        const uintptr_t frames = (uintptr_t)&state->kinds[i];
        if (i == 0) spartanport_qte_build_timed_stub(stub, frames, automatic, resume);
        else if (i == 1) spartanport_qte_build_repeat_stub_fex(stub, automatic,
                                                    (uintptr_t)(image + spartanport_qte_call_target(image + sites[i], sites[i])), resume);
        else if (i == 2) spartanport_qte_build_charged_stub(stub, frames, automatic, resume);
        else spartanport_qte_build_circle_stub(stub, frames, automatic, resume, (uintptr_t)(image + sites[i] + SPARTAN_QTE_CIRCLE_SKIP));
    }
    protect(stubs, page_size, 5);
    gport_guest_cpu_code((uintptr_t)stubs, page_size);
    for (unsigned i = 0; i < AUTO_QTE_SITES; ++i) {
        if (sites[i] == UINT64_MAX) continue;
        if (!trap_redirect_add((uintptr_t)(image + sites[i]), (uintptr_t)(stubs + i * 128))) {
            printf("FEX Automatic QTE: %s prompts unavailable (JIT hook rejected)\n", names[i]);
            continue;
        }
        ++ready;
    }
    if (!ready && !rotation) return;
    spartanport_qte_status(1);
    printf("FEX Automatic QTE: ready for %d of %d prompt kinds (menu, Game)\n", ready + rotation, AUTO_QTE_SITES + 1);
}

/* 暂停菜单的 "Exit to Desktop" 的 JIT 路径：镜像代码被翻译执行，不能像原生那样往镜像写
 * 跳板——位点换成 2 字节 syscall 陷阱（与金手指、QTE 同一套机制），handler 把 rip 重定向
 * 到 trampoline stub。stub 的字节与上游完全一致（0xfff → 退出进程，0xf00..0xf60 /
 * 0xffd / 0xffe → 我们的 settings provider，其余原样重放）；只有两处不同：
 *   - stub 里调的"退出"必须是 guest 可调用的蹦床（gport_guest_cpu_host_function），
 *     客机不能直接跳到宿主的原生地址；
 *   - provider vtable 里那一项同理，放蹦床地址而不是宿主函数地址。
 * 钩子装好之后才把打好补丁的 R_UI.WAD 挂上去（否则那行点了没人处理）。 */
static uintptr_t menu_vtable[4] = {0, 0, 0, 0};
static const uintptr_t *menu_provider = menu_vtable;

/* 敌人血条（src/actor_hook.h）的 JIT 路径：位点是伤害记录函数里的 7 字节（函数的序言），
 * stub 记下这次命中（敌人的血量/上限）后跳回函数继续。序言位点没有 call 的栈语义，
 * stub 直接跳回即可。 */
static void install_actor_watch_fex(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t at = spartanport_signature_find(image, size, &spartanport_actor_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, SPARTAN_ACTOR_HOOK_LENGTH) ||
        !spartanport_actor_signature_match(image + at, size - at)) {
        printf("FEX Health display: unavailable (code not found in this eboot.bin)\n");
        return;
    }
    unsigned char *stub = allocate(page_size);
    spartanport_actor_build_stub(stub, spartanport_actor_slots_address(),
                                 (uintptr_t)(image + at + SPARTAN_ACTOR_HOOK_LENGTH));
    protect(stub, page_size, 5);
    gport_guest_cpu_code((uintptr_t)stub, page_size);
    printf("FEX Health display: stub page at %p\n", (void *)stub);
    if (!trap_redirect_add((uintptr_t)(image + at), (uintptr_t)stub)) {
        printf("FEX Health display: unavailable (JIT hook rejected at 0x%lx)\n", (unsigned long)at);
        return;
    }
    spartanport_actor_status(1);
    puts("FEX Health display: damage hook ready (enemy health bar in the menu)");
}

/* 伤害倍率与绿/蓝/金球倍率（src/damage_hook.h、src/orb_hook.h）的 JIT 路径：两个位点都是函数
 * 序言，stub 读完菜单里的倍数就跳回函数继续（倍率当次生效）。 */
static void install_stat_multipliers_fex(Segment *segments, uint64_t count, uint64_t size) {
    const uint64_t damage_at = spartanport_signature_find(image, size, &spartanport_damage_site);
    const uint64_t pickup_at = spartanport_signature_find(image, size, &spartanport_orb_pickup_site);
    int damage = damage_at != UINT64_MAX && mapped(segments, count, damage_at, SPARTAN_DAMAGE_HOOK_LENGTH) &&
                 spartanport_damage_signature_match(image + damage_at, size - damage_at);
    int pickup = pickup_at != UINT64_MAX && mapped(segments, count, pickup_at, SPARTAN_ORB_PICKUP_LENGTH) &&
                 spartanport_orb_pickup_signature_match(image + pickup_at, size - pickup_at);
    unsigned char *stubs = damage || pickup ? allocate(page_size) : NULL;
    if (damage)
        spartanport_damage_build_stub(stubs, spartanport_damage_multiplier(1), spartanport_damage_multiplier(0),
                               (uintptr_t)(image + damage_at + SPARTAN_DAMAGE_HOOK_LENGTH));
    if (pickup)
        spartanport_orb_pickup_build_stub(stubs + 256, spartanport_orb_pickup_multiplier(0),
                                   spartanport_orb_pickup_multiplier(1), spartanport_orb_pickup_multiplier(2),
                                   (uintptr_t)(image + pickup_at + SPARTAN_ORB_PICKUP_LENGTH));
    if (stubs) {
        protect(stubs, page_size, 5);
        gport_guest_cpu_code((uintptr_t)stubs, page_size);
    }
    if (damage && !trap_redirect_add((uintptr_t)(image + damage_at), (uintptr_t)stubs)) damage = 0;
    if (pickup && !trap_redirect_add((uintptr_t)(image + pickup_at), (uintptr_t)(stubs + 256))) pickup = 0;
    spartanport_stat_multipliers_status(damage, pickup);
    printf("FEX Multipliers: damage %s, green/blue/gold orbs %s\n", damage ? "ready" : "unavailable",
           pickup ? "ready" : "unavailable");
}

/* 红球倍率（src/orb_hook.h）的 JIT 路径：三个位点是函数体内的 call（不是序言）——陷阱替掉 call
 * 的头两个字节后栈上没有返回地址，所以 stub 用 orb_hook.h 里的 FEX 变体：自己压一个返回地址给
 * 游戏原函数 ret，并把 SysV 的对齐补齐。helper 是宿主函数，地址要用 guest 可调用的蹦床。 */
static void install_orb_multiplier_fex(Segment *segments, uint64_t count, uint64_t size) {
    uint64_t calls[3];
    uint64_t setter_at = spartanport_orb_find(image, size, calls);
    if (setter_at == UINT64_MAX) {
        /* 上游的找法是"先找 setter 的签名，再找三个指向它的 call"。setter 的签名在这份
         * eboot 里不唯一时就反过来：三个 call 位点唯一命中时，它们共同指向的那个函数就是
         * setter（再校验它的序言字节）——call 的 rel32 是链接器写死的，比签名稳。 */
        uint64_t found[3];
        unsigned hits = 0;
        uint64_t target = 0;
        for (unsigned i = 0; i < 3; ++i) {
            found[i] = spartanport_signature_find(image, size, &spartanport_orb_call_sites[i]);
            if (found[i] == UINT64_MAX || image[found[i]] != 0xe8) continue;
            int32_t relative;
            memcpy(&relative, image + found[i] + 1, 4);
            const uint64_t to = (uint64_t)((int64_t)found[i] + 5 + relative);
            if (!hits || to == target) { target = to; ++hits; }
            else hits = 0;
        }
        if (hits == 3 && mapped(segments, count, target, SPARTAN_ORB_SETTER_PROLOGUE_LENGTH) &&
            !memcmp(image + target, spartanport_orb_setter_prologue, SPARTAN_ORB_SETTER_PROLOGUE_LENGTH)) {
            setter_at = target;
            for (unsigned i = 0; i < 3; ++i) calls[i] = found[i];
            printf("FEX Red orbs: setter taken from the three call targets (0x%llx)\n",
                   (unsigned long long)setter_at);
        } else {
            printf("FEX Red orbs: unavailable (setter signature not unique, call sites %s)\n",
                   found[0] == UINT64_MAX || found[1] == UINT64_MAX || found[2] == UINT64_MAX
                       ? "not all unique" : "unique");
            return;
        }
    }
    for (unsigned i = 0; i < 3; ++i)
        if (!mapped(segments, count, calls[i], 5)) return;
    const uintptr_t helper = (uintptr_t)gport_guest_cpu_host_function((void *)spartanport_orbs_gain);
    const uintptr_t setter = (uintptr_t)(image + setter_at);
    unsigned char *stubs = allocate(page_size);
    for (unsigned i = 0; i < 3; ++i)
        spartanport_orb_build_stub_fex(stubs + i * 128, helper, setter, (uintptr_t)(image + calls[i] + 5));
    protect(stubs, page_size, 5);
    gport_guest_cpu_code((uintptr_t)stubs, page_size);
    printf("FEX Red orbs: stub page at %p\n", (void *)stubs);
    for (unsigned i = 0; i < 3; ++i) {
        if (!trap_redirect_add((uintptr_t)(image + calls[i]), (uintptr_t)(stubs + i * 128))) {
            printf("FEX Red orbs: multiplier unavailable (JIT hook rejected at 0x%lx)\n",
                   (unsigned long)calls[i]);
            return;
        }
    }
    spartanport_orbs_status(1);
    puts("FEX Red orbs: live multiplier ready (3 validated gain paths, 0.1x to 100x)");
}

static void install_menu_exit_fex(Segment *segments, uint64_t count, uint64_t size, const char *wad) {
    const uint64_t at = spartanport_signature_find(image, size, &spartanport_menu_site);
    if (at == UINT64_MAX || !mapped(segments, count, at, SPARTAN_MENU_HOOK_LENGTH) ||
        !spartanport_menu_signature_match(image + at, size - at)) {
        printf("FEX Pause menu: Exit to Desktop unavailable (code not found in this eboot.bin)\n");
        return;
    }
    menu_vtable[3] = (uintptr_t)gport_guest_cpu_host_function((void *)menu_setting);
    const uintptr_t quit = (uintptr_t)gport_guest_cpu_host_function((void *)menu_quit);
    unsigned char *stub = allocate(page_size);
    spartanport_menu_build_stub(stub, quit, (uintptr_t)(image + at + SPARTAN_MENU_HOOK_LENGTH),
                               (uintptr_t)&menu_provider);
    protect(stub, page_size, 5);
    /* trampoline 在游戏镜像之外：注册为 guest 代码，JIT 才会翻译它。 */
    gport_guest_cpu_code((uintptr_t)stub, page_size);
    if (!trap_redirect_add((uintptr_t)(image + at), (uintptr_t)stub)) {
        printf("FEX Pause menu: Exit to Desktop unavailable (JIT hook rejected at 0x%lx)\n", (unsigned long)at);
        return;
    }
    runtime_file_mount("/app0/EXEC/WAD/PS4_LE/R_UI.WAD", wad);
    puts("FEX Pause menu: Exit to Desktop ready");
}
#endif

static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"G3PATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    /* run.py sets SPARTAN_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command=getenv("SPARTAN_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
    if (!command || !*command) { fputs("runtime_restart: SPARTAN_RESTART_COMMAND is not set\n",stderr); _exit(1); }
    char *line=_strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
    TerminateProcess(GetCurrentProcess(),0);
#else
    puts("Runtime: restarting through run.sh");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    /* gport: pad shim 按需注入（SPARTAN_PAD_LIB）——不再走全局 LD_PRELOAD：链路上的
     * python/sed/tr 每进程干等 shim 的 SDL3 重试 30 秒（迁移专栏 09 轮次 8）。虚拟手柄
     * 只属于本进程（游戏的 SDL3 在这里），由我们自己加载。 */
    const char *pad_lib = getenv("SPARTAN_PAD_LIB");
    if (pad_lib && !dlopen(pad_lib, RTLD_NOW | RTLD_GLOBAL))
        fprintf(stderr, "pad shim: %s\n", dlerror());
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
    /* The guest CPU allocates the memory the guest may hold pointers to through this (below
       1 TiB). No-op on x86-64 hosts (src/gport_runtime_cpu.c). */
    gport_guest_cpu_init(runtime_low_map);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0, check_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *isa_file=NULL, *pause_menu_wad=NULL;
    const char *mounts[24]; int mount_count=0;  /* glyphs (3) and the switch attack tutorials (16) */
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--isa-rewrite") && i+1<argc) isa_file=argv[++i];
        else if (!strcmp(argv[i], "--pause-menu") && i+1<argc) pause_menu_wad = argv[++i];
        else if (!strcmp(argv[i], "--mount") && i+1<argc) {
            /* guest=host; the guest part never has '=', so the first one splits */
            const char *spec=argv[++i], *eq=strchr(spec,'=');
            if (mount_count==24 || !eq || eq==spec || !eq[1]) { fprintf(stderr, "Bad --mount (guest=host, at most 24): %s\n", spec); return 1; }
            mounts[mount_count++]=spec;
        }
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        /* gport: 上游 launcher 会传 --pause-menu <WAD>（游戏内暂停菜单的 Exit-to-Desktop 行：
         * 行本身由 scripts/pause_menu.py 打在 R_UI.WAD 里，位点钩子由 install_menu_exit 装
         * 上——FEX 路径走陷阱，见下）。原来这里"接受并忽略"，现在存值传给安装器。 */
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--check-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"G3CONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        if (user_dir && user_dir[0]) {
#ifdef _WIN32
            _putenv_s("SPARTAN_USER_DIR", user_dir);
#else
            setenv("SPARTAN_USER_DIR", user_dir, 1);
#endif
        }
        for (int i = 0; i < mount_count; ++i) {
            const char *eq = strchr(mounts[i], '=');
            char guest[64];
            size_t glen = (size_t)(eq - mounts[i]);
            if (glen >= sizeof(guest)) { fprintf(stderr, "Cannot mount %s: guest path longer than 63 characters\n", mounts[i]); return 1; }
            memcpy(guest, mounts[i], glen); guest[glen] = 0;
            if (runtime_file_mount(guest, eq + 1)) { fprintf(stderr, "Cannot mount %s: too many mounts or host path longer than 511 characters\n", mounts[i]); return 1; }
            printf("Runtime: mount %s -> %s\n", guest, eq + 1);
        }
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    spartanportgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    if (getenv("SPARTAN_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"SPARTAN_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "G3PROBE1", 8) && memcmp(magic, "G3PROBE2", 8) && memcmp(magic,"G3PROBE3",8) && memcmp(magic,"G3PROBE4",8) && memcmp(magic,"G3PROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "G3PROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"G3PROBE5",8);
    int linked=!memcmp(magic,"G3PROBE3",8) || !memcmp(magic,"G3PROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* G3PROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"G3PROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="God of War III", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        SpartanGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (spartanportgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,spartanportgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    /* Guest code ranges: the image itself and the import traps. Done through the boundary so the
     * JIT can translate them (no-op on x86-64 hosts). */
    gport_guest_cpu_code((uintptr_t)image, round_page(size));
    gport_guest_cpu_code((uintptr_t)traps, round_page((import_count + 1) * 32));
    /* The stub below jumps here as guest code: on x86-64 this is the raw function; with the JIT
     * it is the guest-callable thunk for it. */
    const uintptr_t handler = gport_guest_cpu_host_function((void *)unresolved);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            /* Host functions handed to the guest are wrapped: the guest calls them in its own
             * convention, the boundary converts (a plain cast on x86-64 hosts). */
            if (resolved && relocs[i].kind == 1) value = gport_guest_cpu_host_function((void *)resolved);
            else if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (check_imports) {
        /* Lists every import the game would stop at, without running it (needs the GPU
         * library: without --cpu-only its symbols are registered above). */
        unsigned char *checked=calloc(import_count ? import_count : 1,1);
        if (!checked) fail("allocation failed");
        uint64_t missing=0;
        for (uint64_t i=0;i<nr;++i) {
            uint64_t index=relocs[i].value;
            if (!relocs[i].kind || checked[index]) continue;
            checked[index]=1;
            if (runtime_resolve(names[index],relocs[i].kind==2) || (native_libc && bindings[index])) continue;
            printf("MISSING: %s (%s)\n",names[index],runtime_import_name(names[index]));
            ++missing;
        }
        printf("Imports: %" PRIu64 " missing\n",missing);
        fflush(NULL);
        _exit(missing ? 1 : 0); /* no destructors: GPU threads are running */
    }
if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    /* 引擎帧率的两个全局要在镜像受保护之前定位（签名搜索要连段间空隙一起读）。 */
    if (gpu_enabled) find_engine_fps(segments, ns, size);
#if defined(GUEST_CPU_NATIVE)
    /* These hooks write x86-64 machine-code stubs straight into the image (0xe9 jumps), which is
     * only meaningful when the game's code runs on the host CPU. Under the JIT the image is
     * translated, so live code patching needs the JIT's own hook path (gport_guest_cpu_hook):
     * the FEX branch below installs its own versions of the same hooks. */
    if (gpu_enabled) install_orb_multiplier(segments, ns, size);
    if (gpu_enabled) install_cheats(app0, segments, ns, size);
    if (gpu_enabled) find_power_up_menu(segments, ns, size);
    if (gpu_enabled) install_actor_watch(segments, ns, size);
    if (gpu_enabled) install_player_hook(segments, ns, size);
    if (gpu_enabled) install_wall_seam_fix(segments, ns, size);
    if (gpu_enabled) install_stat_multipliers(segments, ns, size);
    if (gpu_enabled) install_graphics(segments, ns, size);
    if (gpu_enabled) install_auto_qte_hooks(segments, ns, size, install_qte_hooks(segments, ns, size));
    if (gpu_enabled) install_switch_hooks(segments, ns, size);
    if (gpu_enabled && pause_menu_wad && app0) install_menu_exit(segments, ns, size, pause_menu_wad);
#else
    /* FEX JIT：金手指经 gport_guest_cpu_hook（syscall 陷阱 + rip 重定向到 trampoline stub）
     * 在 install_cheats 中安装；orb/敌血条/伤害倍率安装器仍需各自的 JIT 路径，暂不安装。 */
    if (gpu_enabled) install_cheats(app0, segments, ns, size);
    else printf("FEX Cheats: OFF - the GPU library was not initialized (no cheats installed)\n");
    /* QTE 提示与 Automatic QTE 走同一条陷阱路径（各自的位点与 trampoline）。 */
    if (gpu_enabled) install_auto_qte_hooks_fex(segments, ns, size, install_qte_hooks_fex(segments, ns, size));
    /* 敌血条、伤害/球倍率、红球倍率：位点走陷阱路径（stub 读宿主侧的菜单设置与计数）。 */
    if (gpu_enabled) install_actor_watch_fex(segments, ns, size);
    if (gpu_enabled) install_stat_multipliers_fex(segments, ns, size);
    if (gpu_enabled) install_orb_multiplier_fex(segments, ns, size);
    /* 暂停菜单的 Exit to Desktop：位点走陷阱路径，WAD 由安装器在钩子装好后挂上。 */
    if (gpu_enabled && pause_menu_wad && app0) install_menu_exit_fex(segments, ns, size, pause_menu_wad);
#endif
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    /* 镜像受保护之后：present 线程每帧读那两个帧率全局。 */
    if (gpu_enabled) publish_engine_fps();
#if defined(GUEST_CPU_NATIVE)
    publish_power_up_menu();
#endif
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_orbis_fpu(); /* the PS4's floating-point state from the first guest code on */
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            const uint64_t init_args[3]={0,0,0};
            int result=(int)gport_guest_cpu_call((uintptr_t)(image+modules[m].init),3,init_args);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    /* Static: the host stack may lie above 47-bit guest addresses (arm64 hosts). */
    static struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
#if defined(GUEST_CPU_NATIVE)
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#else
    /* Tell the guest CPU where this thread's guest stack is, then enter the image through the
     * boundary: it converts to the guest's convention and runs the JIT. */
    gport_guest_cpu_thread_stack(stack+MAIN_STACK-64,MAIN_STACK-64);
    const uint64_t entry_args[2]={(uint64_t)(uintptr_t)&params,gport_guest_cpu_host_function((void *)guest_exit)};
    gport_guest_cpu_call((uintptr_t)(image+entry),2,entry_args);
#endif
#endif
    fail("entry unexpectedly returned");
}
