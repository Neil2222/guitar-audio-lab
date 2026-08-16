// Probe: WASAPI SHARED render op de Realtek S/PDIF — padding-granulariteit,
// minimale stabiele padding-target en daarmee de realistische gitaarlatency
// als de engine-output naar shared zou gaan. Simuleert de engine-cadans:
// elke 3 ms een blok schrijven (polling, geen events), zoals guitar_amp_gui.
// Build: cl /nologo /O2 /EHsc /std:c++17 probe_shared_render.cpp /link ole32.lib winmm.lib avrt.lib
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <timeapi.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <map>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")

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
    f.dwChannelMask = ch == 1 ? SPEAKER_FRONT_CENTER
                              : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

static IMMDevice* findDevice(IMMDeviceEnumerator* enu, const wchar_t* needle) {
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)))
        return nullptr;
    UINT n = 0;
    coll->GetCount(&n);
    IMMDevice* found = nullptr;
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev))) continue;
        IPropertyStore* st = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &st))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(st->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR) {
                std::wstring lower = pv.pwszVal;
                for (auto& c : lower) c = towlower(c);
                if (lower.find(needle) != std::wstring::npos) {
                    wprintf(L"device: %s\n", pv.pwszVal);
                    found = dev;
                }
            }
            PropVariantClear(&pv);
            st->Release();
        }
        if (!found) dev->Release();
    }
    coll->Release();
    return found;
}

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&enu)))
        return 1;
    IMMDevice* dev = findDevice(enu, L"realtek digital");
    if (!dev) {
        printf("Realtek Digital niet gevonden\n");
        return 1;
    }
    IAudioClient* c = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&c)))
        return 1;
    WAVEFORMATEX* mix = nullptr;
    c->GetMixFormat(&mix);
    printf("mix-format: %lu Hz, %u ch, %u bit (tag %u)\n", mix->nSamplesPerSec,
           mix->nChannels, mix->wBitsPerSample, mix->wFormatTag);
    REFERENCE_TIME defP = 0, minP = 0;
    c->GetDevicePeriod(&defP, &minP);
    printf("device-period: default=%.2f ms, min=%.2f ms\n", defP / 10000.0,
           minP / 10000.0);

    WAVEFORMATEXTENSIBLE fmt = pcm16(2);
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                               1000000 /* 100 ms */, 0, (WAVEFORMATEX*)&fmt,
                               nullptr);
    printf("Initialize shared (pcm16 2ch 48k, autoconvert): 0x%08lx\n",
           (unsigned long)hr);
    if (FAILED(hr)) return 1;
    UINT32 bufFrames = 0;
    c->GetBufferSize(&bufFrames);
    REFERENCE_TIME strLat = 0;
    c->GetStreamLatency(&strLat);
    printf("buffer=%u frames (%.1f ms), GetStreamLatency=%.2f ms\n", bufFrames,
           bufFrames * 1000.0 / SR, strLat / 10000.0);
    IAudioRenderClient* ren = nullptr;
    if (FAILED(c->GetService(__uuidof(IAudioRenderClient), (void**)&ren)))
        return 1;

    DWORD mmIdx = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIdx);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    // --- Fase A: padding-granulariteit ---
    // Vooraf 50 ms vullen, dan 300 ms lang strak samplen hoe padding zakt.
    {
        BYTE* pb = nullptr;
        ren->GetBuffer(2400, &pb);
        memset(pb, 0, 2400 * 4);
        ren->ReleaseBuffer(2400, 0);
        c->Start();
        LARGE_INTEGER t0, t;
        QueryPerformanceCounter(&t0);
        UINT32 prev = 0;
        c->GetCurrentPadding(&prev);
        std::map<int, int> stepHist;  // afname-grootte -> aantal
        int transitions = 0;
        double firstMs = -1, lastMs = 0;
        for (;;) {
            QueryPerformanceCounter(&t);
            double ms = (t.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
            if (ms > 300.0) break;
            UINT32 pad = 0;
            c->GetCurrentPadding(&pad);
            if (pad != prev) {
                int step = (int)prev - (int)pad;
                stepHist[step]++;
                transitions++;
                if (firstMs < 0) firstMs = ms;
                lastMs = ms;
                prev = pad;
            }
        }
        c->Stop();
        c->Reset();
        printf("\n[A] padding-granulariteit (300 ms, %d overgangen):\n",
               transitions);
        for (auto& kv : stepHist)
            printf("    afname %5d frames (%.2f ms): %d keer\n", kv.first,
                   kv.first * 1000.0 / SR, kv.second);
        if (transitions > 1)
            printf("    gemiddeld interval tussen overgangen: %.2f ms\n",
                   (lastMs - firstMs) / (transitions - 1));
    }

    // --- Fase B: minimale stabiele padding-target met 3 ms-cadans ---
    // Per target: primen, dan 10 s lang elke 3 ms bijvullen tot target.
    // (Engine schrijft 144 frames per capture-event; bijvullen-tot-target is
    // equivalent incl. driftcorrectie.)
    UINT32 targets[] = {288, 480, 624, 768, 960, 1440};
    int seconds = (argc > 1) ? atoi(argv[1]) : 10;
    printf("\n[B] stabiliteit per padding-target (%d s per target, 3 ms-cadans):\n",
           seconds);
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    for (UINT32 target : targets) {
        BYTE* pb = nullptr;
        if (SUCCEEDED(ren->GetBuffer(target, &pb))) {
            memset(pb, 0, (size_t)target * 4);
            ren->ReleaseBuffer(target, 0);
        }
        c->Start();
        LARGE_INTEGER due;
        due.QuadPart = -30000;  // 3 ms
        SetWaitableTimer(timer, &due, 3, nullptr, nullptr, FALSE);
        UINT32 minPad = UINT32_MAX, maxPad = 0;
        uint64_t padSum = 0;
        long ticks = 0, zeros = 0, low = 0;
        LARGE_INTEGER t0, t;
        QueryPerformanceCounter(&t0);
        for (;;) {
            WaitForSingleObject(timer, 100);
            QueryPerformanceCounter(&t);
            double ms = (t.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
            if (ms > seconds * 1000.0) break;
            UINT32 pad = 0;
            c->GetCurrentPadding(&pad);
            ticks++;
            padSum += pad;
            if (pad < minPad) minPad = pad;
            if (pad > maxPad) maxPad = pad;
            if (pad == 0) zeros++;
            if (pad < 48) low++;  // < 1 ms over
            if (pad < target) {
                UINT32 need = target - pad;
                if (SUCCEEDED(ren->GetBuffer(need, &pb))) {
                    memset(pb, 0, (size_t)need * 4);
                    ren->ReleaseBuffer(need, 0);
                }
            }
        }
        CancelWaitableTimer(timer);
        c->Stop();
        c->Reset();
        printf("  target %4u (%5.1f ms): pad min=%4u avg=%6.1f max=%4u | "
               "pad==0: %ld keer, pad<48: %ld (van %ld ticks)\n",
               target, target * 1000.0 / SR, minPad,
               ticks ? (double)padSum / ticks : 0.0, maxPad, zeros, low,
               ticks);
    }

    if (mm) AvRevertMmThreadCharacteristics(mm);
    ren->Release();
    c->Release();
    dev->Release();
    enu->Release();
    CoTaskMemFree(mix);
    timeEndPeriod(1);
    return 0;
}
