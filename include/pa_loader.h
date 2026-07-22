/** PortAudio 共享加载层 — 运行时 DLL 绑定.
 *
 * main_realtime.c 和 calibrate_feedback.c 共用,
 * 避免 ~45 行 PA 样板重复.
 */
#ifndef PA_LOADER_H
#define PA_LOADER_H

#include <windows.h>

/* ── 类型 ── */
typedef int PaError;
typedef void PaStream;
#define paFloat32 0x00000001
#define paNoFlag  0
#define paNoError 0

/* ── 函数指针 ── */
extern PaError (*p_Pa_Initialize)(void);
extern PaError (*p_Pa_Terminate)(void);
extern PaError (*p_Pa_OpenStream)(PaStream **, const void *, const void *,
                                  double, unsigned long, unsigned long,
                                  void *, void *);
extern PaError (*p_Pa_StartStream)(PaStream *);
extern PaError (*p_Pa_StopStream)(PaStream *);
extern PaError (*p_Pa_CloseStream)(PaStream *);
extern int    (*p_Pa_GetDeviceCount)(void);
extern const void *(*p_Pa_GetDeviceInfo)(int);
extern const void *(*p_Pa_GetHostApiInfo)(int);
extern int    (*p_Pa_GetDefaultHostApi)(void);
extern int    (*p_Pa_HostApiTypeIdToHostApiIndex)(int);
extern const char *(*p_Pa_GetErrorText)(int);

/* ── 辅助结构 ── */
typedef struct {
    int    device, channelCount, sampleFormat;
    double suggestedLatency;
    void  *hostApiSpecificStreamInfo;
} PaStreamParams;

typedef struct {
    double inputBufferAdcTime, currentTime, outputBufferDacTime;
} PaCbTimeInfo;

typedef struct {
    int         structVersion;
    const char *name;
    int         type, deviceCount, defaultInputDevice, defaultOutputDevice;
} PaHostApiInfo2;

typedef struct {
    int         structVersion;
    const char *name;
    int         hostApi, maxInputChannels, maxOutputChannels;
    double      defLowInLat, defLowOutLat, defHighInLat, defHighOutLat;
    double      defaultSampleRate;
} PaDeviceInfo2;

/* ── API ── */
int  pa_init(void);
void pa_cleanup(void);

#endif
