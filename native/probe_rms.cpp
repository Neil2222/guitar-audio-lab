// Meet via WASAPI-loopback de RMS/peak op een render-endpoint (substring-arg)
// gedurende N seconden. Draait zelf een stille shared renderer zodat de
// loopback pompt. Gebruik: probe_rms <substring> [seconden]
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <timeapi.h>
#include <cstdio>
#include <cmath>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

static const int SR = 48000;

static WAVEFORMATEXTENSIBLE pcm16(WORD ch) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = ch;
    f.Format.nSamplesPerSec = SR;
    f.Format.wBitsPerSample = 16;
    f.Format.nBlockAlign = (WORD)(ch * 2);
    f.Format.nAvgBytesPerSec = SR * f.Format.nBlockAlign;
    f.Format.cbSize = 22;
    f.Samples.wValidBitsPerSample = 16;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("gebruik: probe_rms <substring> [seconden]\n");
        return 1;
    }
    std::wstring needle;
    for (char* a = argv[1]; *a; a++) needle += (wchar_t)towlower(*a);
    int seconds = argc > 2 ? atoi(argv[2]) : 3;
    timeBeginPeriod(1);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&enu)))
        return 1;
    IMMDeviceCollection* coll = nullptr;
    enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    UINT n = 0;
    coll->GetCount(&n);
    IMMDevice* dev = nullptr;
    for (UINT i = 0; i < n && !dev; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(coll->Item(i, &d))) continue;
        IPropertyStore* st = nullptr;
        std::wstring name;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &st))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(st->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR)
                name = pv.pwszVal;
            PropVariantClear(&pv);
            st->Release();
        }
        std::wstring lower = name;
        for (auto& c : lower) c = towlower(c);
        if (lower.find(needle) != std::wstring::npos) {
            dev = d;
            wprintf(L"endpoint: %s\n", name.c_str());
        } else {
            d->Release();
        }
    }
    coll->Release();
    if (!dev) {
        printf("endpoint niet gevonden\n");
        return 1;
    }
    IAudioClient* lb = nullptr;
    dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&lb);
    WAVEFORMATEX* mix = nullptr;
    lb->GetMixFormat(&mix);
    HRESULT hr = lb->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, mix,
                                nullptr);
    if (FAILED(hr)) {
        printf("loopback-init faalde: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    IAudioCaptureClient* cap = nullptr;
    lb->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);

    IAudioClient* sil = nullptr;
    IAudioRenderClient* silRen = nullptr;
    dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&sil);
    WAVEFORMATEXTENSIBLE sf = pcm16(2);
    sil->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    1000000, 0, (WAVEFORMATEX*)&sf, nullptr);
    UINT32 silBuf = 0;
    sil->GetBufferSize(&silBuf);
    sil->GetService(__uuidof(IAudioRenderClient), (void**)&silRen);
    BYTE* pb = nullptr;
    if (SUCCEEDED(silRen->GetBuffer(silBuf / 2, &pb))) {
        memset(pb, 0, (size_t)(silBuf / 2) * 4);
        silRen->ReleaseBuffer(silBuf / 2, 0);
    }
    sil->Start();
    lb->Start();

    LARGE_INTEGER freq, t0, t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    double sumSq = 0.0;
    long nSamp = 0, frames = 0;
    float peak = 0.0f;
    for (;;) {
        QueryPerformanceCounter(&t);
        if ((t.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart >
            seconds * 1000.0)
            break;
        UINT32 pad = 0;
        if (SUCCEEDED(sil->GetCurrentPadding(&pad)) && pad < silBuf / 2) {
            UINT32 need = silBuf / 2 - pad;
            if (SUCCEEDED(silRen->GetBuffer(need, &pb))) {
                memset(pb, 0, (size_t)need * 4);
                silRen->ReleaseBuffer(need, 0);
            }
        }
        for (;;) {
            BYTE* data = nullptr;
            UINT32 fr = 0;
            DWORD flags = 0;
            HRESULT h = cap->GetBuffer(&data, &fr, &flags, nullptr, nullptr);
            if (h != S_OK || fr == 0) break;
            frames += fr;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                const float* f = (const float*)data;
                for (UINT32 i = 0; i < fr * mix->nChannels; i++) {
                    float a = fabsf(f[i]);
                    if (a > peak) peak = a;
                    sumSq += (double)f[i] * f[i];
                    nSamp++;
                }
            }
            cap->ReleaseBuffer(fr);
        }
        Sleep(5);
    }
    double rms = nSamp ? sqrt(sumSq / nSamp) : 0.0;
    printf("frames=%ld  rms=%.5f  peak=%.5f  (%s)\n", frames, rms, peak,
           peak > 0.005 ? "SIGNAAL" : "stil");
    lb->Stop();
    sil->Stop();
    timeEndPeriod(1);
    return 0;
}
