/** OS 可移植层 — 睡眠 + 时间戳 (R-28: Phase-2/3 移植预备).
 *
 *  用法:
 *    #include "os_port.h"
 *    gf_sleep_ms(100);                // Phase-2 Linux → usleep(100000)
 *    gf_log_timestamp(logfile, tag);  // Phase-3 裸机 → 定义 GFANC_NO_TIME 去除
 *
 *  与 os_atomic.h 互补: 原子操作用 os_atomic, 系统调用用 os_port.
 */
#ifndef OS_PORT_H
#define OS_PORT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 可移植睡眠 ── */
#ifdef _WIN32
  #include <windows.h>
  #define gf_sleep_ms(ms) Sleep((DWORD)(ms))
#else
  #include <unistd.h>
  #define gf_sleep_ms(ms) usleep((ms) * 1000)
#endif

/* ── 可移植日志时间戳 ── */
/* Phase-3 裸机: 编译时 -DGFANC_NO_TIME 去除 time()/ctime() 依赖 */
#ifdef GFANC_NO_TIME
  #define gf_log_timestamp(lf, tag) ((void)0)
#else
  #include <time.h>
  #define gf_log_timestamp(lf, tag) do { \
      time_t _t = time(NULL); \
      if (lf) fprintf(lf, "# GFANC session " tag ": %s", ctime(&_t)); \
    } while(0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* OS_PORT_H */
