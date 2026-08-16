// Neemt N seconden op van een capture-endpoint (substring) in shared mode
// en print RMS/peak — "kamer-microfoon als meetinstrument".
// Gebruik: probe_mic list          -> toon alle capture-endpoints
//          probe_mic <substr> [s]  -> opnemen en meten
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

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("gebruik: probe_mic list | probe_mic <substr> [sec]\n");
        return 1;
    }
    std::wstring needle;
    for (char* a = argv[1]; *a; a++) needle += (wchar_t)towlower(*a);
    int seconds = argc > 2 ? atoi(argv[2]) : 4;
    bool listOnly = needle == L"list";
    timeBeginPeriod(1);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&enu);
    IMMDeviceCollection* coll = nullptr;
    enu->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
    UINT n = 0;
    coll->GetCount(&n);
    IMMDevice* dev = nullptr;
    for (UINT i = 0; i < n; i++) {
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
        if (listOnly) wprintf(L"  [%u] %s\n", i, name.c_str());
        std::wstring lower = name;
        for (auto& c : lower) c = towlower(c);
        if (!listOnly && !dev &&
            lower.find(needle) != std::wstring::npos) {
            dev = d;
            wprintf(L"mic: %s\n", name.c_str());
            continue;
        }
        d->Release();
    }
    coll->Release();
    if (listOnly) return 0;
    if (!dev) {
        printf("mic niet gevonden\n");
        return 1;
    }
    IAudioClient* c = nullptr;
    dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c);
    WAVEFORMATEX* mix = nullptr;
    c->GetMixFormat(&mix);
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix,
                               nullptr);
    if (FAILED(hr)) {
        printf("init faalde: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    IAudioCaptureClient* cap = nullptr;
    c->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
    c->Start();
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
        Sleep(10);
    }
    double rms = nSamp ? sqrt(sumSq / nSamp) : 0.0;
    printf("frames=%ld  rms=%.5f  peak=%.5f  (%s)\n", frames, rms, peak,
           peak > 0.01 ? "GELUID IN DE KAMER" : "stil");
    c->Stop();
    timeEndPeriod(1);
    return 0;
}
