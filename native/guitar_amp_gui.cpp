// Guitar Audio Lab â€” GUI (Dear ImGui + DX11) rond de NAM-audio-engine.
//
// Engine = passthrough_ll/guitar_amp-architectuur: WASAPI Exclusive capture
// (event, 3 ms) als klok + polling render met kleine padding (~4,8 ms totaal),
// NAM-model ertussen, DC-blocker, fades, soft clip.
// De engine draait op een eigen "Pro Audio"-thread; de UI praat via atomics.
//
// Build: native\build.ps1 (sectie guitar_amp_gui)

#define _CRT_RAND_S
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <cstdarg>

#include <windows.h>
#include <timeapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <d3d11.h>
#include <endpointvolume.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <shellapi.h>
#include <winsock2.h>
#include <wincodec.h>

#include "json.hpp"

#include <atomic>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/activations.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "windowscodecs.lib")

static const int SR = 48000;
namespace fs = std::filesystem;

// backing track voor de speler (48 kHz stereo interleaved float)
struct PlayerTrack {
    std::vector<float> pcm;
    size_t frames = 0;
    size_t resumePos = 0;  // afspeelpositie overleeft een engine-herstart
    std::string name;
};

// cabinet-IR: taps staan OMGEKEERD zodat de convolutie een contiguÃ«
// dot-product wordt (auto-vectoriseerbaar); lege taps = "IR uit"
struct IrData {
    std::vector<float> taps;  // reversed, max 4096
    std::string name;
};

// ============================================================= engine-state
struct EngineShared {
    // parameters (UI -> engine)
    std::atomic<float> gain{1.0f};
    std::atomic<float> master{0.5f};
    // besturing
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    // statistiek (engine -> UI)
    std::atomic<float> peakIn{0.0f};      // block-peak, UI reset naar 0
    std::atomic<float> peakOut{0.0f};
    std::atomic<int> padAvgFrames{0};
    std::atomic<int> dspAvgUs{0};
    std::atomic<int> dspMaxUs{0};
    std::atomic<long> underruns{0};
    std::atomic<long> driftCorr{0};
    std::atomic<long> lateWakeups{0};
    std::atomic<double> modelSR{0.0};
    // naadloze modelwissel: loader-thread zet het nieuwe model klaar in
    // pendingModel; de engine pikt het op blokgrens op en parkeert het oude
    // in retiredModel, dat de UI-thread opruimt (geen delete op het audiopad)
    std::atomic<nam::DSP*> pendingModel{nullptr};
    std::atomic<nam::DSP*> retiredModel{nullptr};
    std::atomic<bool> modelLoading{false};

    // cabinet-IR (zelfde pending/retired-patroon als het model)
    std::atomic<IrData*> pendingIr{nullptr};
    std::atomic<IrData*> retiredIr{nullptr};
    std::atomic<bool> irLoading{false};

    // speler (zelfde pending/retired-patroon als het model)
    std::atomic<PlayerTrack*> pendingTrack{nullptr};
    std::atomic<PlayerTrack*> retiredTrack{nullptr};
    std::atomic<bool> trackLoading{false};
    std::atomic<bool> playerPlaying{false};
    std::atomic<bool> playerLoop{false};
    std::atomic<float> playerVol{0.7f};
    std::atomic<long long> playerSeek{-1};   // gewenste positie in frames
    std::atomic<long long> playerPosFr{0};   // huidige positie (engine -> UI)
    std::atomic<long long> playerLenFr{0};
    std::mutex trackNameMutex;
    std::string trackName;

    // stemmer: ring met rauwe (pre-gain) inputsamples voor pitch-detectie
    static const int TRING = 16384;
    float tring[TRING] = {};
    std::atomic<uint32_t> tringW{0};

    // 3-bands EQ (dB), engine herrekent coefficienten bij eqDirty
    std::atomic<float> eqBass{0.0f};
    std::atomic<float> eqMid{0.0f};
    std::atomic<float> eqTreble{0.0f};
    std::atomic<bool> eqDirty{true};

    // noise gate: drempel in dBFS; -90 = uit
    std::atomic<float> gateDb{-90.0f};

    // outputmodus van de draaiende engine (exclusive of shared-fallback)
    std::atomic<bool> outShared{false};

    // online modellencatalogus + downloadstatus
    std::mutex catMutex;
    std::vector<std::string> catalog;       // repo-paden van .nam-bestanden
    std::atomic<bool> catFetching{false};
    std::atomic<bool> downloading{false};
    std::atomic<bool> modelsChanged{false};  // UI moet models\ opnieuw scannen
    std::mutex netStatusMutex;
    std::string netStatus;

    void setNetStatus(const std::string& s) {
        std::lock_guard<std::mutex> lk(netStatusMutex);
        netStatus = s;
    }
    std::string getNetStatus() {
        std::lock_guard<std::mutex> lk(netStatusMutex);
        return netStatus;
    }

    void setTrackName(const std::string& n) {
        std::lock_guard<std::mutex> lk(trackNameMutex);
        trackName = n;
    }
    std::string getTrackName() {
        std::lock_guard<std::mutex> lk(trackNameMutex);
        return trackName;
    }
    std::mutex errMutex;
    std::string lastError;

    void setError(const std::string& e) {
        std::lock_guard<std::mutex> lk(errMutex);
        lastError = e;
    }
    std::string getError() {
        std::lock_guard<std::mutex> lk(errMutex);
        return lastError;
    }
};

static EngineShared g;
static std::thread gEngineThread;

// ------------------------------------------------------------- device-hulp
static IMMDevice* findDevice(IMMDeviceEnumerator* enu, EDataFlow flow,
                             const wchar_t* needle) {
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
                std::wstring lower = pv.pwszVal;
                for (auto& c : lower) c = towlower(c);
                if (lower.find(needle) != std::wstring::npos) {
                    found = dev;
                    dev->AddRef();
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

// ============================================================= EQ (biquads)
// RBJ-cookbook filters; transposed direct form II.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline double proc(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void lowShelf(double f0, double dB) {
        double A = pow(10.0, dB / 40.0), w = 2.0 * 3.14159265358979 * f0 / SR;
        double cw = cos(w), sw = sin(w);
        double al = sw / 2.0 * sqrt(2.0);  // shelf slope S=1
        double sa = 2.0 * sqrt(A) * al;
        double a0 = (A + 1) + (A - 1) * cw + sa;
        b0 = A * ((A + 1) - (A - 1) * cw + sa) / a0;
        b1 = 2 * A * ((A - 1) - (A + 1) * cw) / a0;
        b2 = A * ((A + 1) - (A - 1) * cw - sa) / a0;
        a1 = -2 * ((A - 1) + (A + 1) * cw) / a0;
        a2 = ((A + 1) + (A - 1) * cw - sa) / a0;
    }
    void highShelf(double f0, double dB) {
        double A = pow(10.0, dB / 40.0), w = 2.0 * 3.14159265358979 * f0 / SR;
        double cw = cos(w), sw = sin(w);
        double al = sw / 2.0 * sqrt(2.0);
        double sa = 2.0 * sqrt(A) * al;
        double a0 = (A + 1) - (A - 1) * cw + sa;
        b0 = A * ((A + 1) + (A - 1) * cw + sa) / a0;
        b1 = -2 * A * ((A - 1) + (A + 1) * cw) / a0;
        b2 = A * ((A + 1) + (A - 1) * cw - sa) / a0;
        a1 = 2 * ((A - 1) - (A + 1) * cw) / a0;
        a2 = ((A + 1) - (A - 1) * cw - sa) / a0;
    }
    void peaking(double f0, double q, double dB) {
        double A = pow(10.0, dB / 40.0), w = 2.0 * 3.14159265358979 * f0 / SR;
        double cw = cos(w), al = sin(w) / (2.0 * q);
        double a0 = 1 + al / A;
        b0 = (1 + al * A) / a0;
        b1 = -2 * cw / a0;
        b2 = (1 - al * A) / a0;
        a1 = -2 * cw / a0;
        a2 = (1 - al / A) / a0;
    }
};

// ============================================================= engine-thread
#define ENG_FAIL(msg)                                       \
    do {                                                    \
        char buf_[160];                                     \
        snprintf(buf_, sizeof(buf_), "%s (hr=0x%08lx)", msg, \
                 (unsigned long)hr);                        \
        g.setError(buf_);                                   \
        goto cleanup;                                       \
    } while (0)

static void engineMain(std::wstring modelPath, std::wstring outDevId) {
    const int MAXBLOCK = 2048;

    IMMDeviceEnumerator* enu = nullptr;
    IMMDevice *inDev = nullptr, *outDev = nullptr;
    IAudioClient *capClient = nullptr, *renClient = nullptr;
    IAudioCaptureClient* cap = nullptr;
    IAudioRenderClient* ren = nullptr;
    HANDLE capEvent = nullptr;
    HANDLE mm = nullptr;
    bool started = false;
    std::unique_ptr<nam::DSP> model;
    HRESULT hr = S_OK;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);

    // --- model laden ---
    try {
        nam::activations::Activation::enable_fast_tanh();
        model = nam::get_dsp(fs::path(modelPath));
    } catch (const std::exception& e) {
        g.setError(std::string("model laden mislukt: ") + e.what());
        goto cleanup;
    }
    if (!model || model->NumInputChannels() != 1 ||
        model->NumOutputChannels() != 1) {
        g.setError("model laden mislukt of geen mono model");
        goto cleanup;
    }
    g.modelSR.store(model->GetExpectedSampleRate());
    model->Reset(SR, MAXBLOCK);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enu);
    if (FAILED(hr)) ENG_FAIL("MMDeviceEnumerator");
    inDev = findDevice(enu, eCapture, L"rocksmith");
    if (!outDevId.empty())
        enu->GetDevice(outDevId.c_str(), &outDev);
    if (!outDev) outDev = findDevice(enu, eRender, L"realtek digital");
    if (!inDev || !outDev) {
        g.setError("Rocksmith-kabel of outputdevice niet gevonden");
        goto cleanup;
    }

    {
        // --- capture ---
        hr = inDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&capClient);
        if (FAILED(hr)) ENG_FAIL("Activate capture");
        REFERENCE_TIME defP = 0, minP = 0;
        capClient->GetDevicePeriod(&defP, &minP);
        WAVEFORMATEXTENSIBLE inFmt = pcm16(1);
        hr = capClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP,
                                   minP, (WAVEFORMATEX*)&inFmt, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 fr = 0;
            capClient->GetBufferSize(&fr);
            capClient->Release();
            capClient = nullptr;
            REFERENCE_TIME p = (REFERENCE_TIME)((10000000.0 * fr) / SR + 0.5);
            hr = inDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 (void**)&capClient);
            if (FAILED(hr)) ENG_FAIL("re-Activate capture");
            hr = capClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p, p,
                                       (WAVEFORMATEX*)&inFmt, nullptr);
        }
        // net gestopt exclusive-sessies geven soms kort DEVICE_IN_USE: retry
        for (int retry = 0; hr == AUDCLNT_E_DEVICE_IN_USE && retry < 5;
             retry++) {
            Sleep(200);
            capClient->Release();
            capClient = nullptr;
            hr = inDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 (void**)&capClient);
            if (FAILED(hr)) break;
            hr = capClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                       minP, minP, (WAVEFORMATEX*)&inFmt,
                                       nullptr);
        }
        if (hr == AUDCLNT_E_DEVICE_IN_USE) {
            g.setError("input bezet: sluit andere audio-apps (of Rocksmith)");
            goto cleanup;
        }
        if (FAILED(hr)) ENG_FAIL("Initialize capture");
        UINT32 capFrames = 0;
        capClient->GetBufferSize(&capFrames);
        capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        capClient->SetEventHandle(capEvent);
        hr = capClient->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
        if (FAILED(hr)) ENG_FAIL("GetService capture");

        // --- render (polling) ---
        hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              (void**)&renClient);
        if (FAILED(hr)) ENG_FAIL("Activate render");
        WAVEFORMATEXTENSIBLE outFmt = pcm16(2);
        bool sharedOut = false;
        hr = renClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 200000, 0,
                                   (WAVEFORMATEX*)&outFmt, nullptr);
        for (int retry = 0; hr == AUDCLNT_E_DEVICE_IN_USE && retry < 5;
             retry++) {
            Sleep(200);
            renClient->Release();
            renClient = nullptr;
            hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  (void**)&renClient);
            if (FAILED(hr)) break;
            hr = renClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 200000,
                                       0, (WAVEFORMATEX*)&outFmt, nullptr);
        }
        if (hr == AUDCLNT_E_DEVICE_IN_USE) {
            g.setError("output bezet: sluit apps die geluid afspelen");
            goto cleanup;
        }
        if (FAILED(hr)) {
            // fallback: shared mode met automatische formaatconversie â€”
            // nodig voor devices zonder exclusive-ondersteuning (Bluetooth)
            renClient->Release();
            renClient = nullptr;
            hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  (void**)&renClient);
            if (FAILED(hr)) ENG_FAIL("re-Activate render (shared)");
            hr = renClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                1000000 /* 100 ms buffer */, 0, (WAVEFORMATEX*)&outFmt,
                nullptr);
            if (FAILED(hr)) ENG_FAIL("Initialize render (shared)");
            sharedOut = true;
        }
        g.outShared.store(sharedOut);
        // padding-doel: exclusive kan strak (6 ms); shared verbruikt in
        // 10 ms-porties, dus daar ruimer mikken (30 ms)
        const UINT32 TARGET = sharedOut ? 1440 : 288;
        const UINT32 slack = sharedOut ? 512 : 96;
        UINT32 renFrames = 0;
        renClient->GetBufferSize(&renFrames);
        hr = renClient->GetService(__uuidof(IAudioRenderClient), (void**)&ren);
        if (FAILED(hr)) ENG_FAIL("GetService render");

        const UINT32 low = TARGET - slack, high = TARGET + slack;
        BYTE* pb = nullptr;
        hr = ren->GetBuffer(TARGET, &pb);
        if (FAILED(hr)) ENG_FAIL("prime");
        memset(pb, 0, (size_t)TARGET * 4);
        ren->ReleaseBuffer(TARGET, 0);

        DWORD mmIdx = 0;
        mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIdx);

        std::vector<double> dIn(MAXBLOCK), dOut(MAXBLOCK);
        std::vector<float> fOut(MAXBLOCK);
        std::unique_ptr<PlayerTrack> track;
        size_t tpos = 0;
        std::unique_ptr<IrData> ir;
        std::vector<float> irTail, irCtx;  // convolutie-context
        Biquad eqB, eqM, eqT;
        bool eqFlat = true;
        g.eqDirty.store(true);  // coefficienten berekenen bij start
        double gateEnv = 0.0, gateGain = 1.0;
        double dcX1 = 0.0, dcY1 = 0.0;
        float fadeEnv = 0.0f;
        const float fadeIn = 1.0f / (0.1f * SR);
        const float fadeOut = 1.0f / (0.1f * SR);
        bool fadingOut = false;

        long wakeups = 0;
        double padAcc = 0.0, dspAcc = 0.0;
        long accN = 0;
        double dspMax = 0.0;
        LARGE_INTEGER freq, last{}, now, dspA, dspB;
        QueryPerformanceFrequency(&freq);
        const double expectMs = capFrames * 1000.0 / SR;
        const long warmup = SR / (long)capFrames;

        renClient->Start();
        capClient->Start();
        started = true;
        g.running.store(true);

        for (;;) {
            if (g.stopRequested.load(std::memory_order_relaxed) && !fadingOut)
                fadingOut = true;
            DWORD w = WaitForSingleObject(capEvent, 500);
            if (w != WAIT_OBJECT_0) {
                if (fadingOut) break;
                continue;
            }
            wakeups++;
            bool warm = wakeups > warmup;
            QueryPerformanceCounter(&now);
            if (last.QuadPart && warm) {
                double ms = (now.QuadPart - last.QuadPart) * 1000.0 /
                            freq.QuadPart;
                if (ms > 1.5 * expectMs) g.lateWakeups++;
            }
            last = now;

            UINT32 total = 0;
            for (;;) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                HRESULT h =
                    cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (h == AUDCLNT_S_BUFFER_EMPTY) break;
                if (FAILED(h)) break;
                const int16_t* s = (const int16_t*)data;
                float gn = g.gain.load(std::memory_order_relaxed);
                float pk = 0.0f;
                uint32_t tw = g.tringW.load(std::memory_order_relaxed);
                for (UINT32 i = 0; i < frames && total + i < (UINT32)MAXBLOCK;
                     i++) {
                    float raw = s[i] / 32768.0f;
                    g.tring[(tw + i) & (EngineShared::TRING - 1)] = raw;
                    float v = raw * gn;
                    float a = fabsf(v);
                    if (a > pk) pk = a;
                    dIn[total + i] = (double)v;
                }
                g.tringW.store(tw + frames, std::memory_order_release);
                if (pk > g.peakIn.load(std::memory_order_relaxed))
                    g.peakIn.store(pk, std::memory_order_relaxed);
                cap->ReleaseBuffer(frames);
                total += frames;
                if (total > (UINT32)MAXBLOCK) total = MAXBLOCK;
                if (frames < capFrames) break;
            }
            if (total == 0) continue;

            // noise gate (voor het model): envelope-follower + zachte gain
            {
                float gtDb = g.gateDb.load(std::memory_order_relaxed);
                if (gtDb > -89.0f) {
                    double thr = pow(10.0, gtDb / 20.0);
                    for (UINT32 i = 0; i < total; i++) {
                        double a = fabs(dIn[i]);
                        gateEnv = a > gateEnv ? a : gateEnv * 0.9995;
                        double tgt = gateEnv > thr ? 1.0 : 0.0;
                        gateGain += (tgt - gateGain) *
                                    (tgt > gateGain ? 0.01 : 0.001);
                        dIn[i] *= gateGain;
                    }
                } else {
                    gateGain = 1.0;
                }
            }

            // naadloze modelwissel op blokgrens
            {
                nam::DSP* nm = g.pendingModel.exchange(nullptr);
                if (nm) {
                    nam::DSP* old = model.release();
                    model.reset(nm);
                    g.modelSR.store(model->GetExpectedSampleRate());
                    dcX1 = dcY1 = 0.0;
                    fadeEnv = 0.0f;  // korte fade-in voor het nieuwe model
                    nam::DSP* prev = g.retiredModel.exchange(old);
                    if (prev) delete prev;  // zeldzame fallback bij snel wisselen
                }
            }
            // nieuwe cabinet-IR oppakken
            {
                IrData* ni = g.pendingIr.exchange(nullptr);
                if (ni) {
                    IrData* old = ir.release();
                    if (ni->taps.empty()) {
                        delete ni;  // lege taps = IR uitschakelen
                    } else {
                        ir.reset(ni);
                        irTail.assign(ir->taps.size() - 1, 0.0f);
                    }
                    fadeEnv = 0.0f;
                    IrData* prev = g.retiredIr.exchange(old);
                    if (prev) delete prev;
                }
            }
            // nieuwe backing track oppakken
            {
                PlayerTrack* nt = g.pendingTrack.exchange(nullptr);
                if (nt) {
                    PlayerTrack* old = track.release();
                    track.reset(nt);
                    tpos = track->resumePos < track->frames ? track->resumePos
                                                            : 0;
                    PlayerTrack* prev = g.retiredTrack.exchange(old);
                    if (prev) delete prev;
                }
                long long sk = g.playerSeek.exchange(-1);
                if (sk >= 0 && track)
                    tpos = (size_t)sk < track->frames ? (size_t)sk
                                                      : track->frames;
            }

            QueryPerformanceCounter(&dspA);
            double* ip = dIn.data();
            double* op = dOut.data();
            model->process(&ip, &op, (int)total);
            // cabinet-IR: contiguÃ« convolutie (taps omgekeerd opgeslagen)
            if (ir) {
                const size_t nt = ir->taps.size();
                irCtx.resize(nt - 1 + total);
                memcpy(irCtx.data(), irTail.data(),
                       (nt - 1) * sizeof(float));
                for (UINT32 i = 0; i < total; i++)
                    irCtx[nt - 1 + i] = (float)dOut[i];
                const float* tp = ir->taps.data();
                for (UINT32 i = 0; i < total; i++) {
                    const float* x = irCtx.data() + i;
                    float acc = 0.0f;
                    for (size_t j = 0; j < nt; j++) acc += tp[j] * x[j];
                    dOut[i] = acc;
                }
                memcpy(irTail.data(), irCtx.data() + total,
                       (nt - 1) * sizeof(float));
            }
            QueryPerformanceCounter(&dspB);
            double dspMs =
                (dspB.QuadPart - dspA.QuadPart) * 1000.0 / freq.QuadPart;

            if (g.eqDirty.exchange(false)) {
                float b = g.eqBass.load(), m = g.eqMid.load(),
                      t = g.eqTreble.load();
                eqB.lowShelf(100.0, b);
                eqM.peaking(550.0, 0.7, m);
                eqT.highShelf(3000.0, t);
                eqFlat = fabsf(b) < 0.01f && fabsf(m) < 0.01f &&
                         fabsf(t) < 0.01f;
            }
            float ms = g.master.load(std::memory_order_relaxed);
            float pkOut = 0.0f;
            for (UINT32 i = 0; i < total; i++) {
                double x = dOut[i];
                double y = x - dcX1 + 0.9974 * dcY1;
                dcX1 = x;
                dcY1 = y;
                if (!eqFlat) y = eqT.proc(eqM.proc(eqB.proc(y)));
                if (fadingOut) {
                    fadeEnv -= fadeOut;
                    if (fadeEnv < 0.0f) fadeEnv = 0.0f;
                } else if (fadeEnv < 1.0f) {
                    fadeEnv += fadeIn;
                    if (fadeEnv > 1.0f) fadeEnv = 1.0f;
                }
                float v = (float)y * ms * fadeEnv;
                if (v > 0.8f) v = 0.8f + 0.2f * tanhf((v - 0.8f) / 0.2f);
                else if (v < -0.8f) v = -0.8f - 0.2f * tanhf((-v - 0.8f) / 0.2f);
                float a = fabsf(v);
                if (a > pkOut) pkOut = a;
                fOut[i] = v;
            }
            if (pkOut > g.peakOut.load(std::memory_order_relaxed))
                g.peakOut.store(pkOut, std::memory_order_relaxed);

            UINT32 padding = 0;
            if (FAILED(renClient->GetCurrentPadding(&padding))) continue;
            if (warm) {
                padAcc += padding;
                dspAcc += dspMs;
                accN++;
                if (dspMs > dspMax) dspMax = dspMs;
                if (padding == 0) g.underruns++;
                if (accN >= 100) {  // ~elke 0,3 s publiceren
                    g.padAvgFrames.store((int)(padAcc / accN));
                    g.dspAvgUs.store((int)(dspAcc / accN * 1000.0));
                    g.dspMaxUs.store((int)(dspMax * 1000.0));
                    padAcc = dspAcc = 0.0;
                    accN = 0;
                }
            }

            UINT32 writeOfs = 0, writeN = total;
            if (padding > high && writeN > 0) {
                UINT32 drop = padding - TARGET;
                if (drop > writeN) drop = writeN;
                writeOfs = drop;
                writeN -= drop;
                if (warm) g.driftCorr += (long)drop;
            }
            UINT32 pad2 = padding + writeN;
            UINT32 fill = 0;
            if (pad2 < low) {
                fill = low - pad2;
                if (warm) g.driftCorr += (long)fill;
            }
            UINT32 n = writeN + fill;
            if (n > renFrames - padding) n = renFrames - padding;
            if (n == 0) { if (fadingOut && fadeEnv <= 0.0f) break; continue; }

            BYTE* out = nullptr;
            if (FAILED(ren->GetBuffer(n, &out))) continue;
            int16_t* o = (int16_t*)out;
            float lastS = writeN ? fOut[writeOfs + writeN - 1] : 0.0f;
            const bool tPlay = track &&
                               g.playerPlaying.load(std::memory_order_relaxed);
            const float tVol = g.playerVol.load(std::memory_order_relaxed);
            const float* tp = track ? track->pcm.data() : nullptr;
            const size_t tFrames = track ? track->frames : 0;
            for (UINT32 i = 0; i < n; i++) {
                float gv = i < writeN ? fOut[writeOfs + i] : lastS;
                float L = gv, R = gv;
                if (tPlay && tpos < tFrames) {
                    L += tp[tpos * 2] * tVol;
                    R += tp[tpos * 2 + 1] * tVol;
                    tpos++;
                }
                auto clip = [](float v) {
                    if (v > 0.8f) v = 0.8f + 0.2f * tanhf((v - 0.8f) / 0.2f);
                    else if (v < -0.8f)
                        v = -0.8f - 0.2f * tanhf((-v - 0.8f) / 0.2f);
                    return v;
                };
                o[i * 2] = (int16_t)lrintf(clip(L) * 32767.0f);
                o[i * 2 + 1] = (int16_t)lrintf(clip(R) * 32767.0f);
            }
            ren->ReleaseBuffer(n, 0);
            if (track) {
                if (tPlay && tpos >= tFrames) {
                    if (g.playerLoop.load(std::memory_order_relaxed)) tpos = 0;
                    else g.playerPlaying.store(false);
                }
                track->resumePos = tpos;
                g.playerPosFr.store((long long)tpos,
                                    std::memory_order_relaxed);
            }

            if (fadingOut && fadeEnv <= 0.0f) break;
        }

        // track teruggeven zodat hij een engine-herstart overleeft;
        // staat er al een nieuwere klaar, dan wint die en vervalt de onze
        if (track) {
            track->resumePos = tpos;
            PlayerTrack* expected = nullptr;
            if (g.pendingTrack.compare_exchange_strong(expected, track.get()))
                track.release();
            else
                track.reset();
        }
        // idem voor de cabinet-IR
        if (ir) {
            IrData* expected = nullptr;
            if (g.pendingIr.compare_exchange_strong(expected, ir.get()))
                ir.release();
            else
                ir.reset();
        }
    }

