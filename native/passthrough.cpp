// Native WASAPI Exclusive passthrough: Rocksmith (mono in) -> S/PDIF (stereo uit).
// Event-driven exclusive mode op de minimale device-periode, MMCSS "Pro Audio".
// Zelfde architectuur als scripts/split_passthrough.py, maar zonder PortAudio/Python.
//
// Build (zie native/build.ps1):
//   cl /O2 /EHsc /std:c++17 passthrough.cpp /link ole32.lib avrt.lib
//
// Gebruik:
//   passthrough.exe [--seconds N] [--period FRAMES] [--gain G]
//     --period: gevraagde periode in frames @48k (0 = minimum van het device)

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

static const int SR = 48000;

#define CHECK(hr, msg)                                                        \
    do {                                                                      \
        if (FAILED(hr)) {                                                     \
            fprintf(stderr, "FOUT %s: hr=0x%08lx\n", msg, (unsigned long)hr); \
            return false;                                                     \
        }                                                                     \
    } while (0)

// ---------------------------------------------------------------- ringbuffer
// SPSC-ring van mono float-samples. Writer = capture-thread, reader = render.
struct Ring {
    std::vector<float> buf;
    std::atomic<size_t> w{0}, r{0};

    explicit Ring(size_t n) : buf(n) {}
    size_t cap() const { return buf.size(); }
    size_t fill() const {
        size_t wr = w.load(std::memory_order_acquire);
        size_t rd = r.load(std::memory_order_acquire);
        return (wr + cap() - rd) % cap();
    }
    size_t push(const float* src, size_t n) {  // ret: geschreven
        size_t wr = w.load(std::memory_order_relaxed);
        size_t rd = r.load(std::memory_order_acquire);
        size_t free = cap() - 1 - (wr + cap() - rd) % cap();
        if (n > free) n = free;
        for (size_t i = 0; i < n; i++) buf[(wr + i) % cap()] = src[i];
        w.store((wr + n) % cap(), std::memory_order_release);
        return n;
    }
    size_t pop(float* dst, size_t n) {  // ret: gelezen
        size_t rd = r.load(std::memory_order_relaxed);
        size_t wr = w.load(std::memory_order_acquire);
        size_t avail = (wr + cap() - rd) % cap();
        if (n > avail) n = avail;
        for (size_t i = 0; i < n; i++) dst[i] = buf[(rd + i) % cap()];
        r.store((rd + n) % cap(), std::memory_order_release);
        return n;
    }
    size_t skip(size_t n) {  // reader gooit n samples weg (driftcorrectie)
        size_t rd = r.load(std::memory_order_relaxed);
        size_t wr = w.load(std::memory_order_acquire);
        size_t avail = (wr + cap() - rd) % cap();
        if (n > avail) n = avail;
        r.store((rd + n) % cap(), std::memory_order_release);
        return n;
    }
};

// ---------------------------------------------------------------- statistiek
struct Stats {
    std::atomic<long> capWakeups{0}, renWakeups{0};
    std::atomic<long> underruns{0}, dropSamples{0}, pushDropped{0};
    std::atomic<long> capErrors{0}, renErrors{0}, timeouts{0};
    std::atomic<float> peak{0.0f};
    // render-wakeup-jitter (alleen door render-thread beschreven)
    double maxIntervalMs = 0.0;
    long lateWakeups = 0;
    // capture-wakeup-jitter (alleen door capture-thread beschreven)
    double capMaxIntervalMs = 0.0;
    long capLateWakeups = 0;
    // ring-fill sampling door render-thread
    double fillSum = 0.0;
    long fillCount = 0;
    size_t fillMax = 0;
};

// ---------------------------------------------------------------- devices
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
                std::wstring name = pv.pwszVal;
                std::wstring lower = name;
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

// Kandidaat-formaten voor exclusive mode (16-bit eerst: kabel is 16-bit).
static WAVEFORMATEXTENSIBLE makeFormat(int channels, int container, int valid) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = (WORD)channels;
    f.Format.nSamplesPerSec = SR;
    f.Format.wBitsPerSample = (WORD)container;
    f.Format.nBlockAlign = (WORD)(channels * container / 8);
    f.Format.nAvgBytesPerSec = SR * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = (WORD)valid;
    f.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                                    : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

