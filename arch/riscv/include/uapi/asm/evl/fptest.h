/*
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Derived from Xenomai Cobalt, https://xenomai.org/
 * Copyright (C) 2006 Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>.
 */
#ifndef _EVL_RISCV_ASM_UAPI_FPTEST_H
#define _EVL_RISCV_ASM_UAPI_FPTEST_H

#include <linux/types.h>

#define evl_riscv_fpsimd  0x1
#define evl_riscv_sve     0x2

/*
 * CAUTION: keep this code strictly inlined in macros: we don't want
 * GCC to apply any callee-saved logic to fpsimd registers in
 * evl_set_fpregs() before evl_check_fpregs() can verify their
 * contents, but we still want GCC to know about the registers we have
 * clobbered.
 */

#define evl_set_fpregs(__features, __val)              \
    do {                                                \
        unsigned int __i;                               \
        __u64 __e[32];                                  \
                                                        \
        if (__features & evl_riscv_fpsimd) {             \
            for (__i = 0; __i < 32; __i++)              \
                __e[__i] = (__val);                     \
            __asm__ __volatile__(                       \
                "fld  f0, 0(%0)  \n"                    \
                "fld  f1, 8(%0)  \n"                    \
                "fld  f2, 16(%0) \n"                    \
                "fld  f3, 24(%0) \n"                    \
                "fld  f4, 32(%0) \n"                    \
                "fld  f5, 40(%0) \n"                    \
                "fld  f6, 48(%0) \n"                    \
                "fld  f7, 56(%0) \n"                    \
                "fld  f8, 64(%0) \n"                    \
                "fld  f9, 72(%0) \n"                    \
                "fld  f10, 80(%0) \n"                   \
                "fld  f11, 88(%0) \n"                   \
                "fld  f12, 96(%0) \n"                   \
                "fld  f13, 104(%0) \n"                  \
                "fld  f14, 112(%0) \n"                  \
                "fld  f15, 120(%0) \n"                  \
                "fld  f16, 128(%0) \n"                  \
                "fld  f17, 136(%0) \n"                  \
                "fld  f18, 144(%0) \n"                  \
                "fld  f19, 152(%0) \n"                  \
                "fld  f20, 160(%0) \n"                  \
                "fld  f21, 168(%0) \n"                  \
                "fld  f22, 176(%0) \n"                  \
                "fld  f23, 184(%0) \n"                  \
                "fld  f24, 192(%0) \n"                  \
                "fld  f25, 200(%0) \n"                  \
                "fld  f26, 208(%0) \n"                  \
                "fld  f27, 216(%0) \n"                  \
                "fld  f28, 224(%0) \n"                  \
                "fld  f29, 232(%0) \n"                  \
                "fld  f30, 240(%0) \n"                  \
                "fld  f31, 248(%0) \n"                  \
                : /* No outputs. */                      \
                : "r"(&__e[0])                           \
                : "f0", "f1", "f2", "f3", "f4", "f5",   \
                  "f6", "f7", "f8", "f9", "f10", "f11",  \
                  "f12", "f13", "f14", "f15", "f16",     \
                  "f17", "f18", "f19", "f20", "f21",     \
                  "f22", "f23", "f24", "f25", "f26",     \
                  "f27", "f28", "f29", "f30", "f31",     \
                  "memory");                             \
        }                                               \
    } while (0)


#define evl_check_fpregs(__features, __val, __bad)               \
    ({                                                            \
        unsigned int __result = (__val), __i;                     \
        __u64 __e[32];                                             \
                                                                 \
        if (__features & evl_riscv_fpsimd) {                       \
            for (__i = 0; __i < 32; __i++)                         \
                __e[__i] = __val;                                  \
                                                                 \
            __asm__ __volatile__(                                  \
                "fsd f0, 0(%0)   \n"                              \
                "fsd f1, 8(%0)   \n"                              \
                "fsd f2, 16(%0)  \n"                              \
                "fsd f3, 24(%0)  \n"                              \
                "fsd f4, 32(%0)  \n"                              \
                "fsd f5, 40(%0)  \n"                              \
                "fsd f6, 48(%0)  \n"                              \
                "fsd f7, 56(%0)  \n"                              \
                "fsd f8, 64(%0)  \n"                              \
                "fsd f9, 72(%0)  \n"                              \
                "fsd f10, 80(%0) \n"                              \
                "fsd f11, 88(%0) \n"                              \
                "fsd f12, 96(%0) \n"                              \
                "fsd f13, 104(%0)\n"                              \
                "fsd f14, 112(%0)\n"                              \
                "fsd f15, 120(%0)\n"                              \
                "fsd f16, 128(%0)\n"                              \
                "fsd f17, 136(%0)\n"                              \
                "fsd f18, 144(%0)\n"                              \
                "fsd f19, 152(%0)\n"                              \
                "fsd f20, 160(%0)\n"                              \
                "fsd f21, 168(%0)\n"                              \
                "fsd f22, 176(%0)\n"                              \
                "fsd f23, 184(%0)\n"                              \
                "fsd f24, 192(%0)\n"                              \
                "fsd f25, 200(%0)\n"                              \
                "fsd f26, 208(%0)\n"                              \
                "fsd f27, 216(%0)\n"                              \
                "fsd f28, 224(%0)\n"                              \
                "fsd f29, 232(%0)\n"                              \
                "fsd f30, 240(%0)\n"                              \
                "fsd f31, 248(%0)\n"                              \
                : /* No outputs */                                \
                : "r"(&__e[0])                                     \
                : "memory");                                       \
                                                                 \
            for (__i = 0; __i < 32; __i++) {                       \
                if (__e[__i] != __val) {                          \
                    __result = __e[__i];                           \
                    (__bad) = __i;                                 \
                    break;                                         \
                }                                                  \
            }                                                      \
        }                                                          \
        __result;                                                  \
    })


#endif /* !_EVL_RISCV_ASM_UAPI_FPTEST_H */