cleanup:
    {
        nam::DSP* nm = g.pendingModel.exchange(nullptr);
        if (nm) delete nm;  // niet-opgepikte wissel opruimen
    }
    if (started) {
        capClient->Stop();
        renClient->Stop();
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    if (cap) cap->Release();
    if (ren) ren->Release();
    if (capClient) capClient->Release();
    if (renClient) renClient->Release();
    if (capEvent) CloseHandle(capEvent);
    if (inDev) inDev->Release();
    if (outDev) outDev->Release();
    if (enu) enu->Release();
    timeEndPeriod(1);
    CoUninitialize();
    g.running.store(false);
}

static void stopEngine() {
    if (gEngineThread.joinable()) {
        g.stopRequested.store(true);
        gEngineThread.join();
    }
    g.stopRequested.store(false);
}

static void startEngine(const std::wstring& modelPath,
                        const std::wstring& outDevId) {
    stopEngine();
    g.setError("");
    g.underruns = 0;
    g.driftCorr = 0;
    g.lateWakeups = 0;
    g.dspMaxUs = 0;
    gEngineThread = std::thread(engineMain, modelPath, outDevId);
}

// ============================================================= modellenshop
// Kleine WinHTTP GET-helper (https).
static bool httpGet(const std::wstring& host, const std::wstring& path,
                    std::string& out, std::string& err) {
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"GuitarAudioLab/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                0);
    if (!ses) {
        err = "WinHttpOpen faalde";
        return false;
    }
    HINTERNET con = WinHttpConnect(ses, host.c_str(),
                                   INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = nullptr;
    if (con)
        req = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (req &&
        WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n",
                           (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req,
                            WINHTTP_QUERY_STATUS_CODE |
                                WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
                    break;
                size_t base = out.size();
                out.resize(base + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, &out[base], avail, &got)) break;
                out.resize(base + got);
            }
            ok = !out.empty();
            if (!ok) err = "leeg antwoord";
        } else {
            err = "HTTP-status " + std::to_string(status);
        }
    } else {
        err = "verbinding mislukt";
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

static std::wstring urlEncodePath(const std::string& s) {
    std::wstring out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
            c == '/') {
            out.push_back((wchar_t)c);
        } else {
            out.push_back(L'%');
            out.push_back((wchar_t)hex[c >> 4]);
            out.push_back((wchar_t)hex[c & 15]);
        }
    }
    return out;
}

static const wchar_t* CATALOG_REPO_API =
    L"/repos/pelennor2170/NAM_models/git/trees/HEAD?recursive=1";

static void fetchCatalogAsync(const fs::path& cacheFile) {
    if (g.catFetching.exchange(true)) return;
    std::thread([cacheFile] {
        g.setNetStatus("lijst ophalen...");
        std::string body, err;
        if (httpGet(L"api.github.com", CATALOG_REPO_API, body, err)) {
            std::vector<std::string> paths;
            try {
                auto j = nlohmann::json::parse(body);
                for (auto& e : j["tree"]) {
                    std::string p = e.value("path", "");
                    if (p.size() > 4 && p.substr(p.size() - 4) == ".nam")
                        paths.push_back(p);
                }
            } catch (...) {
                err = "JSON-parsefout";
            }
            if (!paths.empty()) {
                {
                    std::lock_guard<std::mutex> lk(g.catMutex);
                    g.catalog = paths;
                }
                // cache naar schijf voor volgende sessies
                FILE* f = _wfopen(cacheFile.wstring().c_str(), L"wb");
                if (f) {
                    for (auto& p : paths) fprintf(f, "%s\n", p.c_str());
                    fclose(f);
                }
                g.setNetStatus(std::to_string(paths.size()) +
                               " modellen online");
            } else if (!err.empty()) {
                g.setNetStatus("ophalen mislukt: " + err);
            } else {
                g.setNetStatus("geen modellen gevonden");
            }
        } else {
            g.setNetStatus("ophalen mislukt: " + err);
        }
        g.catFetching.store(false);
    }).detach();
}

static void loadCatalogCache(const fs::path& cacheFile) {
    FILE* f = _wfopen(cacheFile.wstring().c_str(), L"rb");
    if (!f) return;
    std::vector<std::string> paths;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string p = line;
        while (!p.empty() && (p.back() == '\n' || p.back() == '\r'))
            p.pop_back();
        if (!p.empty()) paths.push_back(p);
    }
    fclose(f);
    if (!paths.empty()) {
        std::lock_guard<std::mutex> lk(g.catMutex);
        g.catalog = std::move(paths);
    }
}

static void downloadModelAsync(const std::string& repoPath,
                               const fs::path& modelsDir) {
    if (g.downloading.exchange(true)) return;
    std::thread([repoPath, modelsDir] {
        std::string fname = fs::path(repoPath).filename().string();
        g.setNetStatus("downloaden: " + fname);
        std::wstring url = L"/pelennor2170/NAM_models/HEAD/" +
                           urlEncodePath(repoPath);
        std::string body, err;
        if (httpGet(L"raw.githubusercontent.com", url, body, err)) {
            fs::path dest = modelsDir / fs::path(repoPath).filename();
            FILE* f = _wfopen(dest.wstring().c_str(), L"wb");
            if (f) {
                fwrite(body.data(), 1, body.size(), f);
                fclose(f);
                g.modelsChanged.store(true);
                g.setNetStatus("gedownload: " + fname);
            } else {
                g.setNetStatus("opslaan mislukt: " + fname);
            }
        } else {
            g.setNetStatus("download mislukt: " + err);
        }
        g.downloading.store(false);
    }).detach();
}

// ============================================================= metadata
// Per .nam-bestand: merk/stijl/cab/mic/maker/tags/rating, in
// models\metadata.json. Bronnen: TONE3000-API (bij download of backfill),
// anders afgeleid uit de bestandsnaam (deriveMeta, verderop).
struct ModelMeta {
    std::string make, style, cab, mic, maker, source;
    std::vector<std::string> tags;
    int rating = 0;
};

// downloadthreads leveren metadata hier af; de UI-thread verwerkt en bewaart
static std::mutex gMetaPendMutex;
static std::map<std::string, ModelMeta> gMetaPending;

static std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ============================================================= TONE3000
// OAuth 2.0 PKCE-integratie met de officiele TONE3000-API (tone3000.com/api).
// publishable key (t3k_pub_...) = client_id; tokens in t3k.json naast models\.

