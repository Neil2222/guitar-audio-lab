// Low-latency passthrough v2: Rocksmith (exclusive, event, 3 ms) ->
// Realtek S/PDIF (exclusive, POLLING met kleine padding).
//
// Kern-inzicht (zie probe_padding.cpp): de Realtek-driver signaleert events in
// 10 ms-batches, maar de DMA-positie loopt per frame door. Door de renderclient
// in polling mode te gebruiken en na elk capture-blok direct bij te vullen,
// is de output-latency ~= gemiddelde padding i.p.v. 2x10 ms event-buffering.
//
// Eén realtime-thread, geen ringbuffer: capture-event is de klok.
// Driftcorrectie: padding > high -> samples droppen; padding < low -> opvullen.
//
// Build: zie build.ps1 (cl ... passthrough_ll.cpp /link ole32.lib avrt.lib winmm.lib)
// Gebruik: passthrough_ll.exe [--seconds N] [--target FRAMES] [--gain G]

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")

static const int SR = 48000;

#define CHECK(hr, msg)                                                        \
    do {                                                                      \
        if (FAILED(hr)) {                                                     \
            fprintf(stderr, "FOUT %s: hr=0x%08lx\n", msg, (unsigned long)hr); \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static IMMDevice* findDevice(IMMDeviceEnumerator* enu, EDataFlow flow,
                             const wchar_t* needle, std::wstring* outName) {
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(enu->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll)))
        return nullptr;
    UINT n = 0;
    coll->GetCount(&n);
    IMMDevice* found = nullptr;
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev))) continue;
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR) {
                std::wstring name = pv.pwszVal, lower = pv.pwszVal;
                for (auto& c : lower) c = towlower(c);
                if (lower.find(needle) != std::wstring::npos) {
                    found = dev;
                    dev->AddRef();
                    if (outName) *outName = name;
                }
            }
            PropVariantClear(&pv);
            store->Release();
        }
        dev->Release();
    }
    coll->Release();
    return found;
}

static WAVEFORMATEXTENSIBLE pcm16(int channels) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = (WORD)channels;
    f.Format.nSamplesPerSec = SR;
    f.Format.wBitsPerSample = 16;
    f.Format.nBlockAlign = (WORD)(channels * 2);
    f.Format.nAvgBytesPerSec = SR * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 16;
    f.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                                    : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

