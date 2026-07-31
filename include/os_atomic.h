/** OS 原子操作抽象层 (R-19: Phase-2 ARM Linux 移植预备).
 *
 *  用法:
 *    #include "os_atomic.h"
 *    volatile gf_atomic_long seq = 0;
 *    gf_atomic_exchange_add(&seq, 2);   // seq += 2 (返回旧值)
 *    gf_atomic_exchange(&seq, 0);       // seq  = 0 (返回旧值)
 *    gf_atomic_decrement(&seq);         // seq -= 1
 *    gf_atomic_increment(&seq);         // seq += 1
 *
 *  当前实现: Windows Interlocked* (x86/x64 全栅栏).
 *  Phase-2 (ARM Linux):   #define GFANC_ATOMIC_C11 → 使用 C11 stdatomic.h.
 *  Phase-3 (MCU bare metal): 平台特定实现 (Cortex-M: ldrex/strex 或关中断).
 */
#ifndef OS_ATOMIC_H
#define OS_ATOMIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── 平台选择 ── */
#if defined(_WIN32) || defined(_WIN64)
  /* Windows: Interlocked API (kernel32.dll) */
  #define GFANC_ATOMIC_WIN32
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  /* C11: stdatomic.h (Linux/ARM/macOS) */
  #define GFANC_ATOMIC_C11
#else
  /* GCC/Clang 内置函数 (回退) */
  #define GFANC_ATOMIC_GCC_BUILTIN
#endif

/* ── 类型 ── */
#ifdef GFANC_ATOMIC_C11
  #include <stdatomic.h>
  typedef atomic_long gf_atomic_long;
#elif defined(GFANC_ATOMIC_WIN32)
  #include <windows.h>
  typedef LONG gf_atomic_long;
#else
  typedef long gf_atomic_long;
#endif

/* ══════════════════════════════════════════════════════════
   原子操作
   ══════════════════════════════════════════════════════════ */

/** 原子加法并返回旧值: *p += v; return old. */
static inline gf_atomic_long gf_atomic_exchange_add(
    volatile gf_atomic_long *p, gf_atomic_long v)
{
#ifdef GFANC_ATOMIC_WIN32
    return InterlockedExchangeAdd(p, v);
#elif defined(GFANC_ATOMIC_C11)
    return atomic_fetch_add(p, v);
#else
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
#endif
}

/** 原子交换并返回旧值: old = *p; *p = v; return old. */
static inline gf_atomic_long gf_atomic_exchange(
    volatile gf_atomic_long *p, gf_atomic_long v)
{
#ifdef GFANC_ATOMIC_WIN32
    return InterlockedExchange(p, v);
#elif defined(GFANC_ATOMIC_C11)
    return atomic_exchange(p, v);
#else
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
#endif
}

/** 原子递减: *p -= 1. */
static inline gf_atomic_long gf_atomic_decrement(
    volatile gf_atomic_long *p)
{
#ifdef GFANC_ATOMIC_WIN32
    return InterlockedDecrement(p);
#elif defined(GFANC_ATOMIC_C11)
    return atomic_fetch_sub(p, 1) - 1;
#else
    return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
#endif
}

/** 原子递增: *p += 1. */
static inline gf_atomic_long gf_atomic_increment(
    volatile gf_atomic_long *p)
{
#ifdef GFANC_ATOMIC_WIN32
    return InterlockedIncrement(p);
#elif defined(GFANC_ATOMIC_C11)
    return atomic_fetch_add(p, 1) + 1;
#else
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* OS_ATOMIC_H */