// --- compacte SHA-256 (voor de PKCE code challenge) ---
struct Sha256 {
    static void hash(const uint8_t* d, size_t len, uint8_t out[32]) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
            0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::vector<uint8_t> msg(d, d + len);
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) msg.push_back(0);
        uint64_t bits = (uint64_t)len * 8;
        for (int i = 7; i >= 0; i--) msg.push_back((uint8_t)(bits >> (i * 8)));
        auto rotr = [](uint32_t x, int n) {
            return (x >> n) | (x << (32 - n));
        };
        for (size_t off = 0; off < msg.size(); off += 64) {
            uint32_t w[64];
            for (int i = 0; i < 16; i++)
                w[i] = (msg[off + i * 4] << 24) | (msg[off + i * 4 + 1] << 16) |
                       (msg[off + i * 4 + 2] << 8) | msg[off + i * 4 + 3];
            for (int i = 16; i < 64; i++) {
                uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
                              (w[i - 15] >> 3);
                uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
                              (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4],
                     f = h[5], gg = h[6], hh = h[7];
            for (int i = 0; i < 64; i++) {
                uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                uint32_t ch = (e & f) ^ (~e & gg);
                uint32_t t1 = hh + S1 + ch + K[i] + w[i];
                uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t t2 = S0 + mj;
                hh = gg; gg = f; f = e; e = dd + t1;
                dd = c; c = b; b = a; a = t1 + t2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += dd;
            h[4] += e; h[5] += f; h[6] += gg; h[7] += hh;
        }
        for (int i = 0; i < 8; i++) {
            out[i * 4] = (uint8_t)(h[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
            out[i * 4 + 3] = (uint8_t)h[i];
        }
    }
};

static std::string b64url(const uint8_t* d, size_t n) {
    static const char* tab =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16;
        if (i + 1 < n) v |= d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        out.push_back(tab[(v >> 18) & 63]);
        out.push_back(tab[(v >> 12) & 63]);
        if (i + 1 < n) out.push_back(tab[(v >> 6) & 63]);
        if (i + 2 < n) out.push_back(tab[v & 63]);
    }
    return out;
}

static std::string randomB64url(int bytes) {
    std::vector<uint8_t> buf(bytes);
    for (auto& b : buf) {
        unsigned int r = 0;
        rand_s(&r);
        b = (uint8_t)r;
    }
    return b64url(buf.data(), buf.size());
}

static std::string formEncode(const std::string& s) {
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out.push_back((char)c);
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

// algemene HTTPS-request met optionele body en bearer-token
static bool httpRequestEx(const std::wstring& host, const std::wstring& path,
                          const wchar_t* method, const std::string& body,
                          const std::string& bearer, std::string& out,
                          std::string& err) {
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"GuitarAudioLab/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                0);
    if (!ses) { err = "WinHttpOpen faalde"; return false; }
    HINTERNET con = WinHttpConnect(ses, host.c_str(),
                                   INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = nullptr;
    if (con)
        req = WinHttpOpenRequest(con, method, path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    std::wstring hdrs = L"Accept: application/json\r\n";
    if (!body.empty())
        hdrs += L"Content-Type: application/x-www-form-urlencoded\r\n";
    if (!bearer.empty()) {
        hdrs += L"Authorization: Bearer ";
        hdrs += std::wstring(bearer.begin(), bearer.end());
        hdrs += L"\r\n";
    }
    if (req &&
        WinHttpSendRequest(req, hdrs.c_str(), (DWORD)-1,
                           body.empty() ? WINHTTP_NO_REQUEST_DATA
                                        : (LPVOID)body.data(),
                           (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req,
                            WINHTTP_QUERY_STATUS_CODE |
                                WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            size_t base = out.size();
            out.resize(base + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, &out[base], avail, &got)) break;
            out.resize(base + got);
        }
        ok = status >= 200 && status < 300;
        if (!ok) err = "HTTP-status " + std::to_string(status);
    } else {
        err = "verbinding mislukt";
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

// https://host/pad -> (host, pad)
static bool splitUrl(const std::string& url, std::wstring& host,
                     std::wstring& path) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    size_t h0 = p + 3;
    size_t slash = url.find('/', h0);
    std::string hs = url.substr(h0, slash == std::string::npos
                                        ? std::string::npos
                                        : slash - h0);
    std::string ps = slash == std::string::npos ? "/" : url.substr(slash);
    host.assign(hs.begin(), hs.end());
    path.assign(ps.begin(), ps.end());
    return true;
}

struct T3KResult {
    long long toneId = 0;
    std::string title, creator, makes, tags;  // makes/tags komma-gescheiden
    long long downloads = 0, favorites = 0;
};

struct T3KState {
    std::mutex mu;                 // beschermt tokens + pubKey
    std::string pubKey, accessToken, refreshToken;
    long long expiresAt = 0;       // epoch seconden
    std::atomic<bool> connected{false};
    std::atomic<bool> busy{false};
    std::mutex resMu;
    std::vector<T3KResult> results;
    std::mutex dlMu;               // register: tone-ID -> bestandsnaam
    std::map<long long, std::string> downloaded;
    // paginering van de laatste zoekopdracht
    std::atomic<int> curPage{1};
    std::atomic<int> totalPages{1};
    std::atomic<long long> totalTones{0};
    fs::path configFile;

    void save() {
        nlohmann::json dl = nlohmann::json::object();
        {
            std::lock_guard<std::mutex> lk(dlMu);
            for (auto& [id, fn] : downloaded)
                dl[std::to_string(id)] = fn;
        }
        std::lock_guard<std::mutex> lk(mu);
        nlohmann::json j = {{"publishable_key", pubKey},
                            {"access_token", accessToken},
                            {"refresh_token", refreshToken},
                            {"expires_at", expiresAt},
                            {"downloads", dl}};
        FILE* f = _wfopen(configFile.wstring().c_str(), L"wb");
        if (f) {
            std::string s = j.dump(2);
            fwrite(s.data(), 1, s.size(), f);
            fclose(f);
        }
    }
    void load() {
        FILE* f = _wfopen(configFile.wstring().c_str(), L"rb");
        if (!f) return;
        std::string s;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        fclose(f);
        try {
            auto j = nlohmann::json::parse(s);
            {
                std::lock_guard<std::mutex> lk(mu);
                pubKey = j.value("publishable_key", "");
                accessToken = j.value("access_token", "");
                refreshToken = j.value("refresh_token", "");
                expiresAt = j.value("expires_at", 0LL);
                if (!refreshToken.empty()) connected.store(true);
            }
            if (j.contains("downloads") && j["downloads"].is_object()) {
                std::lock_guard<std::mutex> lk(dlMu);
                for (auto& [k, v] : j["downloads"].items())
                    if (v.is_string())
                        downloaded[atoll(k.c_str())] = v.get<std::string>();
            }
        } catch (...) {}
    }
    void markDownloaded(long long id, const std::string& fname) {
        {
            std::lock_guard<std::mutex> lk(dlMu);
            downloaded[id] = fname;
        }
        save();
    }
    bool isDownloaded(long long id, const fs::path& modelsDir) {
        std::lock_guard<std::mutex> lk(dlMu);
        auto it = downloaded.find(id);
        if (it == downloaded.end()) return false;
        std::error_code ec;
        return fs::exists(modelsDir / it->second, ec);
    }
};
static T3KState t3k;

static const char* T3K_REDIRECT = "http://127.0.0.1:53682/callback";
static const wchar_t* T3K_HOST = L"www.tone3000.com";

// vernieuw het access token als het (bijna) verlopen is
static bool t3kEnsureToken(std::string& err) {
    std::string refresh, key;
    {
        std::lock_guard<std::mutex> lk(t3k.mu);
        long long now = (long long)time(nullptr);
        if (!t3k.accessToken.empty() && now < t3k.expiresAt - 60) return true;
        refresh = t3k.refreshToken;
        key = t3k.pubKey;
    }
    if (refresh.empty()) { err = "niet verbonden"; return false; }
    std::string body = "grant_type=refresh_token&refresh_token=" +
                       formEncode(refresh) + "&client_id=" + formEncode(key);
    std::string resp;
    if (!httpRequestEx(T3K_HOST, L"/api/v1/oauth/token", L"POST", body, "",
                       resp, err))
        return false;
    try {
        auto j = nlohmann::json::parse(resp);
        std::lock_guard<std::mutex> lk(t3k.mu);
        t3k.accessToken = j.value("access_token", "");
        if (j.contains("refresh_token"))
            t3k.refreshToken = j.value("refresh_token", "");
        t3k.expiresAt =
            (long long)time(nullptr) + j.value("expires_in", 3600LL);
    } catch (...) {
        err = "token-antwoord onleesbaar";
        return false;
    }
    t3k.save();
    return true;
}

// browser-login: PKCE + loopback-listener op 127.0.0.1:53682
static void t3kConnectAsync(std::string pubKey) {
    if (t3k.busy.exchange(true)) return;
    std::thread([pubKey] {
        {
            std::lock_guard<std::mutex> lk(t3k.mu);
            t3k.pubKey = pubKey;
        }
        g.setNetStatus("wacht op login in de browser...");
        std::string verifier = randomB64url(32);
        uint8_t digest[32];
        Sha256::hash((const uint8_t*)verifier.data(), verifier.size(), digest);
        std::string challenge = b64url(digest, 32);
        std::string state = randomB64url(16);

        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(53682);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOL yes = TRUE;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes,
                   sizeof(yes));
        if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 ||
            listen(ls, 1) != 0) {
            g.setNetStatus("poort 53682 bezet - login niet mogelijk");
            closesocket(ls);
            t3k.busy.store(false);
            return;
        }

        std::string url =
            "https://www.tone3000.com/api/v1/oauth/authorize?client_id=" +
            formEncode(pubKey) + "&redirect_uri=" + formEncode(T3K_REDIRECT) +
            "&response_type=code&code_challenge=" + challenge +
            "&code_challenge_method=S256&state=" + state;
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);

        // wacht max 3 minuten op de redirect
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ls, &fds);
        timeval tv{180, 0};
        std::string code, retState;
        if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
            SOCKET cs = accept(ls, nullptr, nullptr);
            if (cs != INVALID_SOCKET) {
                char buf[4096];
                int n = recv(cs, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = 0;
                    std::string req(buf);
                    size_t q = req.find("GET ");
                    size_t sp = req.find(' ', 4);
                    if (q == 0 && sp != std::string::npos) {
                        std::string target = req.substr(4, sp - 4);
                        auto getParam = [&](const char* k) -> std::string {
                            std::string kk = std::string(k) + "=";
                            size_t p = target.find(kk);
                            if (p == std::string::npos) return "";
                            p += kk.size();
                            size_t e = target.find('&', p);
                            return target.substr(p, e == std::string::npos
                                                        ? std::string::npos
                                                        : e - p);
                        };
                        code = getParam("code");
                        retState = getParam("state");
                    }
                }
                const char* resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                    "<html><body style='font-family:sans-serif;background:"
                    "#111;color:#eee;text-align:center;padding-top:80px'>"
                    "<h2>Gelukt!</h2><p>Je kunt terug naar Guitar Audio Lab."
                    "</p></body></html>";
                send(cs, resp, (int)strlen(resp), 0);
                closesocket(cs);
            }
        }
        closesocket(ls);
        WSACleanup();

        if (code.empty() || retState != state) {
            g.setNetStatus(code.empty() ? "login geannuleerd of verlopen"
                                        : "state-controle mislukt");
            t3k.busy.store(false);
            return;
        }
        std::string body =
            "grant_type=authorization_code&code=" + formEncode(code) +
            "&code_verifier=" + verifier +
            "&redirect_uri=" + formEncode(T3K_REDIRECT) +
            "&client_id=" + formEncode(pubKey);
        std::string resp, err;
        if (httpRequestEx(T3K_HOST, L"/api/v1/oauth/token", L"POST", body, "",
                          resp, err)) {
            try {
                auto j = nlohmann::json::parse(resp);
                {
                    std::lock_guard<std::mutex> lk(t3k.mu);
                    t3k.accessToken = j.value("access_token", "");
                    t3k.refreshToken = j.value("refresh_token", "");
                    t3k.expiresAt = (long long)time(nullptr) +
                                    j.value("expires_in", 3600LL);
                }
                t3k.save();
                t3k.connected.store(true);
                g.setNetStatus("verbonden met TONE3000");
            } catch (...) {
                g.setNetStatus("token-antwoord onleesbaar");
            }
        } else {
            g.setNetStatus("token ophalen mislukt: " + err);
        }
        t3k.busy.store(false);
    }).detach();
}

// defensief een tonenlijst uit het JSON-antwoord halen
static void t3kParseResults(const std::string& body) {
    std::vector<T3KResult> out;
    try {
        auto j = nlohmann::json::parse(body);
        const nlohmann::json* arr = nullptr;
        for (const char* k : {"tones", "data", "items", "results"})
            if (j.contains(k) && j[k].is_array()) { arr = &j[k]; break; }
        if (!arr && j.is_array()) arr = &j;
        if (arr) {
            for (auto& t : *arr) {
                // per tone een eigen try: Ã©Ã©n kapot record mag de rest
                // niet meeslepen (velden kunnen null zijn)
                try {
                T3KResult r;
                if (t.contains("id") && t["id"].is_number())
                    r.toneId = t["id"].get<long long>();
                auto strF = [&t](const char* k) -> std::string {
                    return t.contains(k) && t[k].is_string()
                               ? t[k].get<std::string>()
                               : "";
                };
                r.title = strF("title");
                if (t.contains("downloads_count") &&
                    t["downloads_count"].is_number())
                    r.downloads = t["downloads_count"].get<long long>();
                if (t.contains("favorites_count") &&
                    t["favorites_count"].is_number())
                    r.favorites = t["favorites_count"].get<long long>();
                if (t.contains("user") && t["user"].is_object() &&
                    t["user"].contains("username") &&
                    t["user"]["username"].is_string())
                    r.creator = t["user"]["username"].get<std::string>();
                if (r.creator.empty()) r.creator = strF("username");
                auto joinArr = [](const nlohmann::json& arr) {
                    std::string out;
                    for (auto& m : arr) {
                        std::string name =
                            m.is_object() ? m.value("name", "")
                                          : (m.is_string()
                                                 ? m.get<std::string>()
                                                 : "");
                        if (!name.empty()) {
                            if (!out.empty()) out += ", ";
                            out += name;
                        }
                    }
                    return out;
                };
                if (t.contains("makes") && t["makes"].is_array())
                    r.makes = joinArr(t["makes"]);
                if (t.contains("tags") && t["tags"].is_array())
                    r.tags = joinArr(t["tags"]);
                if (!r.title.empty()) out.push_back(std::move(r));
                } catch (...) {}
            }
        }
    } catch (...) {}
    {
        std::lock_guard<std::mutex> lk(t3k.resMu);
        t3k.results = std::move(out);
    }
}

static void t3kSearchAsync(std::string query, std::string sort,
                           std::string format = "nam", int page = 1) {
    if (t3k.busy.exchange(true)) return;
    std::thread([query, sort, format, page] {
        std::string err;
        if (!t3kEnsureToken(err)) {
            g.setNetStatus("TONE3000: " + err);
            t3k.busy.store(false);
            return;
        }
        g.setNetStatus("TONE3000: zoeken...");
        std::string bearer;
        {
            std::lock_guard<std::mutex> lk(t3k.mu);
            bearer = t3k.accessToken;
        }
        std::string q = "/api/v1/tones/search?format=" + formEncode(format) +
                        "&sort=" + formEncode(sort) +
                        "&page=" + std::to_string(page) + "&page_size=25";
        if (!query.empty()) q += "&query=" + formEncode(query);
        std::wstring wq(q.begin(), q.end());
        std::string resp;
        if (httpRequestEx(T3K_HOST, wq, L"GET", "", bearer, resp, err)) {
            t3kParseResults(resp);
            try {
                auto j = nlohmann::json::parse(resp);
                if (j.contains("page") && j["page"].is_number())
                    t3k.curPage.store(j["page"].get<int>());
                if (j.contains("total_pages") &&
                    j["total_pages"].is_number())
                    t3k.totalPages.store(j["total_pages"].get<int>());
                if (j.contains("total") && j["total"].is_number())
                    t3k.totalTones.store(j["total"].get<long long>());
            } catch (...) {}
            size_t n;
            {
                std::lock_guard<std::mutex> lk(t3k.resMu);
                n = t3k.results.size();
            }
            g.setNetStatus(
                "TONE3000: " + std::to_string(n) + " resultaten (pagina " +
                std::to_string(t3k.curPage.load()) + "/" +
                std::to_string(t3k.totalPages.load()) + ", " +
                std::to_string(t3k.totalTones.load()) + " totaal)");
        } else {
            g.setNetStatus("TONE3000: zoeken mislukt: " + err);
        }
        t3k.busy.store(false);
    }).detach();
}

static std::string sanitizeFilename(std::string s) {
    for (auto& c : s)
        if (strchr("\\/:*?\"<>|", c)) c = '_';
    return s;
}

static void t3kDownloadAsync(long long toneId, std::string title,
                             fs::path destDir, std::string makes = "",
                             std::string tags = "", std::string creator = "",
                             bool isIr = false) {
    if (g.downloading.exchange(true)) return;
    std::thread([toneId, title, destDir, makes, tags, creator, isIr] {
        std::string err;
        if (!t3kEnsureToken(err)) {
            g.setNetStatus("TONE3000: " + err);
            g.downloading.store(false);
            return;
        }
        std::string bearer;
        {
            std::lock_guard<std::mutex> lk(t3k.mu);
            bearer = t3k.accessToken;
        }
        g.setNetStatus("TONE3000: modellen opvragen...");
        std::string resp;
        std::wstring q = L"/api/v1/models?tone_id=" +
                         std::to_wstring(toneId);
        if (!httpRequestEx(T3K_HOST, q, L"GET", "", bearer, resp, err)) {
            g.setNetStatus("TONE3000: modellen opvragen mislukt: " + err);
            g.downloading.store(false);
            return;
        }
        std::string modelUrl, modelName;
        try {
            auto j = nlohmann::json::parse(resp);
            const nlohmann::json* arr = nullptr;
            for (const char* k : {"models", "data", "items"})
                if (j.contains(k) && j[k].is_array()) { arr = &j[k]; break; }
            if (!arr && j.is_array()) arr = &j;
            // velden kunnen null zijn (bv. "size" bij IR's) -> nooit value()
            auto strField = [](const nlohmann::json& o,
                              const char* k) -> std::string {
                return o.contains(k) && o[k].is_string()
                           ? o[k].get<std::string>()
                           : "";
            };
            if (arr && !arr->empty()) {
                // voorkeur: "standard"-formaat, anders het eerste bestand
                for (auto& m : *arr) {
                    if (strField(m, "size") == "standard") {
                        modelUrl = strField(m, "model_url");
                        modelName = strField(m, "name");
                        break;
                    }
                }
                if (modelUrl.empty()) {
                    modelUrl = strField((*arr)[0], "model_url");
                    modelName = strField((*arr)[0], "name");
                }
            }
        } catch (...) {}
        if (modelUrl.empty()) {
            g.setNetStatus("TONE3000: geen downloadbaar model gevonden");
            g.downloading.store(false);
            return;
        }
        g.setNetStatus("TONE3000: downloaden...");
        std::wstring host, path;
        std::string body;
        if (!splitUrl(modelUrl, host, path) ||
            !httpRequestEx(host, path, L"GET", "", bearer, body, err)) {
            g.setNetStatus("TONE3000: download mislukt: " + err);
            g.downloading.store(false);
            return;
        }
        std::string fname = sanitizeFilename(
            title + (modelName.empty() ? "" : " - " + modelName));
        const char* ext = isIr ? ".wav" : ".nam";
        if (fname.size() < 4 ||
            lowerStr(fname.substr(fname.size() - 4)) != ext)
            fname += ext;
        fs::path dest = destDir / fname;
        // titels zijn niet uniek: bestaat het bestand al (andere tone met
        // dezelfde naam), maak de naam uniek met het tone-ID
        {
            std::error_code ec;
            bool ownFile = false;
            {
                std::lock_guard<std::mutex> lk(t3k.dlMu);
                auto it = t3k.downloaded.find(toneId);
                ownFile = it != t3k.downloaded.end() && it->second == fname;
            }
            if (fs::exists(dest, ec) && !ownFile) {
                fname = fname.substr(0, fname.size() - 4) + " (t3k " +
                        std::to_string(toneId) + ")" + ext;
                dest = destDir / fname;
            }
        }
        FILE* f = _wfopen(dest.wstring().c_str(), L"wb");
        if (f) {
            fwrite(body.data(), 1, body.size(), f);
            fclose(f);
            g.modelsChanged.store(true);
            t3k.markDownloaded(toneId, fname);
            // metadata uit de API meteen meegeven aan de bibliotheek
            if (!isIr) {
                ModelMeta mm;
                mm.make = makes;
                mm.maker = creator;
                mm.source = "TONE3000";
                std::string cur;
                for (char c : tags) {
                    if (c == ',') {
                        if (!cur.empty()) mm.tags.push_back(cur);
                        cur.clear();
                    } else if (c != ' ' || !cur.empty()) {
                        cur += c;
                    }
                }
                if (!cur.empty()) mm.tags.push_back(cur);
                std::lock_guard<std::mutex> lk(gMetaPendMutex);
                gMetaPending[fname] = std::move(mm);
            }
            g.setNetStatus("TONE3000: gedownload: " + fname);
        } else {
            g.setNetStatus("TONE3000: opslaan mislukt");
        }
        g.downloading.store(false);
    }).detach();
}

