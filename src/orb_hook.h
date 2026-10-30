/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SPARTAN_ORB_HOOK_H
#define SPARTAN_ORB_HOOK_H
#include <stdint.h>
#include <string.h>
#include "signature.h"

/* The red orb setter (image offset 0x6a9f0 in the reference build) and its three callers that
 * add gains, found by signature in any build; each call must still target the setter. */
static const SpartanSignature spartanport_orb_setter_site = {"55 48 89 e5 c5 fa 5d 05 ?? ?? ?? ?? c5 fa 11 87 e8 01 00 00", 0};
static const SpartanSignature spartanport_orb_call_sites[3] = {
    {"48 89 df e8 ?? ?? ?? ?? c4 e1 fa 2c 75 f0 bf 04 00 00 00", 3},
    {"c4 e1 fa 2a c8 c5 fa 58 c1 e8 ?? ?? ?? ?? 8b b3 44 01 00 00", 9},
    {"c4 e1 fa 2a c8 c5 fa 58 c1 4c 89 f7 e8 ?? ?? ?? ??", 12}
};
static inline void spartanport_orb_build_stub(unsigned char code[26], uintptr_t helper, uintptr_t setter) {
    static const unsigned char pattern[26] = {
        0x57,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0x5f,
        0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0
    };
    memcpy(code, pattern, 26);
    memcpy(code + 3, &helper, 8);
    memcpy(code + 16, &setter, 8);
}

/* FEX JIT 路径的变体：位点被换成 2 字节 syscall 陷阱，不再是 call——栈上没有 call 压下的返回
 * 地址，原生版的 "jmp setter" 会让原函数 ret 弹到垃圾。所以 stub 自己压一个返回地址（游戏原
 * 函数 ret 时从那里回到 call 之后），并把对齐补齐：陷阱入口 rsp ≡ 0 (mod 16)，SysV 要求
 * helper 的调用点 rsp ≡ 0、被调方入口 rsp ≡ 8。helper 是宿主函数，地址必须是 guest 可调用的
 * 蹦床（gport_guest_cpu_host_function）——客机不能直接跳到宿主的原生地址。 */
static inline unsigned spartanport_orb_build_stub_fex(unsigned char *out, uintptr_t helper, uintptr_t setter,
                                              uintptr_t resume) {
    static const unsigned char body[] = {
        0x57,                            /* push rdi: the receiver survives the helper */
        0x48,0x83,0xec,0x08,             /* sub rsp, 8: the helper's call wants rsp ≡ 0 */
        0x48,0xb8, 0,0,0,0,0,0,0,0,     /* movabs rax, helper */
        0xff,0xd0,                       /* call rax */
        0x48,0x83,0xc4,0x08,             /* add rsp, 8 */
        0x5f,                            /* pop rdi */
        0x48,0x83,0xec,0x08,             /* sub rsp, 8: the setter wants rsp ≡ 8 at its entry */
        0x48,0xb8, 0,0,0,0,0,0,0,0,     /* movabs rax, resume */
        0x48,0x89,0x04,0x24,             /* mov [rsp], rax: the return address the setter pops */
        0x48,0xb8, 0,0,0,0,0,0,0,0,     /* movabs rax, setter */
        0xff,0xe0,                       /* jmp rax */
    };
    memcpy(out, body, sizeof(body));
    memcpy(out + 7, &helper, 8);
    memcpy(out + 28, &resume, 8);
    memcpy(out + 42, &setter, 8);
    return sizeof(body);
}
/* The setter's offset with its three call sites in calls[], or UINT64_MAX when the code is not
 * found or a call does not reach the setter. */
