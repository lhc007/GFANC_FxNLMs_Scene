/** WASAPI 实时音频 I/O 实现 — Windows Core Audio API.
 *
 * 使用共享模式 WASAPI, 阻塞 I/O (非事件驱动).
 * 参考: Microsoft Core Audio API 文档
 *
 * 编译: 需链接 ole32.lib
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* COM GUID 定义: MinGW 需要手动定义 INITGUID 来生成这些符号 */
#define INITGUID
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#undef INITGUID

/* 链接 Ole32 (MinGW/GCC 不需要, MSVC 需要 pragma) */
#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#endif

typedef struct {
    /* COM */
    IAudioClient          *in_client;
    IAudioClient          *out_client;
    IAudioCaptureClient   *in_capture;
    IAudioRenderClient    *out_render;
    WAVEFORMATEX          *in_fmt;
    WAVEFORMATEX          *out_fmt;

    /* 参数 */
    int fs, in_ch, out_ch;
    int latency_frames;     /* 每通道 latency 帧数 */

    /* 中间缓冲 */
    float *tmp_in;          /* [in_ch  * latency_frames] */
    float *tmp_out;         /* [out_ch * latency_frames] */
    int    tmp_in_filled;   /* tmp_in 中已有帧数 */

    /* 状态 */
    int    started;
} wasapi_t;

/* ── 前向声明 ── */
static void wasapi_close_internal(wasapi_t *w);

/* ── 辅助: LIST 释放 ── */
#define SAFE_RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while(0)

/* ── 获取指定名称/方向的音频设备 ── */
static IMMDevice *get_device(const wchar_t *name, EDataFlow flow)
{
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDeviceCollection  *collection = NULL;
    IMMDevice            *device = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr)) goto done;

    hr = enumerator->lpVtbl->EnumAudioEndpoints(enumerator, flow,
                  DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) goto done;

    UINT count = 0;
    collection->lpVtbl->GetCount(collection, &count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice *dev = NULL;
        IPropertyStore *props = NULL;
        PROPVARIANT var;
        PropVariantInit(&var);

        hr = collection->lpVtbl->Item(collection, i, &dev);
        if (FAILED(hr)) continue;

        hr = dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &props);
        if (FAILED(hr)) { SAFE_RELEASE(dev); continue; }

        hr = props->lpVtbl->GetValue(props, &PKEY_Device_FriendlyName, &var);
        if (SUCCEEDED(hr) && var.pwszVal) {
            int match = (name == NULL || wcsstr(var.pwszVal, name) != NULL);
            if (match) {
                if (device) SAFE_RELEASE(device);
                device = dev; /* 不 Release, 调用者负责 */
                PropVariantClear(&var);
                SAFE_RELEASE(props);
                break; /* 取第一个匹配 */
            }
        }
        PropVariantClear(&var);
        SAFE_RELEASE(props);
        if (!device) SAFE_RELEASE(dev);
    }

done:
    SAFE_RELEASE(collection);
    SAFE_RELEASE(enumerator);
    if (!device && name) {
        wprintf(L"WASAPI: device '%s' not found, using default\n", name);
        /* fallback: get default device */
        if (enumerator == NULL)
            CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                            &IID_IMMDeviceEnumerator, (void **)&enumerator);
        if (enumerator)
            enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, flow,
                           eConsole, &device);
        SAFE_RELEASE(enumerator);
    }
    return device;
}

/* ── 初始化 AudioClient ── */
static IAudioClient *init_audio_client(IMMDevice *dev, int fs, int nch, int latency_frames,
                                        WAVEFORMATEX **out_fmt, int is_input)
{
    IAudioClient *client = NULL;
    WAVEFORMATEX *mix_fmt = NULL;
    WAVEFORMATEX  target;
    HRESULT hr;

    hr = dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_ALL,
                               NULL, (void **)&client);
    if (FAILED(hr)) return NULL;

    /* 获取设备原生格式 */
    hr = client->lpVtbl->GetMixFormat(client, &mix_fmt);
    if (FAILED(hr)) { SAFE_RELEASE(client); return NULL; }

    /* 构造目标格式 */
    memset(&target, 0, sizeof(target));
    target.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT; /* float32 */
    target.nChannels       = (WORD)nch;
    target.nSamplesPerSec  = (DWORD)fs;
    target.wBitsPerSample  = 32;
    target.nBlockAlign     = (WORD)(nch * 4);
    target.nAvgBytesPerSec = (DWORD)(fs * nch * 4);
    target.cbSize          = 0;

    /* 初始化: 共享模式, 低延迟 */
    DWORD flags = 0;
    REFERENCE_TIME latency_100ns = (REFERENCE_TIME)latency_frames * 10000000LL / fs;

    hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED, flags,
                                     latency_100ns, 0, &target, NULL);

    CoTaskMemFree(mix_fmt);

    if (FAILED(hr)) {
        /* 回退: 尝试更大的 latency */
        latency_100ns = (REFERENCE_TIME)(latency_frames * 2) * 10000000LL / fs;
        hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0,
                                         latency_100ns, 0, &target, NULL);
    }

    if (FAILED(hr)) {
        SAFE_RELEASE(client);
        return NULL;
    }

    /* 获取实际格式 */
    if (out_fmt) {
        *out_fmt = (WAVEFORMATEX *)malloc(sizeof(WAVEFORMATEX) + target.cbSize);
        memcpy(*out_fmt, &target, sizeof(WAVEFORMATEX));
    }

    return client;
}