// haalt voor alle geregistreerde TONE3000-downloads de metadata op
static void t3kFetchMetaAsync() {
    if (t3k.busy.exchange(true)) return;
    std::thread([] {
        std::string err;
        if (!t3kEnsureToken(err)) {
            g.setNetStatus("TONE3000: " + err);
            t3k.busy.store(false);
            return;
        }
        std::string bearer;
        {
            std::lock_guard<std::mutex> lk(t3k.mu);
            bearer = t3k.accessToken;
        }
        std::map<long long, std::string> dl;
        {
            std::lock_guard<std::mutex> lk(t3k.dlMu);
            dl = t3k.downloaded;
        }
        int done = 0;
        for (auto& [id, fname] : dl) {
            g.setNetStatus("metadata ophalen " + std::to_string(++done) +
                           "/" + std::to_string(dl.size()));
            std::wstring q = L"/api/v1/tones/" + std::to_wstring(id);
            std::string resp;
            if (!httpRequestEx(T3K_HOST, q, L"GET", "", bearer, resp, err))
                continue;
            try {
                auto j = nlohmann::json::parse(resp);
                if (j.contains("tone") && j["tone"].is_object())
                    j = j["tone"];
                ModelMeta mm;
                mm.source = "TONE3000";
                auto joinArr = [](const nlohmann::json& arr,
                                  std::vector<std::string>* vec) {
                    std::string out;
                    for (auto& m : arr) {
                        std::string name =
                            m.is_object() ? m.value("name", "")
                                          : (m.is_string()
                                                 ? m.get<std::string>()
                                                 : "");
                        if (!name.empty()) {
                            if (!out.empty()) out += ", ";
                            out += name;
                            if (vec) vec->push_back(name);
                        }
                    }
                    return out;
                };
                if (j.contains("makes") && j["makes"].is_array())
                    mm.make = joinArr(j["makes"], nullptr);
                if (j.contains("tags") && j["tags"].is_array())
                    joinArr(j["tags"], &mm.tags);
                if (j.contains("user") && j["user"].is_object())
                    mm.maker = j["user"].value("username", "");
                std::lock_guard<std::mutex> lk(gMetaPendMutex);
                gMetaPending[fname] = std::move(mm);
            } catch (...) {}
        }
        g.setNetStatus("metadata bijgewerkt voor " + std::to_string(done) +
                       " modellen");
        t3k.busy.store(false);
    }).detach();
}

// ============================================================= audiospeler
// Decodeert een audiobestand (mp3/wav/flac/m4a/wma) naar 48 kHz stereo float
// via Media Foundation. Als de reader geen 48 kHz wil leveren, resampelen we
// zelf (Hermite).
static bool decodeAudioFile(const std::wstring& path, std::vector<float>& out,
                            size_t& outFrames, std::string& err) {
    IMFSourceReader* rd = nullptr;
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 1);
    if (attr)  // staat o.a. de resampler naar 48 kHz toe
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attr, &rd);
    if (attr) attr->Release();
    if (FAILED(hr)) {
        err = "bestand openen mislukt";
        return false;
    }
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    IMFMediaType* want = nullptr;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, SR);
    want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                 nullptr, want);
    want->Release();
    if (FAILED(hr)) {  // val terug op float in het bronformaat
        IMFMediaType* w2 = nullptr;
        MFCreateMediaType(&w2);
        w2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        w2->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        hr = rd->SetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, w2);
        w2->Release();
        if (FAILED(hr)) {
            err = "formaat niet ondersteund";
            rd->Release();
            return false;
        }
    }
    UINT32 rate = SR, ch = 2;
    IMFMediaType* actual = nullptr;
    if (SUCCEEDED(rd->GetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual))) {
        actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        actual->Release();
    }
    if (ch == 0) ch = 2;

    std::vector<float> raw;  // interleaved in bronformaat
    for (;;) {
        DWORD flags = 0;
        IMFSample* smp = nullptr;
        hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                            nullptr, &flags, nullptr, &smp);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (smp) smp->Release();
            break;
        }
        if (!smp) continue;
        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED(smp->ConvertToContiguousBuffer(&buf))) {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                size_t n = len / sizeof(float);
                const float* f = (const float*)p;
                raw.insert(raw.end(), f, f + n);
                buf->Unlock();
            }
            buf->Release();
        }
        smp->Release();
    }
    rd->Release();
    if (raw.empty()) {
        err = "geen audio gedecodeerd";
        return false;
    }

    // naar stereo
    size_t inFrames = raw.size() / ch;
    std::vector<float> st(inFrames * 2);
    for (size_t i = 0; i < inFrames; i++) {
        if (ch == 1) {
            st[i * 2] = st[i * 2 + 1] = raw[i];
        } else {
            st[i * 2] = raw[i * ch];
            st[i * 2 + 1] = raw[i * ch + 1];
        }
    }
    // naar 48 kHz (Hermite-interpolatie) indien nodig
    if (rate != SR) {
        double ratio = (double)rate / SR;
        size_t oFrames = (size_t)(inFrames / ratio);
        out.resize(oFrames * 2);
        for (size_t i = 0; i < oFrames; i++) {
            double srcPos = i * ratio;
            size_t i1 = (size_t)srcPos;
            double t = srcPos - i1;
            for (int c = 0; c < 2; c++) {
                auto S = [&](size_t idx) -> float {
                    if (idx >= inFrames) idx = inFrames - 1;
                    return st[idx * 2 + c];
                };
                float y0 = S(i1 > 0 ? i1 - 1 : 0), y1 = S(i1), y2 = S(i1 + 1),
                      y3 = S(i1 + 2);
                float c1 = 0.5f * (y2 - y0);
                float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                out[i * 2 + c] =
                    (float)(((c3 * t + c2) * t + c1) * t + y1);
            }
        }
        outFrames = oFrames;
    } else {
        out = std::move(st);
        outFrames = inFrames;
    }
    return true;
}

static void loadTrackAsync(const std::wstring& path) {
    if (g.trackLoading.exchange(true)) return;
    std::thread([path] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto t = std::make_unique<PlayerTrack>();
        std::string err;
        if (decodeAudioFile(path, t->pcm, t->frames, err)) {
            t->name = fs::path(path).filename().string();
            g.setTrackName(t->name);
            g.playerLenFr.store((long long)t->frames);
            g.playerPosFr.store(0);
            PlayerTrack* old = g.pendingTrack.exchange(t.release());
            if (old) delete old;
        } else {
            g.setError("nummer laden mislukt: " + err);
        }
        CoUninitialize();
        g.trackLoading.store(false);
    }).detach();
}

// laadt een cabinet-IR (of schakelt uit met leeg pad) en biedt hem de
// engine aan; werkt ook als de engine (nog) niet draait
static void loadIrAsync(const std::wstring& path) {
    if (g.irLoading.exchange(true)) return;
    std::thread([path] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto ir = std::make_unique<IrData>();
        bool ok = true;
        if (!path.empty()) {
            std::vector<float> pcm;
            size_t frames = 0;
            std::string err;
            if (decodeAudioFile(path, pcm, frames, err) && frames > 0) {
                size_t n = frames < 4096 ? frames : 4096;
                std::vector<float> mono(n);
                double energy = 0.0;
                for (size_t i = 0; i < n; i++) {
                    mono[i] = 0.5f * (pcm[i * 2] + pcm[i * 2 + 1]);
                    energy += (double)mono[i] * mono[i];
                }
                if (energy > 1e-12) {
                    float scale = (float)(1.0 / sqrt(energy));
                    for (auto& v : mono) v *= scale;
                }
                ir->taps.assign(mono.rbegin(), mono.rend());  // omgekeerd
                ir->name = fs::path(path).filename().string();
            } else {
                g.setError("IR laden mislukt: " + err);
                ok = false;
            }
        }
        if (ok) {
            IrData* old = g.pendingIr.exchange(ir.release());
            if (old) delete old;
        }
        CoUninitialize();
        g.irLoading.store(false);
    }).detach();
}

static std::wstring openAudioFileDialog(HWND owner) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&dlg)))) {
        COMDLG_FILTERSPEC specs[] = {
            {L"Audio", L"*.mp3;*.wav;*.flac;*.m4a;*.wma;*.aac"},
            {L"Alle bestanden", L"*.*"}};
        dlg->SetFileTypes(2, specs);
        dlg->SetTitle(L"Kies een nummer om mee te spelen");
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    result = p;
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

// ============================================================= stemmer
// YIN-pitchdetectie (De CheveignÃ© & Kawahara) op een venster rauwe samples.
// Retourneert frequentie in Hz, of 0 als er geen betrouwbare pitch is.
static float detectPitch(const float* x, int n) {
    const int W = 1024;          // correlatievenster
    const int maxTau = 1024;     // ~47 Hz ondergrens (laag genoeg voor drop-B)
    const int minTau = 40;       // ~1200 Hz bovengrens
    if (n < W + maxTau) return 0.0f;
    // ruis-gate
    double rms = 0.0;
    for (int i = 0; i < W; i++) rms += (double)x[i] * x[i];
    rms = sqrt(rms / W);
    if (rms < 0.002) return 0.0f;  // ~-54 dBFS

    static float d[1025];
    d[0] = 1.0f;
    double cum = 0.0;
    int best = 0;
    for (int tau = 1; tau <= maxTau; tau++) {
        double sum = 0.0;
        for (int i = 0; i < W; i++) {
            double diff = (double)x[i] - x[i + tau];
            sum += diff * diff;
        }
        cum += sum;
        d[tau] = cum > 0.0 ? (float)(sum * tau / cum) : 1.0f;
    }
    for (int tau = minTau; tau <= maxTau - 1; tau++) {
        if (d[tau] < 0.12f && d[tau] <= d[tau + 1]) {  // eerste lokale dip
            best = tau;
            break;
        }
    }
    if (!best) return 0.0f;
    // parabolische verfijning
    float t = (float)best;
    if (best > 1 && best < maxTau) {
        float a = d[best - 1], b = d[best], c = d[best + 1];
        float denom = a - 2.0f * b + c;
        if (fabsf(denom) > 1e-9f) t += 0.5f * (a - c) / denom;
    }
    return (float)SR / t;
}

// Laadt een model op een achtergrondthread en biedt het de draaiende engine
// aan via pendingModel â€” de audio speelt intussen gewoon door.
static void loadModelAsync(const std::wstring& path) {
    if (g.modelLoading.exchange(true)) return;  // al bezig met laden
    std::thread([path] {
        try {
            nam::activations::Activation::enable_fast_tanh();
            auto m = nam::get_dsp(fs::path(path));
            if (m && m->NumInputChannels() == 1 &&
                m->NumOutputChannels() == 1) {
                m->Reset(SR, 2048);  // incl. prewarm, vÃ³Ã³r de overdracht
                nam::DSP* old = g.pendingModel.exchange(m.release());
                if (old) delete old;  // vorige wissel nooit opgepikt
                g.setError("");
            } else {
                g.setError("model laden mislukt of geen mono model");
            }
        } catch (const std::exception& e) {
            g.setError(std::string("model laden mislukt: ") + e.what());
        }
        g.modelLoading.store(false);
    }).detach();
}

// ============================================================= modellen
// ============================================================= outputdevices
struct OutDevice {
    std::wstring id;
    std::string name;
};

// alle actieve render-endpoints; Hands-Free-profielen (bel-audio, 8-16 kHz)
// worden overgeslagen
static std::vector<OutDevice> enumRenderDevices() {
    std::vector<OutDevice> out;
    IMMDeviceEnumerator* enu = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&enu)))
        return out;
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                          &coll))) {
        UINT n = 0;
        coll->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev))) continue;
            LPWSTR id = nullptr;
            IPropertyStore* store = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) &&
                SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName,
                                              &pv)) &&
                    pv.vt == VT_LPWSTR) {
                    std::wstring wname = pv.pwszVal;
                    if (wname.find(L"Hands-Free") == std::wstring::npos &&
                        wname.find(L"hands-free") == std::wstring::npos) {
                        int len = WideCharToMultiByte(CP_UTF8, 0,
                                                      wname.c_str(), -1,
                                                      nullptr, 0, nullptr,
                                                      nullptr);
                        std::string u8(len > 0 ? len - 1 : 0, 0);
                        if (len > 0)
                            WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1,
                                                u8.data(), len, nullptr,
                                                nullptr);
                        out.push_back({id, u8});
                    }
                }
                PropVariantClear(&pv);
                store->Release();
            }
            if (id) CoTaskMemFree(id);
            dev->Release();
        }
        coll->Release();
    }
    enu->Release();
    return out;
}

// categorie-indeling op merknaam-trefwoorden; laatste = "Overig"
static const struct {
    const char* name;
    const char* keywords[8];
} MODEL_CATS[] = {
    {"Fender", {"fender", "twin", "deluxe rev", "bassman", "princeton",
                "tweed", nullptr}},
    {"Marshall", {"marshall", "jcm", "jmp", "plexi", "1959", "jubilee",
                  nullptr}},
    {"Vox", {"vox", "ac15", "ac30", nullptr}},
    {"Peavey/EVH", {"5150", "6505", "6534", "5153", "evh", "blockletter",
                    nullptr}},
    {"Mesa", {"mesa", "boogie", "rectifier", "mark ii", nullptr}},
    {"High-gain", {"engl", "diezel", "soldano", "slo", "friedman", "revv",
                   "bogner", nullptr}},
    {"Boutique", {"ceriatone", "matchless", "two-rock", "dumble",
                  "tone king", "bad cat", "king kong", nullptr}},
    {"Pedalen", {"pedal", "od808", "ts9", "screamer", "klon", "mxr",
                 "drive", nullptr}},
    {"Bas", {"bass", "ampeg", "darkglass", nullptr}},
};
static const int NUM_CATS = (int)(sizeof(MODEL_CATS) / sizeof(MODEL_CATS[0]));
// categorie-index NUM_CATS = "Overig"

static int categorize(const std::string& name) {
    std::string low = name;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    for (int i = 0; i < NUM_CATS; i++)
        for (int k = 0; MODEL_CATS[i].keywords[k]; k++)
            if (low.find(MODEL_CATS[i].keywords[k]) != std::string::npos)
                return i;
    return NUM_CATS;  // Overig
}

struct ModelEntry {
    std::wstring path;
    std::string label;  // UTF-8 voor ImGui
    int cat = 0;
    long long mtime = 0;  // bestandsdatum = downloadmoment
};

static std::vector<ModelEntry> scanModels(const fs::path& dir) {
    std::vector<ModelEntry> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == L".nam") {
            std::string u8 = e.path().filename().string();
            std::error_code ec2;
            auto ft = fs::last_write_time(e.path(), ec2);
            long long mt =
                ec2 ? 0 : (long long)ft.time_since_epoch().count();
            out.push_back({e.path().wstring(), u8, categorize(u8), mt});
        }
    }
    return out;
}

// vult alleen lege metadata-velden aan met heuristiek op de bestandsnaam
static void deriveMeta(const std::string& fname, int cat, ModelMeta& m) {
    std::string low = lowerStr(fname);
    auto has = [&](const char* k) { return low.find(k) != std::string::npos; };
    if (m.make.empty())
        m.make = cat < NUM_CATS ? MODEL_CATS[cat].name : "";
    if (m.style.empty()) {
        if (has("clean")) m.style = "Clean";
        else if (has("crunch") || has("breakup") || has("edge"))
            m.style = "Crunch";
        else if (has("high gain") || has("metal") || has("boost") ||
                 has("lead") || has("solo") || has("5150") || has("6505") ||
                 has("rectifier") || has("uber"))
            m.style = "High-gain";
    }
    if (m.mic.empty()) {
        static const struct { const char* k; const char* v; } MICS[] = {
            {"sm57", "SM57"},   {"sm7b", "SM7B"},  {"r121", "R121"},
            {"r-121", "R121"},  {"md421", "MD421"}, {"md-421", "MD421"},
            {"414", "AKG 414"}, {"e906", "e906"},  {"u87", "U87"},
            {"royer", "Royer"}};
        for (auto& mc : MICS)
            if (has(mc.k)) {
                if (!m.mic.empty()) m.mic += " + ";
                m.mic += mc.v;
            }
    }
    if (m.cab.empty()) {
        // NxM-patroon (4x12, 2x12, 4x10, 1x12, ...)
        for (size_t i = 0; i + 2 < low.size(); i++) {
            if (isdigit((unsigned char)low[i]) && low[i + 1] == 'x' &&
                isdigit((unsigned char)low[i + 2])) {
                size_t e = i + 3;
                if (e < low.size() && isdigit((unsigned char)low[e])) e++;
                m.cab = fname.substr(i, e - i);
                break;
            }
        }
    }
    if (m.maker.empty()) {
        size_t sp1 = fname.find(' ');
        if (sp1 != std::string::npos) {
            size_t sp2 = fname.find(' ', sp1 + 1);
            if (sp2 != std::string::npos && sp2 - sp1 == 2)
                m.maker = fname.substr(0, sp2);
        }
    }
}

// ============================================================= stijl
static ImFont* gFont = nullptr;
static ImFont* gFontBold = nullptr;
static ID3D11ShaderResourceView* gLogoSRV = nullptr;

// PNG -> D3D11-texture via WIC (voor het zijbalk-logo)
static ID3D11ShaderResourceView* loadPngTexture(ID3D11Device* dev,
                                                const wchar_t* path) {
    ID3D11ShaderResourceView* srv = nullptr;
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac))))
        return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(fac->CreateDecoderFromFilename(
            path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
            &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) &&
        SUCCEEDED(fac->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        std::vector<uint8_t> px((size_t)w * h * 4);
        if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(),
                                       px.data()))) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex))) {
                dev->CreateShaderResourceView(tex, nullptr, &srv);
                tex->Release();
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    fac->Release();
    return srv;
}

