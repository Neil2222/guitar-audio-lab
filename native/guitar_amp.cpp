// Guitar Audio Lab — Fase 4-prototype: Rocksmith -> NAM amp-model -> S/PDIF.
//
// Audio-architectuur = passthrough_ll.cpp (capture-event als klok, polling
// render, ~4,8 ms). Daartussen: Neural Amp Modeler (NeuralAmpModelerCore,
// derde partij, MIT) met per-blok DSP-tijdmeting.
//
// Build: zie build.ps1. Gebruik:
//   guitar_amp.exe [--model pad.nam] [--seconds N] [--gain G] [--master M]
//                  [--target FRAMES]

#include <windows.h>
#include <timeapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/activations.h"

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
    int seconds = 60, target = 288;
    float gain = 1.0f, master = 1.0f;
    std::wstring modelPath =
        L"C:\\Users\\Neilt\\github\\guitar-audio-lab\\third_party\\"
        L"NeuralAmpModelerCore\\example_models\\wavenet_a1_standard.nam";
    for (int i = 1; i < argc - 1; i++) {
        if (!wcscmp(argv[i], L"--seconds")) seconds = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--target")) target = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--gain")) gain = (float)_wtof(argv[i + 1]);
        if (!wcscmp(argv[i], L"--master")) master = (float)_wtof(argv[i + 1]);
        if (!wcscmp(argv[i], L"--model")) modelPath = argv[i + 1];
    }

    // --- NAM-model laden (vóór alle audio-setup) ---
    nam::activations::Activation::enable_fast_tanh();
    std::unique_ptr<nam::DSP> model;
    try {
        model = nam::get_dsp(std::filesystem::path(modelPath));
    } catch (const std::exception& e) {
        fprintf(stderr, "FOUT: model laden mislukt: %s\n", e.what());
        return 1;
    }
    if (!model) {
        fprintf(stderr, "FOUT: model laden mislukt\n");
        return 1;
    }
    if (model->NumInputChannels() != 1 || model->NumOutputChannels() != 1) {
        fprintf(stderr, "FOUT: verwacht mono model (in=%d out=%d)\n",
                model->NumInputChannels(), model->NumOutputChannels());
        return 1;
    }
    double expSR = model->GetExpectedSampleRate();
    wprintf(L"model : %s\n", modelPath.c_str());
    printf("model verwacht %.0f Hz%s\n", expSR,
           (expSR > 0 && (int)expSR != SR)
               ? "  !! wijkt af van 48000 — klinkt mogelijk anders" : "");
    const int MAXBLOCK = 2048;
    model->Reset(SR, MAXBLOCK);  // doet standaard ook prewarm

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

    // --- render: exclusive, polling, kleine padding ---
    IAudioClient* renClient = nullptr;
    hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          (void**)&renClient);
    CHECK(hr, "Activate render");
    WAVEFORMATEXTENSIBLE outFmt = pcm16(2);
    hr = renClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 200000, 0,
                               (WAVEFORMATEX*)&outFmt, nullptr);
    CHECK(hr, "Initialize render polling exclusive");
    UINT32 renFrames = 0;
    renClient->GetBufferSize(&renFrames);
    IAudioRenderClient* ren = nullptr;
    hr = renClient->GetService(__uuidof(IAudioRenderClient), (void**)&ren);
    CHECK(hr, "GetService render");

    printf("capture: %u frames/periode (%.2f ms), padding-doel %d frames "
           "(%.2f ms), gain=%.1f master=%.1f\n",
           capFrames, capFrames * 1000.0 / SR, target, target * 1000.0 / SR,
           gain, master);

    const UINT32 low = (UINT32)(target > 96 ? target - 96 : 0);
    const UINT32 high = (UINT32)(target + 96);

    BYTE* pb = nullptr;
    hr = ren->GetBuffer(target, &pb);
    CHECK(hr, "prime GetBuffer");
    memset(pb, 0, (size_t)target * 4);
    ren->ReleaseBuffer(target, 0);

    long wakeups = 0, underruns = 0, dropFrames = 0, padFrames = 0,
         capErrors = 0, renErrors = 0, timeouts = 0, lateWakeups = 0,
         dspOverBudget = 0;
    double padSum = 0.0;
    long padCount = 0;
    UINT32 padMin = 0xFFFFFFFF, padMax = 0;
    double maxIntervalMs = 0.0, dspMaxMs = 0.0, dspSumMs = 0.0;
    long dspCount = 0;
    float peakIn = 0.0f, peakOut = 0.0f;

    DWORD mmIdx = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIdx);

    std::vector<double> dIn(MAXBLOCK), dOut(MAXBLOCK);
    std::vector<float> fOut(MAXBLOCK);
    double dcX1 = 0.0, dcY1 = 0.0;          // DC-blocker-state
    float fadeEnv = 0.0f;                    // fade-in-envelope
    const float fadeStep = 1.0f / (0.1f * SR);  // ~100 ms fade-in
    renClient->Start();
    capClient->Start();

    LARGE_INTEGER freq, last{}, now, tEnd, t0, dspA, dspB;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    tEnd.QuadPart = t0.QuadPart + freq.QuadPart * seconds;
    const double expectMs = capFrames * 1000.0 / SR;
    const double dspBudgetMs = expectMs * 0.67;
    const long warmupWakeups = SR / (long)capFrames;

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

        UINT32 total = 0;
        for (;;) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            HRESULT h = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (h == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(h)) { capErrors++; break; }
            const int16_t* s = (const int16_t*)data;
            for (UINT32 i = 0; i < frames && total + i < (UINT32)MAXBLOCK; i++) {
                float v = s[i] / 32768.0f * gain;
                float a = fabsf(v);
                if (a > peakIn) peakIn = a;
                dIn[total + i] = (double)v;
            }
            cap->ReleaseBuffer(frames);
            total += frames;
            if (total > (UINT32)MAXBLOCK) total = MAXBLOCK;
            if (frames < capFrames) break;
        }
        if (total == 0) continue;

        // --- NAM ---
        QueryPerformanceCounter(&dspA);
        double* inPtr = dIn.data();
        double* outPtr = dOut.data();
        model->process(&inPtr, &outPtr, (int)total);
        QueryPerformanceCounter(&dspB);
        double dspMs = (dspB.QuadPart - dspA.QuadPart) * 1000.0 / freq.QuadPart;
        if (warm) {
            dspSumMs += dspMs;
            dspCount++;
            if (dspMs > dspMaxMs) dspMaxMs = dspMs;
            if (dspMs > dspBudgetMs) dspOverBudget++;
        }
        // resterende tijd voor fade-out aan het einde van de sessie
        double remainS = (tEnd.QuadPart - now.QuadPart) / (double)freq.QuadPart;
        for (UINT32 i = 0; i < total; i++) {
            // DC-blocker (one-pole highpass ~20 Hz): weert DC-offset uit het
            // model dat anders een harde klik geeft bij start/stop
            double x = dOut[i];
            double y = x - dcX1 + 0.9974 * dcY1;
            dcX1 = x;
            dcY1 = y;
            // fade-in (eerste ~100 ms) en fade-out (laatste ~100 ms)
            if (fadeEnv < 1.0f) fadeEnv += fadeStep;
            float env = fadeEnv > 1.0f ? 1.0f : fadeEnv;
            if (remainS < 0.15) {
                float fo = (float)(remainS / 0.15);
                if (fo < 0.0f) fo = 0.0f;
                env *= fo;
            }
            float v = (float)y * master * env;
            // soft clip: vloeiend begrensd i.p.v. harde knip
            if (v > 0.8f) v = 0.8f + 0.2f * tanhf((v - 0.8f) / 0.2f);
            else if (v < -0.8f) v = -0.8f - 0.2f * tanhf((-v - 0.8f) / 0.2f);
            float a = fabsf(v);
            if (a > peakOut) peakOut = a;
            fOut[i] = v;
        }

        UINT32 padding = 0;
        if (FAILED(renClient->GetCurrentPadding(&padding))) { renErrors++; continue; }
        if (warm) {
            padSum += padding;
            padCount++;
            if (padding < padMin) padMin = padding;
            if (padding > padMax) padMax = padding;
            if (padding == 0) underruns++;
        }

        UINT32 writeOfs = 0, writeN = total;
        if (padding > high && writeN > 0) {
            UINT32 drop = padding - target;
            if (drop > writeN) drop = writeN;
            writeOfs = drop;
            writeN -= drop;
            if (warm) dropFrames += (long)drop;
        }
        UINT32 pad2 = padding + writeN;
        UINT32 fill = 0;
        if (pad2 < low) {
            fill = low - pad2;
            if (warm) padFrames += (long)fill;
        }
        UINT32 n = writeN + fill;
        if (n > renFrames - padding) n = renFrames - padding;
        if (n == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(ren->GetBuffer(n, &out))) { renErrors++; continue; }
        int16_t* o = (int16_t*)out;
        float lastSample = writeN ? fOut[writeOfs + writeN - 1] : 0.0f;
        for (UINT32 i = 0; i < n; i++) {
            float v = i < writeN ? fOut[writeOfs + i] : lastSample;
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
    printf("\n=== RESULTAAT (na 1 s warmup) ===\n");
    printf("latency  input=%.2f ms  output-padding avg=%.2f ms "
           "(min=%u max=%u frames)\n", capFrames * 1000.0 / SR,
           padAvg * 1000.0 / SR, padMin, padMax);
    printf("geschat totaal software: %.2f ms\n",
           capFrames * 1000.0 / SR + padAvg * 1000.0 / SR);
    printf("NAM per blok: avg=%.3f ms  max=%.3f ms  budget=%.2f ms  "
           "overschrijdingen=%ld\n",
           dspCount ? dspSumMs / dspCount : 0.0, dspMaxMs, dspBudgetMs,
           dspOverBudget);
    printf("wakeups=%ld  underruns=%ld  driftdrops=%ld  opvulling=%ld  "
           "errors cap/ren=%ld/%ld  timeouts=%ld\n", wakeups, underruns,
           dropFrames, padFrames, capErrors, renErrors, timeouts);
    printf("wakeup max=%.2f ms  laat(>1.5x)=%ld\n", maxIntervalMs, lateWakeups);
    printf("signaal peak in=%.1f dBFS  uit=%.1f dBFS\n",
           peakIn > 0 ? 20.0 * log10(peakIn) : -999.0,
           peakOut > 0 ? 20.0 * log10(peakOut) : -999.0);
    return 0;
}