/* ══════════════════════════════════════════════════════════
   公开 API
   ══════════════════════════════════════════════════════════ */

wasapi_t *wasapi_open(const wchar_t *in_name, const wchar_t *out_name,
                       int fs, int in_ch, int out_ch, int latency_ms)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "WASAPI: CoInitializeEx failed 0x%lx\n", (unsigned long)hr);
        return NULL;
    }

    wasapi_t *w = (wasapi_t *)calloc(1, sizeof(wasapi_t));
    w->fs = fs; w->in_ch = in_ch; w->out_ch = out_ch;
    w->latency_frames = (int)((long long)latency_ms * fs / 1000);

    /* 获取设备 */
    IMMDevice *in_dev  = get_device(in_name, eCapture);
    IMMDevice *out_dev = get_device(out_name, eRender);

    if (!in_dev || !out_dev) {
        fprintf(stderr, "WASAPI: device not found\n");
        SAFE_RELEASE(in_dev); SAFE_RELEASE(out_dev);
        free(w); return NULL;
    }

    /* 初始化 AudioClient */
    w->in_client = init_audio_client(in_dev, fs, in_ch, w->latency_frames,
                                      &w->in_fmt, 1);
    w->out_client = init_audio_client(out_dev, fs, out_ch, w->latency_frames,
                                       &w->out_fmt, 0);
    SAFE_RELEASE(in_dev);
    SAFE_RELEASE(out_dev);

    if (!w->in_client || !w->out_client) {
        fprintf(stderr, "WASAPI: AudioClient init failed\n");
        wasapi_close_internal(w); return NULL;
    }

    /* 获取 buffer 接口 */
    hr = w->in_client->lpVtbl->GetService(w->in_client,
            &IID_IAudioCaptureClient, (void **)&w->in_capture);
    if (FAILED(hr)) { wasapi_close_internal(w); return NULL; }

    hr = w->out_client->lpVtbl->GetService(w->out_client,
            &IID_IAudioRenderClient, (void **)&w->out_render);
    if (FAILED(hr)) { wasapi_close_internal(w); return NULL; }

    /* 分配中间缓冲 */
    w->tmp_in  = (float *)calloc(in_ch  * w->latency_frames, sizeof(float));
    w->tmp_out = (float *)calloc(out_ch * w->latency_frames, sizeof(float));
    w->tmp_in_filled = 0;

    printf("WASAPI: opened %dHz in=%dch out=%dch latency=%dms\n",
           fs, in_ch, out_ch, latency_ms);
    return w;
}

int wasapi_start(wasapi_t *w)
{
    if (!w || w->started) return -1;
    HRESULT hr;

    /* 预填充 render buffer (静音), Start 后才开始播放 */
    {
        UINT32 padding = 0;
        w->out_client->lpVtbl->GetCurrentPadding(w->out_client, &padding);
        UINT32 to_fill = w->latency_frames - padding;
        BYTE *data;
        hr = w->out_render->lpVtbl->GetBuffer(w->out_render, to_fill, &data);
        if (SUCCEEDED(hr)) {
            memset(data, 0, to_fill * w->out_ch * 4);
            w->out_render->lpVtbl->ReleaseBuffer(w->out_render, to_fill, 0);
        }
    }

    hr = w->in_client->lpVtbl->Start(w->in_client);
    if (FAILED(hr)) return -1;
    hr = w->out_client->lpVtbl->Start(w->out_client);
    if (FAILED(hr)) { w->in_client->lpVtbl->Stop(w->in_client); return -1; }

    w->started = 1;
    printf("WASAPI: streaming started\n");
    return 0;
}

int wasapi_stop(wasapi_t *w)
{
    if (!w || !w->started) return -1;
    w->in_client->lpVtbl->Stop(w->in_client);
    w->out_client->lpVtbl->Stop(w->out_client);
    w->started = 0;
    printf("WASAPI: streaming stopped\n");
    return 0;
}