// klein grijs kopje in halfvet, voor paneel-headers
static void sectionHeader(const char* label) {
    if (gFontBold) ImGui::PushFont(gFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.55f, 0.65f, 1.0f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (gFontBold) ImGui::PopFont();
    ImGui::Spacing();
}

static void applyModernStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.TabRounding = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowPadding = ImVec2(14, 14);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.CellPadding = ImVec2(8, 5);
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 12.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    auto rgb = [](int r, int g, int b, float a = 1.0f) {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
    };
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = rgb(13, 15, 19);
    c[ImGuiCol_ChildBg] = rgb(21, 24, 31);
    c[ImGuiCol_PopupBg] = rgb(24, 27, 35);
    c[ImGuiCol_Border] = rgb(41, 47, 60);
    c[ImGuiCol_FrameBg] = rgb(30, 35, 45);
    c[ImGuiCol_FrameBgHovered] = rgb(37, 43, 56);
    c[ImGuiCol_FrameBgActive] = rgb(44, 52, 68);
    c[ImGuiCol_Text] = rgb(224, 228, 236);
    c[ImGuiCol_TextDisabled] = rgb(118, 127, 143);
    c[ImGuiCol_Button] = rgb(38, 45, 58);
    c[ImGuiCol_ButtonHovered] = rgb(48, 57, 74);
    c[ImGuiCol_ButtonActive] = rgb(58, 70, 92);
    c[ImGuiCol_Header] = rgb(58, 110, 216, 0.32f);
    c[ImGuiCol_HeaderHovered] = rgb(58, 110, 216, 0.42f);
    c[ImGuiCol_HeaderActive] = rgb(58, 110, 216, 0.55f);
    c[ImGuiCol_SliderGrab] = rgb(86, 140, 235);
    c[ImGuiCol_SliderGrabActive] = rgb(110, 160, 248);
    c[ImGuiCol_CheckMark] = rgb(110, 160, 248);
    c[ImGuiCol_Tab] = rgb(24, 28, 36);
    c[ImGuiCol_TabHovered] = rgb(44, 52, 68);
    c[ImGuiCol_TabActive] = rgb(38, 46, 60);
    c[ImGuiCol_TabUnfocused] = rgb(24, 28, 36);
    c[ImGuiCol_TabUnfocusedActive] = rgb(38, 46, 60);
    c[ImGuiCol_TableHeaderBg] = rgb(28, 33, 43);
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.02f);
    c[ImGuiCol_TableBorderLight] = rgb(41, 47, 60);
    c[ImGuiCol_TableBorderStrong] = rgb(41, 47, 60);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = rgb(50, 58, 74);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(62, 72, 92);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(75, 88, 112);
    c[ImGuiCol_Separator] = rgb(41, 47, 60);
    c[ImGuiCol_PlotHistogram] = rgb(86, 140, 235);
}