struct Endpoint {
    IAudioClient* client = nullptr;
    HANDLE event = nullptr;
    UINT32 bufferFrames = 0;
    WAVEFORMATEXTENSIBLE fmt{};
    REFERENCE_TIME defPeriod = 0, minPeriod = 0, usedPeriod = 0;
};

// Exclusive event-driven init met format-onderhandeling en alignment-retry.
static bool initExclusive(IMMDevice* dev, int channels, int requestFrames,
                          Endpoint* ep) {
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               (void**)&ep->client);
    CHECK(hr, "Activate IAudioClient");
    ep->client->GetDevicePeriod(&ep->defPeriod, &ep->minPeriod);

    const int candidates[][2] = {{16, 16}, {32, 24}, {24, 24}, {32, 32}};
    bool haveFmt = false;
    for (auto& c : candidates) {
        WAVEFORMATEXTENSIBLE f = makeFormat(channels, c[0], c[1]);
        if (ep->client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                          (WAVEFORMATEX*)&f, nullptr) == S_OK) {
            ep->fmt = f;
            haveFmt = true;
            break;
        }
    }
    if (!haveFmt) {
        fprintf(stderr, "FOUT: geen exclusive formaat gevonden\n");
        return false;
    }

    REFERENCE_TIME period =
        requestFrames > 0
            ? (REFERENCE_TIME)((10000000.0 * requestFrames) / SR + 0.5)
            : ep->minPeriod;
    if (period < ep->minPeriod) period = ep->minPeriod;

    for (int attempt = 0; attempt < 2; attempt++) {
        hr = ep->client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                    period, (WAVEFORMATEX*)&ep->fmt, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED && attempt == 0) {
            UINT32 frames = 0;
            ep->client->GetBufferSize(&frames);
            ep->client->Release();
            period = (REFERENCE_TIME)((10000000.0 * frames) / SR + 0.5);
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               (void**)&ep->client);
            CHECK(hr, "re-Activate na alignment");
            continue;
        }
        CHECK(hr, "Initialize exclusive");
        break;
    }
    ep->usedPeriod = period;
    ep->client->GetBufferSize(&ep->bufferFrames);
    ep->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = ep->client->SetEventHandle(ep->event);
    CHECK(hr, "SetEventHandle");
    return true;
}

// ---------------------------------------------------------------- conversies
static inline float sampleToFloat(const BYTE* p, int container, int valid) {
    if (container == 16) return *(const int16_t*)p / 32768.0f;
    if (container == 24) {
        int32_t v = (p[0] << 8) | (p[1] << 16) | ((int32_t)(int8_t)p[2] << 24);
        return v / 2147483648.0f;
    }
    return *(const int32_t*)p / 2147483648.0f;  // 32-bit container
}

static inline void floatToSample(float s, BYTE* p, int container) {
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    if (container == 16) {
        *(int16_t*)p = (int16_t)lrintf(s * 32767.0f);
    } else if (container == 24) {
        int32_t v = (int32_t)llrintf((double)s * 8388607.0);
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF;
    } else {
        *(int32_t*)p = (int32_t)llrintf((double)s * 2147483647.0);
    }
}

