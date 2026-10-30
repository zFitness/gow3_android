/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The FEX JIT path's repeat QTE stub (src/qte_hook.h). Under the JIT the prompt's call becomes a
 * 2-byte syscall trap instead of a call, so the stub cannot count on a return address on the
 * stack: it does the call's bookkeeping itself. x86-64 only (the stub is x86-64 code).
 *
 * The stub is entered the way the trap enters it (a jump, no return address of its own); the
 * game's own function is entered the way the stub jumps into it. The stub's bytes are printed
 * so the caller can check the layout. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../src/qte_hook.h"

/* Where the stub jumps back into the game: the game continues there and reads eax. The landing
 * pad records eax and returns; the stack holds the game's return address (the call the stub
 * replaced is not there, so the stub pushed one itself in the game's own path). */
#ifdef __APPLE__
extern void qte_fex_resume(void) __asm__("_qte_fex_resume");
extern uint64_t qte_fex_resume_rax __asm__("_qte_fex_resume_rax");
#define QTE_RESUME_ASM                          \
    ".globl _qte_fex_resume\n"                   \
    "_qte_fex_resume:\n"                        \
    "movq %rax, _qte_fex_resume_rax(%rip)\n"    \
    "ret\n"
#else
extern void qte_fex_resume(void) __asm__("qte_fex_resume");
extern uint64_t qte_fex_resume_rax __asm__("qte_fex_resume_rax");
#define QTE_RESUME_ASM                          \
    ".globl qte_fex_resume\n"                   \
    "qte_fex_resume:\n"                         \
    "movq %rax, qte_fex_resume_rax(%rip)\n"     \
    "ret\n"
#endif
__asm__(QTE_RESUME_ASM);
uint64_t qte_fex_resume_rax;

static int original_called;
static int qte_original(void) { ++original_called; return 7; }

static uint8_t automatic;  /* the menu's Automatic QTE byte */

/* The prompt's state (rbx) and the update's frame (rbp): the frame time is at rbp-0x3c and the
 * stub writes it into the state at rbx+8 — the press credit of a mash prompt. */
static float qte_state[4];
static char qte_frame[128];

static void call_stub(uint8_t* stub) {
    __asm__ volatile("push %%rbp\n\tmov %2, %%rbp\n\tmov %1, %%rbx\n\tcall *%0\n\tpop %%rbp"
                     : : "r"(stub), "r"(qte_state), "r"(qte_frame + 0x40) : "rax", "rcx", "rdx", "rbx", "memory");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* The stub is x86-64 code: it needs an executable page. */
    const long page = sysconf(_SC_PAGESIZE);
    uint8_t* code = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        puts("FAIL: no executable page");
        return 1;
    }
    uint8_t* stub = code;
    const uintptr_t automatic_flag = (uintptr_t)&automatic;
    const uintptr_t resume = (uintptr_t)&qte_fex_resume;
    const uintptr_t original = (uintptr_t)&qte_original;
    const unsigned n = spartanport_qte_build_repeat_stub_fex(stub, automatic_flag, original, resume);
    if (n >= 128) {
        printf("FAIL: the stub does not fit in its slot (%u bytes)\n", n);
        return 1;
    }
    printf("bytes:");
    for (unsigned i = 0; i < n; ++i) printf(" %02x", stub[i]);
    printf("\n");

    /* The game's own path: the stub pushes the resume address and jumps into the game's
     * function, whose ret comes back to the resume. */
    call_stub(stub);
    if (original_called != 1) {
        printf("FAIL: the game's own function ran %d times (expected 1)\n", original_called);
        return 1;
    }
    if (qte_fex_resume_rax != 7) {
        printf("FAIL: eax after the game's own function is %llu (expected 7)\n",
               (unsigned long long)qte_fex_resume_rax);
        return 1;
    }

    /* The automatic path: the stub answers 1 itself and jumps back into the game, without the
     * game's own function. It also credits the press with this frame's time (the state's +8) —
     * without that the prompt's bar never fills and the prompt never completes. */
    automatic = 1;
    qte_fex_resume_rax = 0;
    memset(qte_state, 0, sizeof(qte_state));
    const float dt = 1.0f / 120;
    memcpy(qte_frame + 0x40 - 0x3c, &dt, 4);
    call_stub(stub);
    if (original_called != 1) {
        puts("FAIL: the game's own function ran with Automatic QTE on");
        return 1;
    }
    if (qte_fex_resume_rax != 1) {
        printf("FAIL: eax with Automatic QTE is %llu (expected 1)\n",
               (unsigned long long)qte_fex_resume_rax);
        return 1;
    }
    if (qte_state[2] != dt) {
        printf("FAIL: the press credit is %f (expected %f)\n", (double)qte_state[2], (double)dt);
        return 1;
    }
    puts("PASS: QTE FEX stub");
    return 0;
}