// ============================================================= draaiknop
// Vector-getekende knob in de app-stijl: verticaal slepen, scrollwiel,
// dubbelklik = terug naar standaardwaarde.
static bool Knob(const char* id, const char* label, float* v, float vmin,
                 float vmax, float vdef, const char* fmt,
                 const char* minText = nullptr, bool bipolar = false) {
    const float size = 46.0f;
    const float cellW = 76.0f;
    ImGui::BeginGroup();
    ImGui::PushID(id);
    float startX = ImGui::GetCursorPosX();
    float tw = ImGui::CalcTextSize(label).x;
    ImGui::SetCursorPosX(startX + (cellW - tw) * 0.5f);
    ImGui::TextDisabled("%s", label);
    ImGui::SetCursorPosX(startX + (cellW - size) * 0.5f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##k", ImVec2(size, size));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    ImGuiIO& io = ImGui::GetIO();
    if (active && io.MouseDelta.y != 0.0f) {
        *v -= io.MouseDelta.y * (vmax - vmin) / 200.0f;
        changed = true;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        *v += io.MouseWheel * (vmax - vmin) / 60.0f;
        changed = true;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(0)) {
        *v = vdef;
        changed = true;
    }
    if (*v < vmin) *v = vmin;
    if (*v > vmax) *v = vmax;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    float r = size * 0.5f;
    float t = (vmax > vmin) ? (*v - vmin) / (vmax - vmin) : 0.0f;
    const float a0 = 3.14159265f * 0.75f, a1 = 3.14159265f * 2.25f;
    float av = a0 + t * (a1 - a0);
    ImU32 face = ImGui::GetColorU32(hovered || active
                                        ? ImGuiCol_FrameBgHovered
                                        : ImGuiCol_FrameBg);
    ImU32 track = ImGui::GetColorU32(ImGuiCol_Button);
    ImU32 accent = ImGui::GetColorU32(ImGuiCol_SliderGrab);
    dl->AddCircleFilled(c, r - 4.0f, face, 32);
    dl->PathArcTo(c, r - 1.5f, a0, a1, 32);
    dl->PathStroke(track, 0, 3.0f);
    if (bipolar) {
        // boog vanaf het midden (12 uur) naar de waarde — voor +/- dB
        float aMid = (a0 + a1) * 0.5f;
        if (fabsf(t - 0.5f) > 0.004f) {
            dl->PathArcTo(c, r - 1.5f, av < aMid ? av : aMid,
                          av < aMid ? aMid : av, 24);
            dl->PathStroke(accent, 0, 3.0f);
        }
    } else if (t > 0.003f) {
        dl->PathArcTo(c, r - 1.5f, a0, av, 32);
        dl->PathStroke(accent, 0, 3.0f);
    }
    ImVec2 p1(c.x + cosf(av) * r * 0.28f, c.y + sinf(av) * r * 0.28f);
    ImVec2 p2(c.x + cosf(av) * r * 0.72f, c.y + sinf(av) * r * 0.72f);
    dl->AddLine(p1, p2, ImGui::GetColorU32(ImGuiCol_Text), 2.5f);
    char buf[32];
    if (minText && *v <= vmin + 1e-4f)
        snprintf(buf, sizeof(buf), "%s", minText);
    else
        snprintf(buf, sizeof(buf), fmt, *v);
    float vw = ImGui::CalcTextSize(buf).x;
    ImGui::SetCursorPosX(startX + (cellW - vw) * 0.5f);
    ImGui::TextUnformatted(buf);
    ImGui::Dummy(ImVec2(cellW, 0.0f));  // vaste celbreedte
    ImGui::PopID();
    ImGui::EndGroup();
    return changed;
}

// ============================================================= DX11 + ImGui
static ID3D11Device* gDev = nullptr;
static ID3D11DeviceContext* gCtx = nullptr;
static IDXGISwapChain* gSwap = nullptr;
static ID3D11RenderTargetView* gRTV = nullptr;

// ---- amp-artwork per categorie in het detailpaneel (assets\amp_<slug>.png)
static const char* CAT_SLUGS[] = {"fender", "marshall", "vox",   "peavey",
                                  "mesa",   "highgain", "boutique",
                                  "pedal",  "bas"};
static fs::path gAssetsDir;
static ID3D11ShaderResourceView* gCatArt[NUM_CATS + 2] = {};
static bool gCatArtTried[NUM_CATS + 2] = {};

// idx: 0..NUM_CATS-1 = categorie, NUM_CATS = overig, NUM_CATS+1 = cabinet
static ID3D11ShaderResourceView* getCatArt(int idx) {
    if (idx < 0 || idx > NUM_CATS + 1) return nullptr;
    if (!gCatArtTried[idx]) {
        gCatArtTried[idx] = true;
        const char* slug = idx < NUM_CATS
                               ? CAT_SLUGS[idx]
                               : (idx == NUM_CATS ? "overig" : "cabinet");
        fs::path p = gAssetsDir / (std::string("amp_") + slug + ".png");
        std::error_code ec;
        if (fs::exists(p, ec))
            gCatArt[idx] = loadPngTexture(gDev, p.wstring().c_str());
    }
    return gCatArt[idx];
}

static void createRTV() {
    ID3D11Texture2D* back = nullptr;
    gSwap->GetBuffer(0, IID_PPV_ARGS(&back));
    gDev->CreateRenderTargetView(back, nullptr, &gRTV);
    back->Release();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam,
                              LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (gDev && wParam != SIZE_MINIMIZED) {
                if (gRTV) { gRTV->Release(); gRTV = nullptr; }
                gSwap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                     DXGI_FORMAT_UNKNOWN, 0);
                createRTV();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void meter(const char* label, float peak, float width) {
    float db = peak > 0.0f ? 20.0f * log10f(peak) : -60.0f;
    if (db < -60.0f) db = -60.0f;
    float frac = (db + 60.0f) / 60.0f;
    char txt[32];
    snprintf(txt, sizeof(txt), "%.1f dB", db);
    ImVec4 col = db > -6.0f ? ImVec4(0.9f, 0.3f, 0.2f, 1.0f)
                 : db > -18.0f ? ImVec4(0.9f, 0.75f, 0.2f, 1.0f)
                               : ImVec4(0.3f, 0.8f, 0.4f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(frac, ImVec2(width, 0), txt);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR pCmd, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    fs::path exeDir;
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        exeDir = fs::path(buf).parent_path();
    }
    gAssetsDir = exeDir / L"assets";
    fs::path modelsDir = exeDir.parent_path() / L"models";
    std::vector<ModelEntry> models = scanModels(modelsDir);
    int selModel = 0;
    fs::path catalogCache = modelsDir / L"catalog.txt";
    loadCatalogCache(catalogCache);

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst,
                      nullptr, nullptr, nullptr, nullptr,
                      L"GuitarAudioLab", nullptr};
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Guitar Audio Lab",
                              WS_OVERLAPPEDWINDOW, 140, 80, 1240, 880,
                              nullptr, nullptr, hInst, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &sd, &gSwap, &gDev, &fl, &gCtx)))
        return 1;
    createRTV();

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    {
        ImGuiIO& io = ImGui::GetIO();
        gFont = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeui.ttf", 19.0f);
        gFontBold = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeuib.ttf", 19.0f);
        if (!gFont) {  // vangnet als het font ontbreekt
            io.Fonts->AddFontDefault();
            io.FontGlobalScale = 1.25f;
        }
    }
    applyModernStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(gDev, gCtx);
    gLogoSRV = loadPngTexture(gDev, (exeDir / L"logo.png").wstring().c_str());

    float uiPeakIn = 0.0f, uiPeakOut = 0.0f;
    float gainUI = 1.0f, masterUI = 0.5f;
    float trackVolUI = 0.7f;
    float eqBassUI = 0.0f, eqMidUI = 0.0f, eqTrebleUI = 0.0f;
    float gateUI = -90.0f;
    int catSel = -1;     // -1 = alle, -2 = favorieten, -3 = cabinets/IR's
    int browseSel = -1;  // selectie in de bibliotheekbrowser (modellen)
    int irBrowseSel = -1;  // selectie in de IR-weergave
    int delType = 0, delIdx = -1;  // bevestiging verwijderen (1=model, 2=IR)
    char libSearch[128] = "";
    char catFilter[128] = "";
    char t3kKeyBuf[128] = "";
    char t3kQuery[128] = "";
    int t3kSort = 0;
    int t3kFormat = 0;  // 0 = NAM, 1 = IR
    t3k.configFile = modelsDir.parent_path() / L"t3k.json";
    t3k.load();
    {
        std::lock_guard<std::mutex> lk(t3k.mu);
        strncpy_s(t3kKeyBuf, t3k.pubKey.c_str(), _TRUNCATE);
    }

    // cabinet-IR's (map irs\ naast models\)
    fs::path irsDir = modelsDir.parent_path() / L"irs";
    {
        std::error_code ec;
        fs::create_directories(irsDir, ec);
    }
    struct IrEntry {
        std::wstring path;
        std::string label;
        long long mtime = 0;
    };
    auto scanIrs = [&irsDir]() {
        std::vector<IrEntry> out;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(irsDir, ec)) {
            auto ext = e.path().extension();
            if (ext == L".wav" || ext == L".flac") {
                std::error_code ec2;
                auto ft = fs::last_write_time(e.path(), ec2);
                out.push_back({e.path().wstring(),
                               e.path().filename().string(),
                               ec2 ? 0
                                   : (long long)ft.time_since_epoch()
                                         .count()});
            }
        }
        return out;
    };
    std::vector<IrEntry> irs = scanIrs();
    int selIr = -1;       // -1 = geen IR
    int librarySort = 0;  // 0 = naam, 1 = nieuwste eerst

    // outputdevice-keuze + favorieten, persistentie in settings.json
    std::vector<OutDevice> outDevices = enumRenderDevices();
    int selOut = -1;  // -1 = automatisch (Realtek Digital)
    std::set<std::string> favorites;  // bestandsnamen (.nam)
    fs::path settingsFile = modelsDir.parent_path() / L"settings.json";
    {
        FILE* f = _wfopen(settingsFile.wstring().c_str(), L"rb");
        if (f) {
            std::string s;
            char buf[2048];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
            fclose(f);
            try {
                auto j = nlohmann::json::parse(s);
                std::string saved = j.value("output_device_id", "");
                std::wstring wsaved(saved.begin(), saved.end());
                for (int i = 0; i < (int)outDevices.size(); i++)
                    if (outDevices[i].id == wsaved) selOut = i;
                if (j.contains("favorites") && j["favorites"].is_array())
                    for (auto& fv : j["favorites"])
                        if (fv.is_string())
                            favorites.insert(fv.get<std::string>());
                std::string savedIr = j.value("ir", "");
                if (!savedIr.empty())
                    for (int i = 0; i < (int)irs.size(); i++)
                        if (irs[i].label == savedIr) selIr = i;
                librarySort = j.value("library_sort", 0);
            } catch (...) {}
        }
    }
    if (selIr >= 0) loadIrAsync(irs[selIr].path);  // klaarzetten voor start

    // sorteert modellen en IR's volgens librarySort; selecties blijven kloppen
    auto applySort = [&] {
        std::wstring keepM = (selModel >= 0 && selModel < (int)models.size())
                                 ? models[selModel].path
                                 : L"";
        std::string keepI = (selIr >= 0 && selIr < (int)irs.size())
                                ? irs[selIr].label
                                : "";
        if (librarySort == 1) {
            std::sort(models.begin(), models.end(),
                      [](const ModelEntry& a, const ModelEntry& b) {
                          return a.mtime > b.mtime;
                      });
            std::sort(irs.begin(), irs.end(),
                      [](const IrEntry& a, const IrEntry& b) {
                          return a.mtime > b.mtime;
                      });
        } else {
            std::sort(models.begin(), models.end(),
                      [](const ModelEntry& a, const ModelEntry& b) {
                          return lowerStr(a.label) < lowerStr(b.label);
                      });
            std::sort(irs.begin(), irs.end(),
                      [](const IrEntry& a, const IrEntry& b) {
                          return lowerStr(a.label) < lowerStr(b.label);
                      });
        }
        if (!keepM.empty()) {
            selModel = 0;
            for (int i = 0; i < (int)models.size(); i++)
                if (models[i].path == keepM) selModel = i;
        }
        if (!keepI.empty()) {
            selIr = -1;
            for (int i = 0; i < (int)irs.size(); i++)
                if (irs[i].label == keepI) selIr = i;
        }
        browseSel = -1;
        irBrowseSel = -1;
    };
    applySort();

    auto saveSettings = [&] {
        std::string id;
        if (selOut >= 0) {
            const std::wstring& w = outDevices[selOut].id;
            id.assign(w.begin(), w.end());  // device-IDs zijn ASCII
        }
        nlohmann::json favs = nlohmann::json::array();
        for (auto& fv : favorites) favs.push_back(fv);
        nlohmann::json j = {{"output_device_id", id},
                            {"favorites", favs},
                            {"ir", selIr >= 0 ? irs[selIr].label : ""},
                            {"library_sort", librarySort}};
        FILE* f = _wfopen(settingsFile.wstring().c_str(), L"wb");
        if (f) {
            std::string s = j.dump(2);
            fwrite(s.data(), 1, s.size(), f);
            fclose(f);
        }
    };
    auto currentOutId = [&]() -> std::wstring {
        return selOut >= 0 ? outDevices[selOut].id : L"";
    };

    // apparaatvolume (IAudioEndpointVolume) van het gekozen outputdevice;
    // bij USB/analoge devices is dit hardwarevolume en werkt het dus ook
    // terwijl de engine exclusive speelt
    IAudioEndpointVolume* epVol = nullptr;
    std::wstring epVolId = L"\xFFFF";  // ongeldig, forceert eerste refresh
    float epVolUI = -1.0f;
    bool epVolHw = false;
    int epVolPoll = 0;
    auto ensureEpVol = [&](const std::wstring& id) {
        if (id == epVolId && epVol) return;
        if (epVol) {
            epVol->Release();
            epVol = nullptr;
        }
        epVolId = id;
        epVolUI = -1.0f;
        epVolHw = false;
        IMMDeviceEnumerator* enu = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                       CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator),
                                       (void**)&enu))) {
            IMMDevice* dev = nullptr;
            if (!id.empty()) enu->GetDevice(id.c_str(), &dev);
            else dev = findDevice(enu, eRender, L"realtek digital");
            if (dev) {
                dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                              nullptr, (void**)&epVol);
                dev->Release();
            }
            enu->Release();
        }
        if (epVol) {
            DWORD hw = 0;
            if (SUCCEEDED(epVol->QueryHardwareSupport(&hw)))
                epVolHw = (hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0;
        }
    };
    bool seeking = false;
    float seekVal = 0.0f;
    bool tunerOn = false;
    float tunerFreq = 0.0f;
    double tunerLastSeen = -10.0;
    int frameNo = 0;
    static float tunerBuf[2048];
    // ---- metadata-bibliotheek (models\metadata.json)
    std::map<std::string, ModelMeta> metaDb;
    fs::path metaFile = modelsDir / L"metadata.json";
    {
        FILE* f = _wfopen(metaFile.wstring().c_str(), L"rb");
        if (f) {
            std::string s;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
            fclose(f);
            try {
                auto j = nlohmann::json::parse(s);
                for (auto& [k, v] : j.items()) {
                    ModelMeta mm;
                    mm.make = v.value("make", "");
                    mm.style = v.value("style", "");
                    mm.cab = v.value("cab", "");
                    mm.mic = v.value("mic", "");
                    mm.maker = v.value("maker", "");
                    mm.source = v.value("source", "");
                    mm.rating = v.value("rating", 0);
                    if (v.contains("tags") && v["tags"].is_array())
                        for (auto& t : v["tags"])
                            if (t.is_string())
                                mm.tags.push_back(t.get<std::string>());
                    metaDb[k] = std::move(mm);
                }
            } catch (...) {}
        }
    }
    auto saveMeta = [&] {
        nlohmann::json j = nlohmann::json::object();
        for (auto& [k, v] : metaDb) {
            nlohmann::json tags = nlohmann::json::array();
            for (auto& t : v.tags) tags.push_back(t);
            j[k] = {{"make", v.make},   {"style", v.style},
                    {"cab", v.cab},     {"mic", v.mic},
                    {"maker", v.maker}, {"source", v.source},
                    {"rating", v.rating}, {"tags", tags}};
        }
        FILE* f = _wfopen(metaFile.wstring().c_str(), L"wb");
        if (f) {
            std::string s = j.dump(2);
            fwrite(s.data(), 1, s.size(), f);
            fclose(f);
        }
    };
    auto ensureMeta = [&] {
        for (auto& m : models) deriveMeta(m.label, m.cat, metaDb[m.label]);
    };
    ensureMeta();
    saveMeta();

    // ---- presets: volledige instelling (model+sliders+output) onder een naam
    struct Preset {
        std::string name, model, outId, ir;
        float gain = 1.0f, master = 0.5f, gate = -90.0f;
        float eqB = 0.0f, eqM = 0.0f, eqT = 0.0f;
    };
    std::vector<Preset> presets;
    int curPreset = -1;
    char presetNameBuf[64] = "";
    fs::path presetsFile = modelsDir.parent_path() / L"presets.json";
    {
        FILE* f = _wfopen(presetsFile.wstring().c_str(), L"rb");
        if (f) {
            std::string s;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
            fclose(f);
            try {
                auto j = nlohmann::json::parse(s);
                for (auto& e : j) {
                    Preset p;
                    p.name = e.value("name", "");
                    p.model = e.value("model", "");
                    p.outId = e.value("output_device_id", "");
                    p.ir = e.value("ir", "");
                    p.gain = e.value("gain", 1.0f);
                    p.master = e.value("master", 0.5f);
                    p.gate = e.value("gate", -90.0f);
                    p.eqB = e.value("eq_bass", 0.0f);
                    p.eqM = e.value("eq_mid", 0.0f);
                    p.eqT = e.value("eq_treble", 0.0f);
                    if (!p.name.empty()) presets.push_back(std::move(p));
                }
            } catch (...) {}
        }
    }
    auto savePresets = [&] {
        nlohmann::json j = nlohmann::json::array();
        for (auto& p : presets)
            j.push_back({{"name", p.name},
                         {"model", p.model},
                         {"output_device_id", p.outId},
                         {"ir", p.ir},
                         {"gain", p.gain},
                         {"master", p.master},
                         {"gate", p.gate},
                         {"eq_bass", p.eqB},
                         {"eq_mid", p.eqM},
                         {"eq_treble", p.eqT}});
        FILE* f = _wfopen(presetsFile.wstring().c_str(), L"wb");
        if (f) {
            std::string s = j.dump(2);
            fwrite(s.data(), 1, s.size(), f);
            fclose(f);
        }
    };
    auto capturePreset = [&](const std::string& name) {
        Preset p;
        p.name = name;
        p.model = (selModel >= 0 && selModel < (int)models.size())
                      ? models[selModel].label
                      : "";
        if (selOut >= 0) {
            const std::wstring& w = outDevices[selOut].id;
            p.outId.assign(w.begin(), w.end());
        }
        if (selIr >= 0) p.ir = irs[selIr].label;
        p.gain = gainUI;
        p.master = masterUI;
        p.gate = gateUI;
        p.eqB = eqBassUI;
        p.eqM = eqMidUI;
        p.eqT = eqTrebleUI;
        return p;
    };
    auto applyPreset = [&](const Preset& p) {
        gainUI = p.gain;
        masterUI = p.master;
        gateUI = p.gate;
        eqBassUI = p.eqB;
        eqMidUI = p.eqM;
        eqTrebleUI = p.eqT;
        g.gain.store(gainUI);
        g.master.store(masterUI);
        g.gateDb.store(gateUI);
        g.eqBass.store(eqBassUI);
        g.eqMid.store(eqMidUI);
        g.eqTreble.store(eqTrebleUI);
        g.eqDirty.store(true);
        // outputdevice (leeg = automatisch)
        int newOut = -1;
        if (!p.outId.empty()) {
            std::wstring w(p.outId.begin(), p.outId.end());
            for (int i = 0; i < (int)outDevices.size(); i++)
                if (outDevices[i].id == w) newOut = i;
        }
        bool outChanged = newOut != selOut;
        selOut = newOut;
        // cabinet-IR (op bestandsnaam; leeg = uit)
        {
            int newIr = -1;
            if (!p.ir.empty())
                for (int i = 0; i < (int)irs.size(); i++)
                    if (irs[i].label == p.ir) newIr = i;
            if (newIr != selIr) {
                selIr = newIr;
                loadIrAsync(selIr >= 0 ? irs[selIr].path : L"");
            }
        }
        // model (op bestandsnaam)
        int newModel = -1;
        if (!p.model.empty())
            for (int i = 0; i < (int)models.size(); i++)
                if (models[i].label == p.model) newModel = i;
        bool modelChanged = newModel >= 0 && newModel != selModel;
        if (newModel >= 0) selModel = newModel;
        if (g.running.load()) {
            if (outChanged && !models.empty())
                startEngine(models[selModel].path, currentOutId());
            else if (modelChanged)
                loadModelAsync(models[selModel].path);
        }
        saveSettings();
    };

    // opstart-argument = pad naar een nummer (voor tests/snelkoppelingen)
    if (pCmd && pCmd[0]) {
        std::wstring arg = pCmd;
        while (!arg.empty() && (arg.front() == L'"' || arg.front() == L' '))
            arg.erase(0, 1);
        while (!arg.empty() && (arg.back() == L'"' || arg.back() == L' '))
            arg.pop_back();
        if (arg.empty()) {
            // niets bruikbaars meegegeven
        } else if (fs::exists(arg)) {
            loadTrackAsync(arg);
        } else {
            g.setError("opstartbestand niet gevonden: " +
                       fs::path(arg).filename().string());
        }
    }
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);

        bool running = g.running.load();

        // ===== zijbalk: devices / status / levels (losse cards) =====
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("sidebar", ImVec2(255, 0), ImGuiChildFlags_None);
        ImGui::PopStyleColor();

        // logo + titel (klik = over-dialoog)
        if (gLogoSRV) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::ImageButton("##logobtn",
                                   (ImTextureID)(intptr_t)gLogoSRV,
                                   ImVec2(38, 38)))
                ImGui::OpenPopup("Over Guitar Audio Lab");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Over deze app");
            ImGui::SameLine();
        }
        {
            float ty = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(ty + 10.0f);
            if (gFontBold) ImGui::PushFont(gFontBold);
            ImGui::TextUnformatted("Guitar Audio Lab");
            if (gFontBold) ImGui::PopFont();
        }
        if (ImGui::BeginPopupModal("Over Guitar Audio Lab", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            static ID3D11ShaderResourceView* aboutTex = nullptr;
            static bool aboutTried = false;
            if (!aboutTried) {
                aboutTried = true;
                fs::path p = gAssetsDir / L"about.png";
                std::error_code ec;
                if (fs::exists(p, ec))
                    aboutTex = loadPngTexture(gDev, p.wstring().c_str());
            }
            if (aboutTex)
                ImGui::Image((ImTextureID)(intptr_t)aboutTex,
                             ImVec2(300, 300));
            if (gFontBold) ImGui::PushFont(gFontBold);
            ImGui::TextUnformatted("Guitar Audio Lab  v1.0");
            if (gFontBold) ImGui::PopFont();
            ImGui::TextDisabled("NAM-amp-modelling op ~5 ms latency");
            ImGui::Spacing();
            ImGui::TextDisabled(
                "Engine: WASAPI Exclusive + NeuralAmpModelerCore (MIT)");
            ImGui::TextDisabled("Tones: TONE3000 & de NAM-community");
            ImGui::TextDisabled(
                "UI: Dear ImGui - artwork: lokaal via ComfyUI/SDXL");
            ImGui::Spacing();
            ImGui::TextDisabled(
                "Met een knipoog naar de Pignose waar het ooit begon.");
            ImGui::Spacing();
            if (ImGui::Button("Sluiten", ImVec2(-1, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::Spacing();

        ImGui::BeginChild("card_dev", ImVec2(0, 0),
                          ImGuiChildFlags_Borders |
                              ImGuiChildFlags_AutoResizeY);
        sectionHeader("DEVICES");
        ImGui::TextUnformatted("Rocksmith Real Tone");
        if (running)
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f),
                               "actief  48 kHz / 16 bit / mono");
        else
            ImGui::TextDisabled("klaar  48 kHz / 16 bit / mono");
        ImGui::Spacing();
        ImGui::TextUnformatted("Output:");
        ImGui::SetNextItemWidth(-58);
        {
            const char* preview = selOut >= 0 ? outDevices[selOut].name.c_str()
                                              : "Automatisch (S/PDIF)";
            if (ImGui::BeginCombo("##outdev", preview)) {
                if (ImGui::Selectable("Automatisch (S/PDIF)", selOut == -1)) {
                    if (selOut != -1) {
                        selOut = -1;
                        saveSettings();
                        if (running && !models.empty())
                            startEngine(models[selModel].path, currentOutId());
                    }
                }
                for (int i = 0; i < (int)outDevices.size(); i++) {
                    if (ImGui::Selectable(outDevices[i].name.c_str(),
                                          selOut == i)) {
                        if (selOut != i) {
                            selOut = i;
                            saveSettings();
                            if (running && !models.empty())
                                startEngine(models[selModel].path,
                                            currentOutId());
                        }
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("scan")) {  // opnieuw scannen (bv. na BT)
            outDevices = enumRenderDevices();
            if (selOut >= (int)outDevices.size()) selOut = -1;
        }
        if (running) {
            if (g.outShared.load())
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                                   "actief - shared (hogere latency)");
            else
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f),
                                   "actief - exclusive");
        } else {
            ImGui::TextDisabled("klaar");
        }
        // volume van het outputdevice zelf (zoals de Windows-slider voor
        // precies dit apparaat)
        ensureEpVol(currentOutId());
        if (epVol) {
            if (epVolUI < 0.0f || ++epVolPoll >= 30) {
                epVolPoll = 0;
                float v = 0.0f;
                if (SUCCEEDED(epVol->GetMasterVolumeLevelScalar(&v)))
                    epVolUI = v;
            }
            float pct = (epVolUI < 0.0f ? 0.0f : epVolUI) * 100.0f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##epvol", &pct, 0.0f, 100.0f,
                                   "Apparaatvolume %.0f%%")) {
                epVolUI = pct / 100.0f;
                epVol->SetMasterVolumeLevelScalar(epVolUI, nullptr);
            }
            if (!epVolHw && running && !g.outShared.load())
                ImGui::TextDisabled("(digitale uitgang: volume via Master)");
        }
        ImGui::EndChild();  // card_dev

        ImGui::BeginChild("card_status", ImVec2(0, 0),
                          ImGuiChildFlags_Borders |
                              ImGuiChildFlags_AutoResizeY);
        sectionHeader("STATUS");
        auto statusRow = [](const char* k, const char* fmt, ...) {
            ImGui::TextDisabled("%s", k);
            ImGui::SameLine(110);
            va_list args;
            va_start(args, fmt);
            ImGui::TextV(fmt, args);
            va_end(args);
        };
        statusRow("API", "WASAPI Excl.");
        statusRow("Buffer", "144 (3.0 ms)");
        if (running) {
            double padMs = g.padAvgFrames.load() * 1000.0 / SR;
            statusRow("Latency", "%.1f ms", 3.0 + padMs);
            statusRow("NAM", "%.2f ms/blok", g.dspAvgUs.load() / 1000.0);
            statusRow("Underruns", "%ld", g.underruns.load());
            statusRow("Drift", "%ld", g.driftCorr.load());
        } else {
            statusRow("Latency", "-");
            statusRow("Engine", "gestopt");
        }
        ImGui::EndChild();  // card_status

        ImGui::BeginChild("card_levels", ImVec2(0, 0),
                          ImGuiChildFlags_Borders |
                              ImGuiChildFlags_AutoResizeY);
        sectionHeader("LEVELS");
        {
            float pin = g.peakIn.exchange(0.0f);
            float pout = g.peakOut.exchange(0.0f);
            uiPeakIn = pin > uiPeakIn ? pin : uiPeakIn * 0.93f;
            uiPeakOut = pout > uiPeakOut ? pout : uiPeakOut * 0.93f;
            meter("in", uiPeakIn, 160.0f);
            meter("uit", uiPeakOut, 160.0f);
        }
        ImGui::EndChild();  // card_levels

        ImGui::EndChild();  // sidebar
        ImGui::SameLine();

        // ===== hoofdpaneel =====
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_None);
        ImGui::PopStyleColor();

        // --- bovenpaneel: preset + model + transport + hoofdcontrols ---
        ImGui::BeginChild("card_top", ImVec2(0, 0),
                          ImGuiChildFlags_Borders |
                              ImGuiChildFlags_AutoResizeY);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Preset");
        ImGui::SameLine(76);
        ImGui::SetNextItemWidth(280);
        {
            const char* pv = (curPreset >= 0 && curPreset < (int)presets.size())
                                 ? presets[curPreset].name.c_str()
                                 : "(geen)";
            if (ImGui::BeginCombo("##preset", pv)) {
                for (int i = 0; i < (int)presets.size(); i++) {
                    if (ImGui::Selectable(presets[i].name.c_str(),
                                          i == curPreset)) {
                        curPreset = i;
                        applyPreset(presets[i]);
                    }
                }
                if (presets.empty())
                    ImGui::TextDisabled("nog geen presets");
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Opslaan als...")) {
            presetNameBuf[0] = 0;
            ImGui::OpenPopup("preset_save");
        }
        if (curPreset >= 0 && curPreset < (int)presets.size()) {
            ImGui::SameLine();
            if (ImGui::Button("Bijwerken")) {
                std::string keep = presets[curPreset].name;
                presets[curPreset] = capturePreset(keep);
                savePresets();
            }
            ImGui::SameLine();
            if (ImGui::Button("Verwijder")) {
                presets.erase(presets.begin() + curPreset);
                curPreset = -1;
                savePresets();
            }
        }
        if (ImGui::BeginPopupModal("preset_save", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Naam voor deze preset:");
            ImGui::SetNextItemWidth(320);
            bool enter = ImGui::InputText(
                "##pname", presetNameBuf, sizeof(presetNameBuf),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
            ImGui::Spacing();
            bool canSave = presetNameBuf[0] != 0;
            if (!canSave) ImGui::BeginDisabled();
            if (ImGui::Button("Opslaan", ImVec2(120, 0)) ||
                (enter && canSave)) {
                presets.push_back(capturePreset(presetNameBuf));
                curPreset = (int)presets.size() - 1;
                savePresets();
                ImGui::CloseCurrentPopup();
            }
            if (!canSave) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Annuleren", ImVec2(120, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Model");
        ImGui::SameLine(76);
        if (selModel >= 0 && selModel < (int)models.size()) {
            if (ID3D11ShaderResourceView* art =
                    getCatArt(models[selModel].cat)) {
                ImGui::Image((ImTextureID)(intptr_t)art, ImVec2(42, 27),
                             ImVec2(0.0f, 0.19f), ImVec2(1.0f, 0.81f));
                ImGui::SameLine();
            }
        }
        if (ImGui::ArrowButton("##prevm", ImGuiDir_Left) && !models.empty()) {
            selModel = (selModel + (int)models.size() - 1) % (int)models.size();
            if (running) loadModelAsync(models[selModel].path);
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##nextm", ImGuiDir_Right) && !models.empty()) {
            selModel = (selModel + 1) % (int)models.size();
            if (running) loadModelAsync(models[selModel].path);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-120);
        const char* preview =
            models.empty() ? "(geen .nam in models\\)"
                           : models[selModel].label.c_str();
        if (ImGui::BeginCombo("##model", preview)) {
            for (int i = 0; i < (int)models.size(); i++) {
                if (ImGui::Selectable(models[i].label.c_str(), i == selModel)) {
                    if (i != selModel) {
                        selModel = i;
                        if (running) loadModelAsync(models[selModel].path);
                    }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ververs")) {
            models = scanModels(modelsDir);
            if (selModel >= (int)models.size()) selModel = 0;
            applySort();
        }

        // --- cabinet-IR ---
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Cabinet");
        ImGui::SameLine(76);
        if (selIr >= 0) {
            if (ID3D11ShaderResourceView* art = getCatArt(NUM_CATS + 1)) {
                ImGui::Image((ImTextureID)(intptr_t)art, ImVec2(42, 27),
                             ImVec2(0.0f, 0.19f), ImVec2(1.0f, 0.81f));
                ImGui::SameLine();
            }
        }
        ImGui::SetNextItemWidth(-120);
        {
            const char* pv = selIr >= 0 && selIr < (int)irs.size()
                                 ? irs[selIr].label.c_str()
                                 : "(geen IR - direct uit het model)";
            if (ImGui::BeginCombo("##ircombo", pv)) {
                if (ImGui::Selectable("(geen IR - direct uit het model)",
                                      selIr == -1)) {
                    if (selIr != -1) {
                        selIr = -1;
                        loadIrAsync(L"");
                        saveSettings();
                    }
                }
                for (int i = 0; i < (int)irs.size(); i++) {
                    if (ImGui::Selectable(irs[i].label.c_str(),
                                          selIr == i)) {
                        if (selIr != i) {
                            selIr = i;
                            loadIrAsync(irs[i].path);
                            saveSettings();
                        }
                    }
                }
                if (irs.empty())
                    ImGui::TextDisabled(
                        "geen IR's - zet .wav's in de map irs of download "
                        "via TONE3000 (formaat: IR)");
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Ververs##ir")) {
            irs = scanIrs();
            if (selIr >= (int)irs.size()) selIr = -1;
            applySort();
        }

        // --- start/stop ---
        ImGui::Spacing();
        if (!running) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.13f, 0.52f, 0.31f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.17f, 0.62f, 0.38f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.11f, 0.45f, 0.27f, 1.0f));
            if (ImGui::Button("Start", ImVec2(110, 0)) && !models.empty())
                startEngine(models[selModel].path, currentOutId());
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.62f, 0.22f, 0.19f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.72f, 0.28f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.55f, 0.18f, 0.16f, 1.0f));
            if (ImGui::Button("Stop", ImVec2(110, 0))) stopEngine();
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine();
        {
            std::string err = g.getError();
            if (!err.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s",
                                   err.c_str());
            else if (g.modelLoading.load())
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                                   "model laden...");
            else if (running)
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "actief");
            else
                ImGui::TextDisabled("gestopt");
        }

        // afgedankt model/track/IR (na hot-swap) buiten het audiopad opruimen
        {
            nam::DSP* r = g.retiredModel.exchange(nullptr);
            if (r) delete r;
            PlayerTrack* rt = g.retiredTrack.exchange(nullptr);
            if (rt) delete rt;
            IrData* ri = g.retiredIr.exchange(nullptr);
            if (ri) delete ri;
        }

        // --- compacte controls: alles op Ã©Ã©n regel ---
        {
            if (Knob("gain", "Gain", &gainUI, 0.0f, 4.0f, 1.0f, "%.2fx"))
                g.gain.store(gainUI);
            ImGui::SameLine();
            if (Knob("gate", "Gate", &gateUI, -90.0f, -40.0f, -90.0f,
                     "%.0f dB", "uit"))
                g.gateDb.store(gateUI);
            ImGui::SameLine();
            if (Knob("master", "Master", &masterUI, 0.0f, 1.5f, 0.5f,
                     "%.2f"))
                g.master.store(masterUI);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(14, 0));
            ImGui::SameLine();
            bool ch = false;
            ch |= Knob("eqb", "Bass", &eqBassUI, -12.0f, 12.0f, 0.0f,
                       "%+.0f dB", nullptr, true);
            ImGui::SameLine();
            ch |= Knob("eqm", "Mid", &eqMidUI, -12.0f, 12.0f, 0.0f,
                       "%+.0f dB", nullptr, true);
            ImGui::SameLine();
            ch |= Knob("eqt", "Treble", &eqTrebleUI, -12.0f, 12.0f, 0.0f,
                       "%+.0f dB", nullptr, true);
            ImGui::SameLine();
            {
                ImGui::BeginGroup();
                ImGui::Dummy(ImVec2(0, 26));
                if (ImGui::Button("EQ reset")) {
                    eqBassUI = eqMidUI = eqTrebleUI = 0.0f;
                    ch = true;
                }
                ImGui::EndGroup();
            }
            if (ch) {
                g.eqBass.store(eqBassUI);
                g.eqMid.store(eqMidUI);
                g.eqTreble.store(eqTrebleUI);
                g.eqDirty.store(true);
            }
        }


        ImGui::EndChild();  // card_top

        // ===== tabbladen (in eigen card, vult de rest) =====
        float statusBarH =
            2.0f * ImGui::GetTextLineHeightWithSpacing() + 12.0f;
        ImGui::BeginChild("card_tabs",
                          ImVec2(0, ImGui::GetContentRegionAvail().y -
                                        statusBarH),
                          ImGuiChildFlags_Borders);
        if (ImGui::BeginTabBar("tabs")) {

        // --- tab: mijn modellen (bibliotheekbrowser) ---
        if (ImGui::BeginTabItem("Mijn modellen")) {
            int counts[NUM_CATS + 1] = {};
            int favCount = 0;
            for (auto& m : models) {
                counts[m.cat]++;
                if (favorites.count(m.label)) favCount++;
            }
            auto toLower = [](std::string s) {
                for (auto& c : s) c = (char)tolower((unsigned char)c);
                return s;
            };
            auto stripExt = [](const std::string& s) {
                return s.size() > 4 && s.substr(s.size() - 4) == ".nam"
                           ? s.substr(0, s.size() - 4)
                           : s;
            };
            // maker-heuristiek: "Tim R ..." -> "Tim R"
            auto makerOf = [](const std::string& s) -> std::string {
                size_t sp1 = s.find(' ');
                if (sp1 == std::string::npos) return "";
                size_t sp2 = s.find(' ', sp1 + 1);
                if (sp2 == std::string::npos) return "";
                if (sp2 - sp1 == 2)  // tweede token is 1 letter
                    return s.substr(0, sp2);
                return "";
            };
            float paneH = ImGui::GetContentRegionAvail().y;

            // 1) categorieen
            ImGui::BeginChild("cats", ImVec2(180, paneH),
                              ImGuiChildFlags_Borders);
            sectionHeader("BIBLIOTHEEK");
            auto catRow = [&](const char* nm, int val, int count) {
                char lbl[80];
                snprintf(lbl, sizeof(lbl), "%s  (%d)", nm, count);
                if (ImGui::Selectable(lbl, catSel == val)) catSel = val;
            };
            catRow("Alle", -1, (int)models.size());
            if (favCount > 0) catRow("Favorieten", -2, favCount);
            for (int i = 0; i <= NUM_CATS; i++) {
                if (counts[i] == 0) continue;
                catRow(i < NUM_CATS ? MODEL_CATS[i].name : "Overig", i,
                       counts[i]);
            }
            ImGui::Separator();
            catRow("Cabinets (IR's)", -3, (int)irs.size());
            if (t3k.connected.load()) {
                ImGui::Separator();
                if (t3k.busy.load()) {
                    ImGui::TextDisabled("bezig...");
                    std::string ns = g.getNetStatus();
                    if (!ns.empty())
                        ImGui::TextDisabled("%s", ns.c_str());
                } else if (ImGui::SmallButton("Metadata ophalen")) {
                    t3kFetchMetaAsync();  // TONE3000-info voor downloads
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();

            // 2) modellenlijst met zoekveld
            float detailW = 300.0f;
            ImGui::BeginChild(
                "listwrap",
                ImVec2(ImGui::GetContentRegionAvail().x - detailW -
                           ImGui::GetStyle().ItemSpacing.x,
                       paneH),
                ImGuiChildFlags_Borders);
            ImGui::SetNextItemWidth(-160);
            ImGui::InputTextWithHint("##libsearch", "zoeken...", libSearch,
                                     sizeof(libSearch));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            {
                static const char* sortNames[] = {"Naam (A-Z)",
                                                  "Nieuwste eerst"};
                if (ImGui::Combo("##libsort", &librarySort, sortNames, 2)) {
                    applySort();
                    saveSettings();
                }
            }
            ImGui::BeginChild("modlist", ImVec2(0, 0), ImGuiChildFlags_None);
            // zoektermen (spatie-gescheiden) matchen op naam + metadata,
            // dus "fender clean" of "marshall crunch" werkt
            std::vector<std::string> terms;
            {
                std::string cur;
                for (char c : std::string(libSearch)) {
                    if (c == ' ') {
                        if (!cur.empty()) terms.push_back(toLower(cur));
                        cur.clear();
                    } else {
                        cur += c;
                    }
                }
                if (!cur.empty()) terms.push_back(toLower(cur));
            }
            if (catSel == -3) {
                // cabinet-IR-weergave
                for (int i = 0; i < (int)irs.size(); i++) {
                    if (!terms.empty()) {
                        std::string hay = toLower(irs[i].label);
                        bool ok = true;
                        for (auto& t : terms)
                            if (hay.find(t) == std::string::npos) {
                                ok = false;
                                break;
                            }
                        if (!ok) continue;
                    }
                    ImGui::PushID(100000 + i);
                    if (ImGui::Selectable(
                            irs[i].label.c_str(), i == irBrowseSel,
                            ImGuiSelectableFlags_AllowDoubleClick)) {
                        irBrowseSel = i;
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            selIr = i;
                            loadIrAsync(irs[i].path);
                            saveSettings();
                        }
                    }
                    if (i == selIr) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
                                           "actief");
                    }
                    ImGui::PopID();
                }
                if (irs.empty())
                    ImGui::TextDisabled(
                        "geen IR's - download via TONE3000 (formaat: IR)");
            } else
            for (int i = 0; i < (int)models.size(); i++) {
                auto& m = models[i];
                if (catSel >= 0 && m.cat != catSel) continue;
                if (catSel == -2 && !favorites.count(m.label)) continue;
                if (!terms.empty()) {
                    ModelMeta& mm = metaDb[m.label];
                    std::string hay = m.label + " " + mm.make + " " +
                                      mm.style + " " + mm.cab + " " + mm.mic +
                                      " " + mm.maker;
                    for (auto& t : mm.tags) hay += " " + t;
                    hay = toLower(hay);
                    bool ok = true;
                    for (auto& t : terms)
                        if (hay.find(t) == std::string::npos) {
                            ok = false;
                            break;
                        }
                    if (!ok) continue;
                }
                bool isFav = favorites.count(m.label) > 0;
                std::string row =
                    (isFav ? "* " : "   ") + stripExt(m.label);
                ImGui::PushID(i);
                if (ImGui::Selectable(row.c_str(), i == browseSel,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    browseSel = i;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        selModel = i;
                        if (running) loadModelAsync(m.path);
                    }
                }
                if (i == selModel) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
                                       "geladen");
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndChild();
            ImGui::SameLine();

            // 3) detailpaneel
            ImGui::BeginChild("details", ImVec2(detailW, paneH),
                              ImGuiChildFlags_Borders);
            sectionHeader("DETAILS");
            if (catSel == -3) {
                if (irBrowseSel >= 0 && irBrowseSel < (int)irs.size()) {
                    auto& e = irs[irBrowseSel];
                    if (ID3D11ShaderResourceView* art =
                            getCatArt(NUM_CATS + 1)) {
                        float aw = ImGui::GetContentRegionAvail().x;
                        ImGui::Image((ImTextureID)(intptr_t)art,
                                     ImVec2(aw, aw * 0.62f),
                                     ImVec2(0.0f, 0.19f),
                                     ImVec2(1.0f, 0.81f));
                        ImGui::Spacing();
                    }
                    if (gFontBold) ImGui::PushFont(gFontBold);
                    ImGui::TextWrapped("%s", e.label.c_str());
                    if (gFontBold) ImGui::PopFont();
                    ImGui::Spacing();
                    std::error_code ec;
                    auto sz = fs::file_size(e.path, ec);
                    ImGui::TextDisabled("Grootte");
                    ImGui::SameLine(90);
                    ImGui::Text("%.0f kB", ec ? 0.0 : sz / 1024.0);
                    struct _stat64 st;
                    if (_wstat64(e.path.c_str(), &st) == 0) {
                        tm tmv;
                        localtime_s(&tmv, &st.st_mtime);
                        ImGui::TextDisabled("Datum");
                        ImGui::SameLine(90);
                        ImGui::Text("%02d-%02d-%d", tmv.tm_mday,
                                    tmv.tm_mon + 1, tmv.tm_year + 1900);
                    }
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (irBrowseSel == selIr) {
                        ImGui::BeginDisabled();
                        ImGui::Button("In gebruik als cabinet",
                                      ImVec2(-1, 0));
                        ImGui::EndDisabled();
                    } else if (ImGui::Button("Gebruik als cabinet",
                                             ImVec2(-1, 0))) {
                        selIr = irBrowseSel;
                        loadIrAsync(e.path);
                        saveSettings();
                    }
                    if (ImGui::Button("Verwijder van schijf",
                                      ImVec2(-1, 0))) {
                        delType = 2;
                        delIdx = irBrowseSel;
                        ImGui::OpenPopup("Verwijderen?##delconfirm");
                    }
                } else {
                    ImGui::TextDisabled("Selecteer een IR in de lijst.");
                    ImGui::TextDisabled(
                        "Dubbelklik = gebruiken als cabinet.");
                }
            } else if (browseSel >= 0 && browseSel < (int)models.size()) {
                auto& m = models[browseSel];
                if (ID3D11ShaderResourceView* art = getCatArt(m.cat)) {
                    float aw = ImGui::GetContentRegionAvail().x;
                    ImGui::Image((ImTextureID)(intptr_t)art,
                                 ImVec2(aw, aw * 0.62f), ImVec2(0.0f, 0.19f),
                                 ImVec2(1.0f, 0.81f));
                    ImGui::Spacing();
                }
                if (gFontBold) ImGui::PushFont(gFontBold);
                ImGui::TextWrapped("%s", stripExt(m.label).c_str());
                if (gFontBold) ImGui::PopFont();
                ImGui::Spacing();
                auto row = [](const char* k, const std::string& v) {
                    ImGui::TextDisabled("%s", k);
                    ImGui::SameLine(90);
                    ImGui::TextWrapped("%s", v.empty() ? "-" : v.c_str());
                };
                ModelMeta& mm = metaDb[m.label];
                row("Merk", mm.make);
                // stijl: bewerkbaar (handmatige correctie van de heuristiek)
                {
                    static const char* styles[] = {"-", "Clean", "Crunch",
                                                   "High-gain", "Bas"};
                    int si = 0;
                    for (int s = 1; s < 5; s++)
                        if (mm.style == styles[s]) si = s;
                    ImGui::TextDisabled("Stijl");
                    ImGui::SameLine(90);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##mstyle", &si, styles, 5)) {
                        mm.style = si == 0 ? "" : styles[si];
                        saveMeta();
                    }
                }
                row("Cab", mm.cab);
                row("Mic", mm.mic);
                row("Maker", mm.maker.empty() ? makerOf(m.label) : mm.maker);
                {
                    std::string tj;
                    for (auto& t : mm.tags) {
                        if (!tj.empty()) tj += ", ";
                        tj += t;
                    }
                    row("Tags", tj);
                }
                // rating 0-5
                {
                    ImGui::TextDisabled("Rating");
                    ImGui::SameLine(90);
                    ImGui::SetNextItemWidth(-1);
                    int r = mm.rating;
                    if (ImGui::SliderInt("##mrating", &r, 0, 5,
                                         r ? "%d/5" : "geen")) {
                        mm.rating = r;
                        saveMeta();
                    }
                }
                // bron: metadata, anders registers
                std::string bron = mm.source;
                if (bron.empty()) {
                    bron = "lokaal";
                    {
                        std::lock_guard<std::mutex> lk(t3k.dlMu);
                        for (auto& [id, fn] : t3k.downloaded)
                            if (fn == m.label) { bron = "TONE3000"; break; }
                    }
                    if (bron == "lokaal") {
                        std::lock_guard<std::mutex> lk(g.catMutex);
                        for (auto& p : g.catalog)
                            if (fs::path(p).filename().string() == m.label) {
                                bron = "Community (GitHub)";
                                break;
                            }
                    }
                }
                row("Bron", bron);
                {
                    std::error_code ec;
                    auto sz = fs::file_size(m.path, ec);
                    char szs[32];
                    snprintf(szs, sizeof(szs), "%.0f kB",
                             ec ? 0.0 : sz / 1024.0);
                    row("Grootte", szs);
                    struct _stat64 st;
                    if (_wstat64(m.path.c_str(), &st) == 0) {
                        tm tmv;
                        localtime_s(&tmv, &st.st_mtime);
                        char ds[32];
                        snprintf(ds, sizeof(ds), "%02d-%02d-%d",
                                 tmv.tm_mday, tmv.tm_mon + 1,
                                 tmv.tm_year + 1900);
                        row("Datum", ds);
                    }
                }
                row("Bestand", m.label);
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                bool isLoaded = browseSel == selModel;
                if (isLoaded) {
                    ImGui::BeginDisabled();
                    ImGui::Button("Geladen", ImVec2(-1, 0));
                    ImGui::EndDisabled();
                } else if (ImGui::Button("Laden", ImVec2(-1, 0))) {
                    selModel = browseSel;
                    if (running) loadModelAsync(m.path);
                }
                bool isFav = favorites.count(m.label) > 0;
                if (ImGui::Button(isFav ? "Verwijder uit favorieten"
                                        : "Markeer als favoriet",
                                  ImVec2(-1, 0))) {
                    if (isFav) favorites.erase(m.label);
                    else favorites.insert(m.label);
                    saveSettings();
                }
                if (ImGui::Button("Verwijder van schijf", ImVec2(-1, 0))) {
                    delType = 1;
                    delIdx = browseSel;
                    ImGui::OpenPopup("Verwijderen?##delconfirm");
                }
            } else {
                ImGui::TextDisabled("Selecteer een model in de lijst.");
                ImGui::TextDisabled("Dubbelklik = direct laden.");
            }

            // bevestigingsdialoog (zelfde ID-scope als de knoppen hierboven)
            if (ImGui::BeginPopupModal("Verwijderen?##delconfirm", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                const char* nm = "?";
                if (delType == 1 && delIdx >= 0 &&
                    delIdx < (int)models.size())
                    nm = models[delIdx].label.c_str();
                else if (delType == 2 && delIdx >= 0 &&
                         delIdx < (int)irs.size())
                    nm = irs[delIdx].label.c_str();
                ImGui::TextUnformatted("Definitief van schijf verwijderen?");
                ImGui::PushTextWrapPos(360.0f);
                ImGui::TextDisabled("%s", nm);
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.62f, 0.22f, 0.19f, 1.0f));
                bool doDelete =
                    ImGui::Button("Verwijder", ImVec2(140, 0));
                ImGui::PopStyleColor();
                if (doDelete) {
                    std::error_code ec;
                    if (delType == 1 && delIdx >= 0 &&
                        delIdx < (int)models.size()) {
                        std::string lbl = models[delIdx].label;
                        std::wstring keepPath =
                            (selModel >= 0 && selModel < (int)models.size())
                                ? models[selModel].path
                                : L"";
                        fs::remove(models[delIdx].path, ec);
                        favorites.erase(lbl);
                        metaDb.erase(lbl);
                        models = scanModels(modelsDir);
                        selModel = 0;
                        for (int i = 0; i < (int)models.size(); i++)
                            if (models[i].path == keepPath) selModel = i;
                        applySort();
                        saveSettings();
                        saveMeta();
                    } else if (delType == 2 && delIdx >= 0 &&
                               delIdx < (int)irs.size()) {
                        bool wasActive = delIdx == selIr;
                        std::string activeLbl =
                            (selIr >= 0 && selIr < (int)irs.size())
                                ? irs[selIr].label
                                : "";
                        fs::remove(irs[delIdx].path, ec);
                        irs = scanIrs();
                        selIr = -1;
                        if (!wasActive)
                            for (int i = 0; i < (int)irs.size(); i++)
                                if (irs[i].label == activeLbl) selIr = i;
                        if (wasActive) loadIrAsync(L"");
                        applySort();
                        irBrowseSel = -1;
                        saveSettings();
                    }
                    delType = 0;
                    delIdx = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Annuleren", ImVec2(140, 0))) {
                    delType = 0;
                    delIdx = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // --- tab: TONE3000 ---
        if (ImGui::BeginTabItem("TONE3000")) {
            if (!t3k.connected.load()) {
                ImGui::TextWrapped(
                    "Verbind met TONE3000 (het vroegere ToneHunt, 350k+ "
                    "tones). Maak een gratis account op tone3000.com, ga "
                    "naar Settings > API Keys, maak een publishable key "
                    "(t3k_pub_...) met redirect-URI "
                    "http://127.0.0.1:53682/callback en plak de key "
                    "hieronder.");
                ImGui::SetNextItemWidth(-130);
                ImGui::InputTextWithHint("##t3kkey", "t3k_pub_...", t3kKeyBuf,
                                         sizeof(t3kKeyBuf));
                ImGui::SameLine();
                if (ImGui::Button("Verbinden") && t3kKeyBuf[0])
                    t3kConnectAsync(t3kKeyBuf);
            } else {
                static const char* fmts[] = {"NAM-modellen",
                                             "IR's (cabinets)"};
                static const char* sorts[] = {"meest gedownload",
                                              "beste match", "nieuwste",
                                              "trending"};
                static const char* sortKeys[] = {"downloads-all-time",
                                                 "best-match", "newest",
                                                 "trending"};
                auto redoSearch = [&] {
                    // oude resultaten horen bij het vorige formaat/sortering:
                    // direct legen (anders kloppen de "al binnen"-checks en
                    // downloadbestemmingen niet) en opnieuw zoeken
                    {
                        std::lock_guard<std::mutex> lk(t3k.resMu);
                        t3k.results.clear();
                    }
                    if (!t3k.busy.load())
                        t3kSearchAsync(t3kQuery, sortKeys[t3kSort],
                                       t3kFormat == 1 ? "ir" : "nam");
                };
                ImGui::SetNextItemWidth(190);
                ImGui::InputTextWithHint("##t3kq", "zoek (bv. plexi, metal)",
                                         t3kQuery, sizeof(t3kQuery));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130);
                if (ImGui::Combo("##t3kfmt", &t3kFormat, fmts, 2))
                    redoSearch();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160);
                if (ImGui::Combo("##t3ksort", &t3kSort, sorts, 4))
                    redoSearch();
                ImGui::SameLine();
                if (ImGui::Button("Zoek") && !t3k.busy.load())
                    t3kSearchAsync(t3kQuery, sortKeys[t3kSort],
                                   t3kFormat == 1 ? "ir" : "nam");
                ImGui::SameLine();
                if (ImGui::SmallButton("verbreek")) {
                    std::lock_guard<std::mutex> lk(t3k.mu);
                    t3k.accessToken.clear();
                    t3k.refreshToken.clear();
                    t3k.expiresAt = 0;
                    t3k.connected.store(false);
                    t3k.save();
                }
                std::vector<T3KResult> res;
                {
                    std::lock_guard<std::mutex> lk(t3k.resMu);
                    res = t3k.results;
                }
                if (res.empty()) {
                    ImGui::TextDisabled(
                        "nog geen resultaten - zoek hierboven");
                } else if (ImGui::BeginTable(
                               "t3kres", 5,
                               ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_BordersInnerV,
                               ImVec2(0, 230))) {
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("titel");
                    ImGui::TableSetupColumn("maker", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("downloads", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("favs", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableHeadersRow();
                    for (int i = 0; i < (int)res.size(); i++) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::PushID(i);
                        bool isIrMode = t3kFormat == 1;
                        const fs::path& dstDir =
                            isIrMode ? irsDir : modelsDir;
                        // uitsluitend het tone-ID-register: titels zijn
                        // niet uniek (meerdere tones kunnen "Acoustic
                        // Guitar" heten)
                        bool have =
                            t3k.isDownloaded(res[i].toneId, dstDir);
                        if (have) {
                            ImGui::TextDisabled("al binnen");
                        } else if (ImGui::SmallButton("ophalen") &&
                                   !g.downloading.load()) {
                            t3kDownloadAsync(res[i].toneId, res[i].title,
                                             dstDir, res[i].makes,
                                             res[i].tags, res[i].creator,
                                             isIrMode);
                        }
                        ImGui::PopID();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(res[i].title.c_str());
                        if (!res[i].makes.empty() &&
                            ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", res[i].makes.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(res[i].creator.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%lld", res[i].downloads);
                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text("%lld", res[i].favorites);
                    }
                    ImGui::EndTable();
                }
                // paginering
                if (!res.empty()) {
                    int cp = t3k.curPage.load();
                    int tp = t3k.totalPages.load();
                    bool busy = t3k.busy.load();
                    if (cp <= 1 || busy) ImGui::BeginDisabled();
                    if (ImGui::Button("< Vorige"))
                        t3kSearchAsync(t3kQuery, sortKeys[t3kSort],
                                       t3kFormat == 1 ? "ir" : "nam",
                                       cp - 1);
                    if (cp <= 1 || busy) ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::Text("pagina %d / %d  (%lld tones)", cp, tp,
                                t3k.totalTones.load());
                    ImGui::SameLine();
                    if (cp >= tp || busy) ImGui::BeginDisabled();
                    if (ImGui::Button("Volgende >"))
                        t3kSearchAsync(t3kQuery, sortKeys[t3kSort],
                                       t3kFormat == 1 ? "ir" : "nam",
                                       cp + 1);
                    if (cp >= tp || busy) ImGui::EndDisabled();
                }
            }
            {
                std::string ns = g.getNetStatus();
                if (!ns.empty()) ImGui::TextDisabled("%s", ns.c_str());
            }
            ImGui::EndTabItem();
        }

        // --- tab: meespelen ---
        if (ImGui::BeginTabItem("Meespelen")) {
        if (ImGui::Button("Nummer openen...")) {
            std::wstring f = openAudioFileDialog(hwnd);
            if (!f.empty()) loadTrackAsync(f);
        }
        ImGui::SameLine();
        if (g.trackLoading.load()) {
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "laden...");
        } else {
            std::string tn = g.getTrackName();
            if (tn.empty()) ImGui::TextDisabled("(geen nummer geladen)");
            else ImGui::TextUnformatted(tn.c_str());
        }
        {
            long long lenFr = g.playerLenFr.load();
            if (lenFr > 0) {
                bool playing = g.playerPlaying.load();
                if (ImGui::Button(playing ? "Pauze" : "Speel",
                                  ImVec2(100, 0)))
                    g.playerPlaying.store(!playing);
                ImGui::SameLine();
                bool loop = g.playerLoop.load();
                if (ImGui::Checkbox("Herhaal", &loop))
                    g.playerLoop.store(loop);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-110);
                if (ImGui::SliderFloat("Volume", &trackVolUI, 0.0f, 1.5f,
                                       "%.2f"))
                    g.playerVol.store(trackVolUI);
                double lenS = lenFr / (double)SR;
                double posS = g.playerPosFr.load() / (double)SR;
                float cur = seeking ? seekVal : (float)posS;
                char lbl[64];
                snprintf(lbl, sizeof(lbl), "%d:%02d / %d:%02d",
                         (int)(cur / 60), (int)cur % 60, (int)(lenS / 60),
                         (int)lenS % 60);
                ImGui::SetNextItemWidth(-110);
                if (ImGui::SliderFloat("##seek", &cur, 0.0f, (float)lenS,
                                       lbl)) {
                    seeking = true;
                    seekVal = cur;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    g.playerSeek.store((long long)(seekVal * SR));
                    seeking = false;
                } else if (!ImGui::IsItemActive()) {
                    seeking = false;
                }
                if (playing && !running)
                    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                                       "start de engine om het nummer te "
                                       "horen");
            }
        }

        ImGui::EndTabItem();
        }  // einde tab meespelen

        // --- tab: stemmer ---
        tunerOn = ImGui::BeginTabItem("Stemmer");
        if (tunerOn) {
            frameNo++;
            if (running && (frameNo % 6) == 0) {
                uint32_t w = g.tringW.load(std::memory_order_acquire);
                for (int i = 0; i < 2048; i++)
                    tunerBuf[i] =
                        g.tring[(w - 2048 + i) & (EngineShared::TRING - 1)];
                float f = detectPitch(tunerBuf, 2048);
                if (f > 0.0f) {
                    tunerFreq = f;
                    tunerLastSeen = ImGui::GetTime();
                }
            }
            bool fresh = ImGui::GetTime() - tunerLastSeen < 1.5;
            if (!running) {
                ImGui::TextDisabled("start de engine om te stemmen");
            } else if (fresh && tunerFreq > 0.0f) {
                static const char* names[12] = {"C",  "C#", "D",  "D#",
                                                "E",  "F",  "F#", "G",
                                                "G#", "A",  "A#", "B"};
                float semis = 12.0f * log2f(tunerFreq / 440.0f);
                int midi = (int)lrintf(semis) + 69;
                float cents = (semis - (midi - 69)) * 100.0f;
                char note[16];
                snprintf(note, sizeof(note), "%s%d", names[((midi % 12) + 12) % 12],
                         midi / 12 - 1);
                bool inTune = fabsf(cents) < 5.0f;
                ImGui::SetWindowFontScale(2.4f);
                ImGui::TextColored(inTune ? ImVec4(0.3f, 0.95f, 0.4f, 1.0f)
                                          : ImVec4(0.95f, 0.85f, 0.3f, 1.0f),
                                   "%s", note);
                ImGui::SetWindowFontScale(1.0f);
                ImGui::SameLine();
                ImGui::Text("  %.1f Hz  %+.0f cent", tunerFreq, cents);
                float frac = (cents + 50.0f) / 100.0f;
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                ImGui::PushStyleColor(
                    ImGuiCol_PlotHistogram,
                    inTune ? ImVec4(0.3f, 0.85f, 0.4f, 1.0f)
                           : ImVec4(0.85f, 0.7f, 0.25f, 1.0f));
                ImGui::ProgressBar(frac, ImVec2(-1, 0), "");
                ImGui::PopStyleColor();
                ImGui::TextDisabled("        laag <- | -> hoog   (midden = "
                                    "zuiver)");
            } else {
                ImGui::TextDisabled("speel een losse snaar...");
            }
            ImGui::EndTabItem();
        }

        // --- tab: community (GitHub-lijst) ---
        if (ImGui::BeginTabItem("Community (GitHub)")) {
            if (ImGui::Button("Lijst verversen")) fetchCatalogAsync(catalogCache);
            ImGui::SameLine();
            if (ImGui::Button("Map openen"))
                ShellExecuteW(nullptr, L"open", modelsDir.wstring().c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            {
                std::string ns = g.getNetStatus();
                if (g.catFetching.load() || g.downloading.load())
                    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "%s",
                                       ns.c_str());
                else if (!ns.empty())
                    ImGui::TextDisabled("%s", ns.c_str());
                else
                    ImGui::TextDisabled("communitycollectie (NAM_models)");
            }
            ImGui::SetNextItemWidth(-110);
            ImGui::InputTextWithHint("##catfilter", "zoek (bv. fender, 5150)",
                                     catFilter, sizeof(catFilter));
            std::vector<const std::string*> view;
            {
                std::lock_guard<std::mutex> lk(g.catMutex);
                std::string needle = catFilter;
                for (auto& c : needle) c = (char)tolower((unsigned char)c);
                for (auto& p : g.catalog) {
                    if (!needle.empty()) {
                        std::string low = p;
                        for (auto& c : low) c = (char)tolower((unsigned char)c);
                        if (low.find(needle) == std::string::npos) continue;
                    }
                    view.push_back(&p);
                    if (view.size() >= 2000) break;  // UI-limiet, filter verder
                }
            }
            if (view.empty()) {
                ImGui::TextDisabled(
                    "geen lijst - klik 'Lijst verversen' (internet nodig)");
            } else {
                ImGui::TextDisabled("%d resultaten", (int)view.size());
                ImGui::BeginChild("catlist", ImVec2(0, 180), true);
                ImGuiListClipper clip;
                clip.Begin((int)view.size());
                while (clip.Step()) {
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                        const std::string& p = *view[i];
                        std::string fname = fs::path(p).filename().string();
                        bool have = fs::exists(modelsDir / fname);
                        ImGui::PushID(i);
                        if (have) {
                            ImGui::TextDisabled("al binnen");
                        } else if (ImGui::SmallButton("ophalen")) {
                            downloadModelAsync(p, modelsDir);
                        }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(p.c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
            }
            ImGui::TextDisabled(
                "vaste communitycollectie; voor de grote bibliotheek zie "
                "het TONE3000-tabblad");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
        }  // einde tabbar
        ImGui::EndChild();  // card_tabs

        // nieuwe downloads automatisch in de dropdown tonen
        if (g.modelsChanged.exchange(false)) {
            std::wstring keep =
                models.empty() ? L"" : models[selModel].path;
            models = scanModels(modelsDir);
            for (int i = 0; i < (int)models.size(); i++)
                if (models[i].path == keep) selModel = i;
            if (selModel >= (int)models.size()) selModel = 0;
            irs = scanIrs();
            if (selIr >= (int)irs.size()) selIr = -1;
            applySort();
            ensureMeta();
            saveMeta();
        }
        // metadata van downloadthreads verwerken
        {
            std::lock_guard<std::mutex> lk(gMetaPendMutex);
            if (!gMetaPending.empty()) {
                for (auto& [fn, mm] : gMetaPending) {
                    ModelMeta& dst = metaDb[fn];
                    if (!mm.make.empty()) dst.make = mm.make;
                    if (!mm.maker.empty()) dst.maker = mm.maker;
                    if (!mm.source.empty()) dst.source = mm.source;
                    for (auto& t : mm.tags)
                        if (std::find(dst.tags.begin(), dst.tags.end(), t) ==
                            dst.tags.end())
                            dst.tags.push_back(t);
                    if (dst.style.empty())
                        for (auto& t : dst.tags) {
                            std::string tl = lowerStr(t);
                            if (tl == "clean") dst.style = "Clean";
                            else if (tl.find("crunch") != std::string::npos)
                                dst.style = "Crunch";
                            else if (tl.find("metal") != std::string::npos ||
                                     tl.find("high gain") !=
                                         std::string::npos ||
                                     tl.find("djent") != std::string::npos ||
                                     tl.find("hard rock") !=
                                         std::string::npos)
                                dst.style = "High-gain";
                        }
                }
                gMetaPending.clear();
                ensureMeta();
                saveMeta();
            }
        }

        // --- statusregel onderaan ---
        if (running) {
            double padMs = g.padAvgFrames.load() * 1000.0 / SR;
            ImGui::Text("latency: %.1f ms software (3.0 in + %.1f uit)",
                        3.0 + padMs, padMs);
            ImGui::Text("NAM: %.2f ms/blok (max %.2f)  |  underruns: %ld  |  "
                        "driftcorrectie: %ld frames  |  late wakeups: %ld",
                        g.dspAvgUs.load() / 1000.0, g.dspMaxUs.load() / 1000.0,
                        g.underruns.load(), g.driftCorr.load(),
                        g.lateWakeups.load());
            double msr = g.modelSR.load();
            if (msr > 0 && (int)msr != SR)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                   "let op: model verwacht %.0f Hz", msr);
        } else {
            ImGui::TextDisabled("engine gestopt - Windows-audio is weer vrij");
        }

        ImGui::EndChild();  // content
        ImGui::End();
        ImGui::Render();
        const float clear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
        gCtx->OMSetRenderTargets(1, &gRTV, nullptr);
        gCtx->ClearRenderTargetView(gRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        gSwap->Present(1, 0);
    }

    stopEngine();
    if (gLogoSRV) gLogoSRV->Release();
    for (auto& t : gCatArt)
        if (t) t->Release();
    if (epVol) epVol->Release();
    {
        nam::DSP* r = g.retiredModel.exchange(nullptr);
        if (r) delete r;
        nam::DSP* pm = g.pendingModel.exchange(nullptr);
        if (pm) delete pm;
        PlayerTrack* rt = g.retiredTrack.exchange(nullptr);
        if (rt) delete rt;
        PlayerTrack* pt = g.pendingTrack.exchange(nullptr);
        if (pt) delete pt;
        IrData* ri = g.retiredIr.exchange(nullptr);
        if (ri) delete ri;
        IrData* pi = g.pendingIr.exchange(nullptr);
        if (pi) delete pi;
    }
    MFShutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (gRTV) gRTV->Release();
    if (gSwap) gSwap->Release();
    if (gCtx) gCtx->Release();
    if (gDev) gDev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