// ---------------------------------------------------------------- main
int wmain(int argc, wchar_t** argv) {
    int seconds = 20, requestFrames = 0;
    float gain = 1.0f, targetBufs = 2.0f, highBufs = 3.0f;
    for (int i = 1; i < argc - 1; i++) {
        if (!wcscmp(argv[i], L"--seconds")) seconds = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--period")) requestFrames = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--gain")) gain = (float)_wtof(argv[i + 1]);
        if (!wcscmp(argv[i], L"--target")) targetBufs = (float)_wtof(argv[i + 1]);
        if (!wcscmp(argv[i], L"--high")) highBufs = (float)_wtof(argv[i + 1]);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enu);
    if (FAILED(hr)) return 1;

    std::wstring inName, outName;
    IMMDevice* inDev = findDevice(enu, eCapture, L"rocksmith", &inName);
    IMMDevice* outDev = findDevice(enu, eRender, L"realtek digital", &outName);
    if (!inDev || !outDev) {
        fprintf(stderr, "FOUT: device niet gevonden (in=%p out=%p)\n",
                (void*)inDev, (void*)outDev);
        return 1;
    }
    wprintf(L"input : %s\noutput: %s\n", inName.c_str(), outName.c_str());
    fflush(stdout);

    Endpoint in{}, out{};
    fprintf(stderr, "init input...\n");
    if (!initExclusive(inDev, 1, requestFrames, &in)) return 1;
    fprintf(stderr, "init output...\n");
    if (!initExclusive(outDev, 2, requestFrames, &out)) return 1;

    auto periodMs = [](REFERENCE_TIME rt) { return rt / 10000.0; };
    wprintf(L"input : periode min=%.2f ms gebruikt=%.2f ms buffer=%u frames, "
            L"%d/%d bit\n",
            periodMs(in.minPeriod), periodMs(in.usedPeriod), in.bufferFrames,
            in.fmt.Format.wBitsPerSample, in.fmt.Samples.wValidBitsPerSample);
    wprintf(L"output: periode min=%.2f ms gebruikt=%.2f ms buffer=%u frames, "
            L"%d/%d bit\n",
            periodMs(out.minPeriod), periodMs(out.usedPeriod), out.bufferFrames,
            out.fmt.Format.wBitsPerSample, out.fmt.Samples.wValidBitsPerSample);

    IAudioCaptureClient* cap = nullptr;
    IAudioRenderClient* ren = nullptr;
    hr = in.client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
    if (FAILED(hr)) return 1;
    hr = out.client->GetService(__uuidof(IAudioRenderClient), (void**)&ren);
    if (FAILED(hr)) return 1;

    // ring: cap ruim; doel-fill en highwater in veelvouden van render-buffer
    const size_t target = (size_t)(targetBufs * out.bufferFrames);
    const size_t highWater = (size_t)(highBufs * out.bufferFrames);
    Ring ring(1u << 14);
    Stats st;
    std::atomic<bool> running{true};
    std::atomic<bool> primed{false};

    // eerste output-buffer met stilte vullen voor Start
    {
        BYTE* p = nullptr;
        if (SUCCEEDED(ren->GetBuffer(out.bufferFrames, &p))) {
            memset(p, 0, out.bufferFrames * out.fmt.Format.nBlockAlign);
            ren->ReleaseBuffer(out.bufferFrames, 0);
        }
    }

    std::thread capThread([&] {
        DWORD idx = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
        std::vector<float> tmp(in.bufferFrames);
        const int cont = in.fmt.Format.wBitsPerSample;
        const int valid = in.fmt.Samples.wValidBitsPerSample;
        const int stride = in.fmt.Format.nBlockAlign;
        float localPeak = 0.0f;
        LARGE_INTEGER freq, last{}, now;
        QueryPerformanceFrequency(&freq);
        const double expectMs = in.bufferFrames * 1000.0 / SR;
        long wakeups = 0;
        while (running.load(std::memory_order_relaxed)) {
            DWORD w = WaitForSingleObject(in.event, 500);
            if (w != WAIT_OBJECT_0) {
                if (running) st.timeouts++;
                continue;
            }
            st.capWakeups++;
            wakeups++;
            QueryPerformanceCounter(&now);
            if (last.QuadPart && wakeups > SR / (int)in.bufferFrames) {
                double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
                if (ms > st.capMaxIntervalMs) st.capMaxIntervalMs = ms;
                if (ms > 1.5 * expectMs) st.capLateWakeups++;
            }
            last = now;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            HRESULT h = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(h)) { st.capErrors++; continue; }
            if (frames > tmp.size()) tmp.resize(frames);
            for (UINT32 i = 0; i < frames; i++) {
                float s = sampleToFloat(data + i * stride, cont, valid) * gain;
                tmp[i] = s;
                float a = fabsf(s);
                if (a > localPeak) {
                    localPeak = a;
                    st.peak.store(a, std::memory_order_relaxed);
                }
            }
            cap->ReleaseBuffer(frames);
            size_t pushed = ring.push(tmp.data(), frames);
            if (pushed < frames) st.pushDropped += (long)(frames - pushed);
        }
        if (mm) AvRevertMmThreadCharacteristics(mm);
    });

    std::thread renThread([&] {
        DWORD idx = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
        std::vector<float> tmp(out.bufferFrames);
        const int cont = out.fmt.Format.wBitsPerSample;
        const int stride = out.fmt.Format.nBlockAlign;
        const int bytesPer = cont / 8;
        LARGE_INTEGER freq, last{}, now;
        QueryPerformanceFrequency(&freq);
        const double expectMs = out.bufferFrames * 1000.0 / SR;
        bool warmedUp = false;
        long wakeups = 0;
        while (running.load(std::memory_order_relaxed)) {
            DWORD w = WaitForSingleObject(out.event, 500);
            if (w != WAIT_OBJECT_0) {
                if (running) st.timeouts++;
                continue;
            }
            st.renWakeups++;
            wakeups++;
            warmedUp = wakeups > SR / (int)out.bufferFrames;  // na ~1 s
            QueryPerformanceCounter(&now);
            if (last.QuadPart && warmedUp) {
                double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
                if (ms > st.maxIntervalMs) st.maxIntervalMs = ms;
                if (ms > 1.5 * expectMs) st.lateWakeups++;
            }
            last = now;

            size_t fill = ring.fill();
            if (warmedUp) {
                st.fillSum += (double)fill;
                st.fillCount++;
                if (fill > st.fillMax) st.fillMax = fill;
            }
            if (fill > highWater) {  // driftcorrectie: oudste samples weg
                size_t drop = fill - target;
                ring.skip(drop);
                if (warmedUp) st.dropSamples += (long)drop;
            }
            if (!primed.load(std::memory_order_relaxed)) {
                if (ring.fill() < target) fill = 0;
                else primed.store(true);
            }

            BYTE* data = nullptr;
            if (FAILED(ren->GetBuffer(out.bufferFrames, &data))) {
                st.renErrors++;
                continue;
            }
            size_t got = primed.load(std::memory_order_relaxed)
                             ? ring.pop(tmp.data(), out.bufferFrames)
                             : 0;
            if (got < out.bufferFrames) {
                if (primed.load(std::memory_order_relaxed) && warmedUp)
                    st.underruns++;
                if (got == 0) primed.store(false);  // opnieuw prefillen
                for (size_t i = got; i < out.bufferFrames; i++) tmp[i] = 0.0f;
            }
            for (UINT32 i = 0; i < out.bufferFrames; i++) {
                floatToSample(tmp[i], data + i * stride, cont);
                memcpy(data + i * stride + bytesPer, data + i * stride, bytesPer);
            }
            ren->ReleaseBuffer(out.bufferFrames, 0);
        }
        if (mm) AvRevertMmThreadCharacteristics(mm);
    });

    out.client->Start();
    in.client->Start();
    printf("draait %d s...\n", seconds);
    Sleep(seconds * 1000);
    running = false;
    capThread.join();
    renThread.join();
    in.client->Stop();
    out.client->Stop();

    double inMs = in.bufferFrames * 1000.0 / SR;
    double outMs = 2.0 * out.bufferFrames * 1000.0 / SR;  // dubbele buffering
    double fillAvg = st.fillCount ? st.fillSum / st.fillCount : 0.0;
    double ringMs = fillAvg * 1000.0 / SR;
    printf("\n=== RESULTAAT (na 1 s warmup) ===\n");
    printf("latency  input=%.2f ms  ring=%.2f ms (fill avg %.0f, max %zu "
           "samples)  output=%.2f ms (2x buffer)\n",
           inMs, ringMs, fillAvg, st.fillMax, outMs);
    printf("geschat totaal software: %.2f ms\n", inMs + ringMs + outMs);
    printf("wakeups cap/ren=%ld/%ld  underruns=%ld  driftdrops=%ld samples "
           "(%.1f ms/min)\n",
           st.capWakeups.load(), st.renWakeups.load(), st.underruns.load(),
           st.dropSamples.load(),
           st.dropSamples.load() * 1000.0 / SR * 60.0 / seconds);
    printf("pushDropped=%ld  errors cap/ren=%ld/%ld  timeouts=%ld\n",
           st.pushDropped.load(), st.capErrors.load(), st.renErrors.load(),
           st.timeouts.load());
    printf("render-wakeup max=%.2f ms  laat(>1.5x)=%ld\n", st.maxIntervalMs,
           st.lateWakeups);
    printf("capture-wakeup max=%.2f ms  laat(>1.5x)=%ld\n", st.capMaxIntervalMs,
           st.capLateWakeups);
    float pk = st.peak.load();
    printf("signaal peak=%.1f dBFS\n", pk > 0 ? 20.0 * log10(pk) : -999.0);
    return 0;
}