int wasapi_read_capture(wasapi_t *w, float *buf, int frames)
{
    if (!w || !w->started) return -1;

    int copied = 0;
    int need = frames * w->in_ch;

    /* 先输出 tmp_in 中缓存的数据 */
    if (w->tmp_in_filled > 0) {
        int avail = w->tmp_in_filled * w->in_ch;
        int take = (need < avail) ? need : avail;
        memcpy(buf, w->tmp_in, take * sizeof(float));
        if (take < avail)
            memmove(w->tmp_in, w->tmp_in + take, (avail - take) * sizeof(float));
        w->tmp_in_filled -= take / w->in_ch;
        copied += take;
        if (copied >= need) return frames;
    }

    while (copied < need) {
        UINT32 pkt_frames = 0;
        BYTE *data = NULL;
        DWORD flags;
        HRESULT hr;

        hr = w->in_capture->lpVtbl->GetBuffer(w->in_capture, &data,
                                               &pkt_frames, &flags, NULL, NULL);
        if (FAILED(hr) || pkt_frames == 0) {
            /* 没有数据, 等待一小段时间 */
            Sleep(1);
            continue;
        }

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            /* 静音: 填零 */
            memset(data, 0, pkt_frames * w->in_ch * 4);
        }

        /* 拷贝到输出 */
        int pkt_samples = (int)pkt_frames * w->in_ch;
        int remaining = need - copied;
        if (pkt_samples <= remaining) {
            memcpy(buf + copied, data, pkt_samples * sizeof(float));
            copied += pkt_samples;
        } else {
            /* 分包: 部分直接输出, 剩余存入 tmp_in */
            memcpy(buf + copied, data, remaining * sizeof(float));
            int leftover = pkt_samples - remaining;
            if (leftover <= w->latency_frames * w->in_ch) {
                memcpy(w->tmp_in, (float *)data + remaining,
                       leftover * sizeof(float));
                w->tmp_in_filled = leftover / w->in_ch;
            }
            copied += remaining;
        }

        w->in_capture->lpVtbl->ReleaseBuffer(w->in_capture, pkt_frames);
    }

    return frames;
}

int wasapi_write_render(wasapi_t *w, const float *buf, int frames)
{
    if (!w || !w->started) return -1;

    int written = 0;
    int total = frames * w->out_ch;

    while (written < total) {
        UINT32 padding = 0;
        w->out_client->lpVtbl->GetCurrentPadding(w->out_client, &padding);
        UINT32 avail = (UINT32)w->latency_frames - padding;
        if (avail == 0) {
            Sleep(1);
            continue;
        }

        int to_write_samples = total - written;
        UINT32 to_write_frames = (UINT32)(to_write_samples / w->out_ch);
        if (to_write_frames > avail) to_write_frames = avail;

        BYTE *data;
        HRESULT hr = w->out_render->lpVtbl->GetBuffer(w->out_render,
                                                       to_write_frames, &data);
        if (FAILED(hr)) { Sleep(1); continue; }

        int samples = (int)to_write_frames * w->out_ch;
        memcpy(data, buf + written, samples * sizeof(float));
        w->out_render->lpVtbl->ReleaseBuffer(w->out_render, to_write_frames, 0);
        written += samples;
    }

    return frames;
}

int wasapi_capture_available(wasapi_t *w)
{
    if (!w) return 0;
    UINT32 padding = 0;
    w->in_client->lpVtbl->GetCurrentPadding(w->in_client, &padding);
    return (int)padding + w->tmp_in_filled;
}

int wasapi_render_available(wasapi_t *w)
{
    if (!w) return 0;
    UINT32 padding = 0;
    w->out_client->lpVtbl->GetCurrentPadding(w->out_client, &padding);
    return w->latency_frames - (int)padding;
}

static void wasapi_close_internal(wasapi_t *w)
{
    if (!w) return;
    if (w->started) wasapi_stop(w);
    SAFE_RELEASE(w->in_capture);
    SAFE_RELEASE(w->out_render);
    SAFE_RELEASE(w->in_client);
    SAFE_RELEASE(w->out_client);
    free(w->in_fmt);
    free(w->out_fmt);
    free(w->tmp_in);
    free(w->tmp_out);
    free(w);
}

void wasapi_close(wasapi_t *w)
{
    wasapi_close_internal(w);
}

void wasapi_list_devices(void)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) return;

    IMMDeviceEnumerator *e = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&e);
    if (FAILED(hr)) return;

    const wchar_t *dirs[] = { L"Capture", L"Render" };
    EDataFlow flows[] = { eCapture, eRender };

    for (int d = 0; d < 2; d++) {
        wprintf(L"\n=== %s Devices ===\n", dirs[d]);
        IMMDeviceCollection *col = NULL;
        e->lpVtbl->EnumAudioEndpoints(e, flows[d], DEVICE_STATE_ACTIVE, &col);
        if (!col) continue;

        UINT count;
        col->lpVtbl->GetCount(col, &count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice *dev = NULL;
            IPropertyStore *props = NULL;
            PROPVARIANT var;
            PropVariantInit(&var);

            col->lpVtbl->Item(col, i, &dev);
            dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &props);
            props->lpVtbl->GetValue(props, &PKEY_Device_FriendlyName, &var);
            if (var.pwszVal)
                wprintf(L"  %2u: %s\n", i, var.pwszVal);
            PropVariantClear(&var);
            SAFE_RELEASE(props);
            SAFE_RELEASE(dev);
        }
        SAFE_RELEASE(col);
    }
    SAFE_RELEASE(e);
}