int wmain(int argc, wchar_t** argv) {
    int seconds = 20, target = 288;  // 288 frames = 6 ms doel-padding
    float gain = 1.0f;
    for (int i = 1; i < argc - 1; i++) {
        if (!wcscmp(argv[i], L"--seconds")) seconds = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--target")) target = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--gain")) gain = (float)_wtof(argv[i + 1]);
    }

    timeBeginPeriod(1);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CHECK(hr, "CoInitializeEx");
    IMMDeviceEnumerator* enu = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enu);
    CHECK(hr, "MMDeviceEnumerator");

    std::wstring inName, outName;
    IMMDevice* inDev = findDevice(enu, eCapture, L"rocksmith", &inName);
    IMMDevice* outDev = findDevice(enu, eRender, L"realtek digital", &outName);
    if (!inDev || !outDev) {
        fprintf(stderr, "FOUT: device niet gevonden\n");
        return 1;
    }
    wprintf(L"input : %s\noutput: %s\n", inName.c_str(), outName.c_str());

    // --- capture: exclusive, event-driven, minimale periode ---
    IAudioClient* capClient = nullptr;
    hr = inDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                         (void**)&capClient);
    CHECK(hr, "Activate capture");
    REFERENCE_TIME defP = 0, minP = 0;
    capClient->GetDevicePeriod(&defP, &minP);
    WAVEFORMATEXTENSIBLE inFmt = pcm16(1);
    hr = capClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, minP,
                               (WAVEFORMATEX*)&inFmt, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 fr = 0;
        capClient->GetBufferSize(&fr);
        capClient->Release();
        REFERENCE_TIME p = (REFERENCE_TIME)((10000000.0 * fr) / SR + 0.5);
        hr = inDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&capClient);
        CHECK(hr, "re-Activate capture");
        hr = capClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p, p,
                                   (WAVEFORMATEX*)&inFmt, nullptr);
    }
    CHECK(hr, "Initialize capture exclusive");
    UINT32 capFrames = 0;
    capClient->GetBufferSize(&capFrames);
    HANDLE capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    capClient->SetEventHandle(capEvent);
    IAudioCaptureClient* cap = nullptr;
    hr = capClient->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
    CHECK(hr, "GetService capture");

    // --- render: exclusive, POLLING (geen event), ruime buffer, kleine padding ---
    IAudioClient* renClient = nullptr;
    hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          (void**)&renClient);
    CHECK(hr, "Activate render");
    WAVEFORMATEXTENSIBLE outFmt = pcm16(2);
    hr = renClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 200000, 0,
                               (WAVEFORMATEX*)&outFmt, nullptr);  // 20 ms buffer
    CHECK(hr, "Initialize render polling exclusive");
    UINT32 renFrames = 0;
    renClient->GetBufferSize(&renFrames);
    IAudioRenderClient* ren = nullptr;
    hr = renClient->GetService(__uuidof(IAudioRenderClient), (void**)&ren);
    CHECK(hr, "GetService render");

    printf("capture: %u frames/periode (%.2f ms), render-buffer %u frames, "
           "padding-doel %d frames (%.2f ms)\n",
           capFrames, capFrames * 1000.0 / SR, renFrames, target,
           target * 1000.0 / SR);

    const UINT32 low = (UINT32)(target > 96 ? target - 96 : 0);   // -2 ms
    const UINT32 high = (UINT32)(target + 96);                    // +2 ms

    // prime: doel-padding aan stilte klaarzetten
    BYTE* pb = nullptr;
    hr = ren->GetBuffer(target, &pb);
    CHECK(hr, "prime GetBuffer");
    memset(pb, 0, (size_t)target * 4);
    ren->ReleaseBuffer(target, 0);

    // stats
    long wakeups = 0, underruns = 0, dropFrames = 0, padFrames = 0,
         capErrors = 0, renErrors = 0, timeouts = 0, lateWakeups = 0;
    double padSum = 0.0;
    long padCount = 0;
    UINT32 padMin = 0xFFFFFFFF, padMax = 0;
    double maxIntervalMs = 0.0;
    float peak = 0.0f;

    DWORD mmIdx = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIdx);

    std::vector<float> fbuf(capFrames * 2);
    renClient->Start();
    capClient->Start();

    LARGE_INTEGER freq, last{}, now, tEnd, t0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    tEnd.QuadPart = t0.QuadPart + freq.QuadPart * seconds;
    const double expectMs = capFrames * 1000.0 / SR;
    const long warmupWakeups = SR / (long)capFrames;  // ~1 s

    for (;;) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= tEnd.QuadPart) break;
        DWORD w = WaitForSingleObject(capEvent, 500);
        if (w != WAIT_OBJECT_0) { timeouts++; continue; }
        wakeups++;
        bool warm = wakeups > warmupWakeups;
        QueryPerformanceCounter(&now);
        if (last.QuadPart && warm) {
            double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
            if (ms > maxIntervalMs) maxIntervalMs = ms;
            if (ms > 1.5 * expectMs) lateWakeups++;
        }
        last = now;

        // capture-blok ophalen (kan >1 packet zijn na jitter)
        UINT32 total = 0;
        for (;;) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            HRESULT h = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (h == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(h)) { capErrors++; break; }
            if ((total + frames) * 2 > fbuf.size()) fbuf.resize((total + frames) * 2);
            const int16_t* s = (const int16_t*)data;
            for (UINT32 i = 0; i < frames; i++) {
                float v = s[i] / 32768.0f * gain;
                float a = fabsf(v);
                if (a > peak) peak = a;
                fbuf[total + i] = v;
            }
            cap->ReleaseBuffer(frames);
            total += frames;
            if (frames < capFrames) break;
        }
        if (total == 0) continue;

        UINT32 padding = 0;
        if (FAILED(renClient->GetCurrentPadding(&padding))) { renErrors++; continue; }
        if (warm) {
            padSum += padding;
            padCount++;
            if (padding < padMin) padMin = padding;
            if (padding > padMax) padMax = padding;
            if (padding == 0) underruns++;
        }

        // driftcorrectie
        UINT32 writeOfs = 0, writeN = total;
        if (padding > high && writeN > 0) {  // te vol: oudste deel blok droppen
            UINT32 drop = padding - target;
            if (drop > writeN) drop = writeN;
            writeOfs = drop;
            writeN -= drop;
            if (warm) dropFrames += (long)drop;
        }
        UINT32 pad2 = padding + writeN;
        UINT32 fill = 0;
        if (pad2 < low) {  // te leeg: aanvullen met herhaald laatste sample
            fill = low - pad2;
            if (warm) padFrames += (long)fill;
        }
        UINT32 n = writeN + fill;
        if (n > renFrames - padding) n = renFrames - padding;  // clamp
        if (n == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(ren->GetBuffer(n, &out))) { renErrors++; continue; }
        int16_t* o = (int16_t*)out;
        float lastSample = writeN ? fbuf[writeOfs + writeN - 1] : 0.0f;
        for (UINT32 i = 0; i < n; i++) {
            float v = i < writeN ? fbuf[writeOfs + i] : lastSample;
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            int16_t q = (int16_t)lrintf(v * 32767.0f);
            o[i * 2] = q;
            o[i * 2 + 1] = q;
        }
        ren->ReleaseBuffer(n, 0);
    }

    capClient->Stop();
    renClient->Stop();
    if (mm) AvRevertMmThreadCharacteristics(mm);
    timeEndPeriod(1);

    double padAvg = padCount ? padSum / padCount : 0.0;
    double inMs = capFrames * 1000.0 / SR;
    double outMs = padAvg * 1000.0 / SR;
    printf("\n=== RESULTAAT (na 1 s warmup) ===\n");
    printf("latency  input=%.2f ms  output-padding avg=%.2f ms "
           "(min=%u max=%u frames)\n", inMs, outMs, padMin, padMax);
    printf("geschat totaal software: %.2f ms\n", inMs + outMs);
    printf("wakeups=%ld  underruns=%ld  driftdrops=%ld frames  "
           "opvulling=%ld frames\n", wakeups, underruns, dropFrames, padFrames);
    printf("errors cap/ren=%ld/%ld  timeouts=%ld  wakeup max=%.2f ms  "
           "laat(>1.5x)=%ld\n", capErrors, renErrors, timeouts, maxIntervalMs,
           lateWakeups);
    printf("signaal peak=%.1f dBFS\n",
           peak > 0 ? 20.0 * log10(peak) : -999.0);
    return 0;
}
