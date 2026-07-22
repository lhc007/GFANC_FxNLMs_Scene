/** PortAudio 共享加载层 — 实现. */
#include <stdio.h>
#include "pa_loader.h"

/* ── DLL handle ── */
static HMODULE pa_dll;

/* ── 函数指针定义 ── */
PaError (*p_Pa_Initialize)(void);
PaError (*p_Pa_Terminate)(void);
PaError (*p_Pa_OpenStream)(PaStream **, const void *, const void *,
                            double, unsigned long, unsigned long,
                            void *, void *);
PaError (*p_Pa_StartStream)(PaStream *);
PaError (*p_Pa_StopStream)(PaStream *);
PaError (*p_Pa_CloseStream)(PaStream *);
int    (*p_Pa_GetDeviceCount)(void);
const void *(*p_Pa_GetDeviceInfo)(int);
const void *(*p_Pa_GetHostApiInfo)(int);
int    (*p_Pa_GetDefaultHostApi)(void);
int    (*p_Pa_HostApiTypeIdToHostApiIndex)(int);
const char *(*p_Pa_GetErrorText)(int);

#define PA_LOAD(fn) p_##fn = (void*)GetProcAddress(pa_dll, #fn)

int pa_init(void)
{
    pa_dll = LoadLibraryA("libportaudio64bit-asio.dll");
    if (!pa_dll) { fprintf(stderr, "DLL not found\n"); return -1; }
    PA_LOAD(Pa_Initialize); PA_LOAD(Pa_Terminate);
    PA_LOAD(Pa_OpenStream); PA_LOAD(Pa_StartStream);
    PA_LOAD(Pa_StopStream);  PA_LOAD(Pa_CloseStream);
    PA_LOAD(Pa_GetDeviceCount); PA_LOAD(Pa_GetDeviceInfo);
    PA_LOAD(Pa_GetHostApiInfo); PA_LOAD(Pa_GetDefaultHostApi);
    PA_LOAD(Pa_HostApiTypeIdToHostApiIndex); PA_LOAD(Pa_GetErrorText);
    return 0;
}

void pa_cleanup(void)
{
    if (pa_dll) { FreeLibrary(pa_dll); pa_dll = NULL; }
}