static inline uint64_t spartanport_orb_find(const unsigned char *image, uint64_t size, uint64_t calls[3]) {
    const uint64_t setter = spartanport_signature_find(image, size, &spartanport_orb_setter_site);
    if (setter == UINT64_MAX) return UINT64_MAX;
    for (unsigned i = 0; i < 3; ++i) {
        calls[i] = spartanport_signature_find(image, size, &spartanport_orb_call_sites[i]);
        int32_t relative;
        if (calls[i] == UINT64_MAX || image[calls[i]] != 0xe8) return UINT64_MAX;
        memcpy(&relative, image + calls[i] + 1, 4);
        if ((int64_t)calls[i] + 5 + relative != (int64_t)setter) return UINT64_MAX;
    }
    return setter;
}

/* Orb pickup, image offset 0x6a480 (one caller, 0xbf496): rdi is the player's stats; xmm0
 * health (green), xmm1 magic (blue), xmm2 Rage of Sparta (gold), xmm3 red orbs (scaled by the
 * hooks above). The routine adds each positive amount and clamps it to the bar. The stub
 * replaces the prologue at the entry, where rax, xmm4 and the flags are free. */
enum { SPARTAN_ORB_PICKUP_OFFSET = 0x6a480, SPARTAN_ORB_PICKUP_LENGTH = 9 };
static const unsigned char spartanport_orb_pickup_original[SPARTAN_ORB_PICKUP_LENGTH] = {
    0x55,                /* push rbp */
    0x48,0x89,0xe5,      /* mov rbp, rsp */
    0x53,                /* push rbx */
    0x48,0x83,0xec,0x18  /* sub rsp, 0x18 */
};
static const SpartanSignature spartanport_orb_pickup_site = {"55 48 89 e5 53 48 83 ec 18 c5 fa 11 5d f0 c5 fa 11 55 f4", 0};

static inline int spartanport_orb_pickup_signature_match(const unsigned char *code, uint64_t size) {
    return size >= SPARTAN_ORB_PICKUP_LENGTH && !memcmp(code, spartanport_orb_pickup_original, SPARTAN_ORB_PICKUP_LENGTH);
}

/* Each amount above zero is multiplied by the float at green, blue or gold. */
static inline unsigned spartanport_orb_pickup_build_stub(unsigned char *out, uintptr_t green, uintptr_t blue,
                                                  uintptr_t gold, uintptr_t resume) {
    static const unsigned char body[] = {
        0xc5,0xd8,0x57,0xe4,                 /* vxorps xmm4, xmm4, xmm4 */
        0x48,0xb8, 0,0,0,0,0,0,0,0,          /* movabs rax, green */
        0xc5,0xf8,0x2e,0xc4,                 /* vucomiss xmm0, xmm4 */
        0x76,0x04,                           /* jbe +4 */
        0xc5,0xfa,0x59,0x00,                 /* vmulss xmm0, xmm0, [rax] */
        0x48,0xb8, 0,0,0,0,0,0,0,0,          /* movabs rax, blue */
        0xc5,0xf8,0x2e,0xcc,                 /* vucomiss xmm1, xmm4 */
        0x76,0x04,                           /* jbe +4 */
        0xc5,0xf2,0x59,0x08,                 /* vmulss xmm1, xmm1, [rax] */
        0x48,0xb8, 0,0,0,0,0,0,0,0,          /* movabs rax, gold */
        0xc5,0xf8,0x2e,0xd4,                 /* vucomiss xmm2, xmm4 */
        0x76,0x04,                           /* jbe +4 */
        0xc5,0xea,0x59,0x10,                 /* vmulss xmm2, xmm2, [rax] */
    };
    static const unsigned char jump[] = {0xff,0x25,0,0,0,0}; /* jmp [rip+0] */
    unsigned n = 0;
    memcpy(out, body, sizeof(body));
    memcpy(out + 6, &green, 8);
    memcpy(out + 26, &blue, 8);
    memcpy(out + 46, &gold, 8);
    n += sizeof(body);
    memcpy(out + n, spartanport_orb_pickup_original, SPARTAN_ORB_PICKUP_LENGTH);
    n += SPARTAN_ORB_PICKUP_LENGTH;
    memcpy(out + n, jump, sizeof(jump));
    n += sizeof(jump);
    memcpy(out + n, &resume, 8);
    return n + 8;
}
#endif
