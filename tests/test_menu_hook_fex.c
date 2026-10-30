/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The pause menu's builtin-call stub (src/menu_hook.h) as the game calls it: any other function
 * replays the replaced instructions and resumes; function 0xfff goes to quit; the GBL provider's
 * settings functions (0xf00..0xf60, 0xffd, 0xffe) go to our settings provider instead.
 * x86-64 only (the stub is x86-64 code).
 *
 * The upstream's tests/test_menu_hook.cpp covers the same stub but only builds on Windows; this
 * one runs anywhere with an x86-64 (or Rosetta). */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../src/menu_hook.h"

/* The stub ends with a jump to its resume address; here that is a plain function that returns
 * to the test (the harness called the stub, so the return address is on the stack). */
static void menu_hook_return(void) {}

static jmp_buf back;
static int quits;
static void quit(void) {
    ++quits;
    longjmp(back, 1);  /* no unwinding through the stub */
}

/* The interpreter frame: [rbp-0x2a0] is the provider table, rax the provider; rdi gets the entry. */
static uint64_t frame[96];
static uint64_t providers[2] = {0x1111, 0x2222};
static uint64_t native_provider;

static uint64_t call(unsigned char* stub, uint32_t function, uint64_t provider) {
    uint64_t rdi;
    __asm__ volatile("push %%rbp\n\tmov %%rbx, %%rbp\n\tcall *%%rcx\n\tpop %%rbp"
                     : "=D"(rdi), "+c"(stub), "+a"(provider), "+S"(function)
                     : "b"(frame + 96)
                     : "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    return rdi;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!spartanport_menu_signature_match(spartanport_menu_original, SPARTAN_MENU_HOOK_LENGTH) ||
        spartanport_menu_signature_match(spartanport_menu_original, SPARTAN_MENU_HOOK_LENGTH - 1)) {
        puts("FAIL: the signature matcher accepts the wrong length");
        return 1;
    }
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char* stub = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) {
        puts("FAIL: no executable page");
        return 1;
    }
    const unsigned n = spartanport_menu_build_stub(stub, (uintptr_t)&quit,
                                                   (uintptr_t)&menu_hook_return,
                                                   (uintptr_t)&native_provider);
    if (n >= 256) {
        printf("FAIL: the stub does not fit in its slot (%u bytes)\n", n);
        return 1;
    }
    frame[96 - 0x2a0 / 8] = (uint64_t)(uintptr_t)providers;

    /* A game builtin: the original provider lookup. */
    if (call(stub, 0x5a, 1) != 0x2222) {
        puts("FAIL: a game builtin did not take the original lookup");
        return 1;
    }
    /* 0xffc is not ours: the original lookup, and no quit. */
    if (call(stub, 0xffc, 0) != 0x1111 || quits != 0) {
        puts("FAIL: 0xffc was treated as ours");
        return 1;
    }
    /* The settings functions of the GBL provider (0) go to our provider. */
    if (call(stub, 0xffd, 0) != (uintptr_t)&native_provider ||
        call(stub, 0xffe, 0) != (uintptr_t)&native_provider ||
        call(stub, 0xf00, 0) != (uintptr_t)&native_provider ||
        call(stub, 0xf60, 0) != (uintptr_t)&native_provider) {
        puts("FAIL: the settings functions did not reach our provider");
        return 1;
    }
    /* Only the GBL provider (index 0) is extended: another provider keeps its own entry. */
    if (call(stub, 0xffe, 1) != 0x2222) {
        puts("FAIL: another provider's 0xffe was hijacked");
        return 1;
    }
    /* Just outside the range: the original lookup. */
    if (call(stub, 0xf61, 0) != 0x1111 || call(stub, 0xeff, 0) != 0x1111) {
        puts("FAIL: a function outside our range was hijacked");
        return 1;
    }
    /* The exit function: the stub calls quit, which does not return. */
    if (!setjmp(back)) {
        call(stub, SPARTAN_MENU_EXIT_FUNCTION, 0);
        puts("FAIL: quit returned");
        return 1;
    }
    if (quits != 1) {
        printf("FAIL: quit ran %d times\n", quits);
        return 1;
    }
    puts("PASS: menu hook stub");
    return 0;
}
