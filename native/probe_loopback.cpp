// Probe: WASAPI loopback-gedrag terwijl de Realtek S/PDIF exclusive bezet is.
//   A1: loopback op HETZELFDE endpoint dat exclusive vastzit -> werkt dat?
//   A2: shared render op datzelfde endpoint (= wat Spotify zou doen) -> HRESULT?
//   B1: loopback op een ANDER endpoint zonder actieve renderer -> komt er data?
//   B2: idem met een stille shared renderer erbij -> komt er data?
//   C : effect van endpoint-volume/mute op het loopback-signaal (toon, RMS).
// Muziek-endpoint kiezen: eerste actieve render-endpoint zonder "realtek",
// of via argv[1] (substring, lowercase).
// Build: cl /nologo /O2 /EHsc /std:c++17 probe_loopback.cpp /link ole32.lib winmm.lib
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <timeapi.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

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
    f.dwChannelMask = ch == 1 ? SPEAKER_FRONT_CENTER
                              : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

static std::wstring devName(IMMDevice* dev) {
    std::wstring name = L"?";
    IPropertyStore* st = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &st))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(st->GetValue(PKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR)
            name = pv.pwszVal;
        PropVariantClear(&pv);
        st->Release();
    }
    return name;
}

// leest gedurende ms milliseconden alle loopback-packets; geeft frames terug,
// en (optioneel) RMS van de floatsamples
static long drainLoopback(IAudioCaptureClient* cap, int ms, int channels,
                          double* rmsOut, IAudioClient* tone,
                          IAudioRenderClient* toneRen, UINT32 toneBuf,
                          double* tonePhase) {
    LARGE_INTEGER freq, t0, t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    long total = 0;
    double sumSq = 0.0;
    long nSamp = 0;
    for (;;) {
        QueryPerformanceCounter(&t);
        if ((t.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart > ms) break;
        // toongenerator bijvullen (indien actief): 440 Hz, amplitude 0.15
        if (tone && toneRen) {
            UINT32 pad = 0;
            tone->GetCurrentPadding(&pad);
            UINT32 want = toneBuf / 2;
            if (pad < want) {
                UINT32 need = want - pad;
                BYTE* pb = nullptr;
                if (SUCCEEDED(toneRen->GetBuffer(need, &pb))) {
                    int16_t* s = (int16_t*)pb;
                    for (UINT32 i = 0; i < need; i++) {
                        int16_t v = (int16_t)(sin(*tonePhase) * 0.15 * 32767);
                        *tonePhase += 2.0 * 3.14159265358979 * 440.0 / SR;
                        s[2 * i] = v;
                        s[2 * i + 1] = v;
                    }
                    toneRen->ReleaseBuffer(need, 0);
                }
            }
        }
        for (;;) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            HRESULT h = cap->GetBuffer(&data, &frames, &flags, nullptr,
                                       nullptr);
            if (h != S_OK || frames == 0) break;
            total += frames;
            if (rmsOut && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                const float* f = (const float*)data;
                for (UINT32 i = 0; i < frames * channels; i++)
                    sumSq += (double)f[i] * f[i];
                nSamp += frames * channels;
            }
            cap->ReleaseBuffer(frames);
        }
        Sleep(5);
    }
    if (rmsOut) *rmsOut = nSamp ? sqrt(sumSq / nSamp) : 0.0;
    return total;
}

int main(int argc, char** argv) {
    timeBeginPeriod(1);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&enu)))
        return 1;

    std::wstring wanted;
    if (argc > 1) {
        char* a = argv[1];
        while (*a) wanted += (wchar_t)towlower(*a++);
    }

    // inventarisatie + selectie
    IMMDevice* spdif = nullptr;
    IMMDevice* music = nullptr;
    IMMDeviceCollection* coll = nullptr;
    enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    UINT n = 0;
    coll->GetCount(&n);
    printf("actieve render-endpoints (%u):\n", n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev))) continue;
        std::wstring name = devName(dev);
        std::wstring lower = name;
        for (auto& ch : lower) ch = towlower(ch);
        bool isSpdif = lower.find(L"realtek digital") != std::wstring::npos;
        bool isRealtek = lower.find(L"realtek") != std::wstring::npos;
        wprintf(L"  [%u] %s\n", i, name.c_str());
        if (isSpdif && !spdif) {
            spdif = dev;
            continue;
        }
        if (!music && !isRealtek &&
            (wanted.empty() || lower.find(wanted) != std::wstring::npos)) {
            music = dev;
            continue;
        }
        dev->Release();
    }
    coll->Release();
    if (!spdif) {
        printf("S/PDIF niet gevonden\n");
        return 1;
    }
    if (!music) {
        printf("geen kandidaat muziek-endpoint gevonden\n");
        return 1;
    }
    wprintf(L"\nS/PDIF (exclusive hold): %s\nmuziek-endpoint: %s\n\n",
            devName(spdif).c_str(), devName(music).c_str());

    // --- S/PDIF exclusive claimen, zoals de engine ---
    IAudioClient* exc = nullptr;
    HRESULT hr = spdif->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 (void**)&exc);
    WAVEFORMATEXTENSIBLE fmt = pcm16(2);
    hr = exc->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 200000, 0,
                         (WAVEFORMATEX*)&fmt, nullptr);
    printf("[hold] S/PDIF exclusive Initialize: 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    IAudioRenderClient* excRen = nullptr;
    exc->GetService(__uuidof(IAudioRenderClient), (void**)&excRen);
    UINT32 excBuf = 0;
    exc->GetBufferSize(&excBuf);
    BYTE* pb = nullptr;
    excRen->GetBuffer(excBuf, &pb);
    memset(pb, 0, (size_t)excBuf * 4);
    excRen->ReleaseBuffer(excBuf, 0);
    exc->Start();
    printf("[hold] S/PDIF draait exclusive (stilte)\n\n");

    // --- A1: loopback op hetzelfde endpoint ---
    {
        IAudioClient* lb = nullptr;
        hr = spdif->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&lb);
        if (SUCCEEDED(hr)) {
            WAVEFORMATEX* mix = nullptr;
            HRESULT hm = lb->GetMixFormat(&mix);
            if (SUCCEEDED(hm)) {
                hr = lb->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0,
                                    mix, nullptr);
                printf("[A1] loopback-init op exclusief bezet S/PDIF: "
                       "0x%08lx%s\n",
                       (unsigned long)hr,
                       hr == AUDCLNT_E_DEVICE_IN_USE ? " (DEVICE_IN_USE)"
                                                     : "");
                if (SUCCEEDED(hr)) {
                    IAudioCaptureClient* cap = nullptr;
                    lb->GetService(__uuidof(IAudioCaptureClient),
                                   (void**)&cap);
                    lb->Start();
                    long fr = drainLoopback(cap, 1000, mix->nChannels,
                                            nullptr, nullptr, nullptr, 0,
                                            nullptr);
                    printf("[A1] frames in 1 s: %ld\n", fr);
                    lb->Stop();
                    cap->Release();
                }
                CoTaskMemFree(mix);
            } else {
                printf("[A1] GetMixFormat faalde: 0x%08lx\n",
                       (unsigned long)hm);
            }
            lb->Release();
        }
    }

    // --- A2: shared render op hetzelfde endpoint (Spotify-scenario) ---
    {
        IAudioClient* sh = nullptr;
        hr = spdif->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&sh);
        if (SUCCEEDED(hr)) {
            WAVEFORMATEX* mix = nullptr;
            sh->GetMixFormat(&mix);
            hr = sh->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix,
                                nullptr);
            printf("[A2] shared render-init op exclusief bezet S/PDIF: "
                   "0x%08lx%s\n\n",
                   (unsigned long)hr,
                   hr == AUDCLNT_E_DEVICE_IN_USE ? " (DEVICE_IN_USE)" : "");
            CoTaskMemFree(mix);
            sh->Release();
        }
    }

    // --- B: loopback op het muziek-endpoint (terwijl S/PDIF exclusive) ---
    IAudioClient* lb = nullptr;
    IAudioCaptureClient* lbCap = nullptr;
    WAVEFORMATEX* lbMix = nullptr;
    hr = music->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                         (void**)&lb);
    lb->GetMixFormat(&lbMix);
    printf("[B] muziek-endpoint mix-format: %lu Hz, %u ch, %u bit\n",
           lbMix->nSamplesPerSec, lbMix->nChannels, lbMix->wBitsPerSample);
    REFERENCE_TIME dp = 0, mp = 0;
    lb->GetDevicePeriod(&dp, &mp);
    printf("[B] device-period: default=%.2f ms min=%.2f ms\n", dp / 10000.0,
           mp / 10000.0);
    hr = lb->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                        1000000, 0, lbMix, nullptr);
    printf("[B] loopback-init: 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    lb->GetService(__uuidof(IAudioCaptureClient), (void**)&lbCap);
    lb->Start();

    long frNoRen = drainLoopback(lbCap, 2000, lbMix->nChannels, nullptr,
                                 nullptr, nullptr, 0, nullptr);
    printf("[B1] frames in 2 s ZONDER renderer: %ld\n", frNoRen);

    // stille shared renderer erbij
    IAudioClient* sil = nullptr;
    IAudioRenderClient* silRen = nullptr;
    hr = music->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                         (void**)&sil);
    WAVEFORMATEXTENSIBLE sfmt = pcm16(2);
    hr = sil->Initialize(AUDCLNT_SHAREMODE_SHARED,
                         AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                             AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                         1000000, 0, (WAVEFORMATEX*)&sfmt, nullptr);
    printf("[B2] stille shared renderer init: 0x%08lx\n", (unsigned long)hr);
    UINT32 silBuf = 0;
    sil->GetBufferSize(&silBuf);
    sil->GetService(__uuidof(IAudioRenderClient), (void**)&silRen);
    silRen->GetBuffer(silBuf / 2, &pb);
    memset(pb, 0, (size_t)(silBuf / 2) * 4);
    silRen->ReleaseBuffer(silBuf / 2, 0);
    sil->Start();
    double dummyPhase = 0.0;
    long frRen = drainLoopback(lbCap, 2000, lbMix->nChannels, nullptr, sil,
                               silRen, silBuf, &dummyPhase);
    printf("[B2] frames in 2 s MET stille renderer: %ld\n\n", frRen);

    // --- C: volume/mute-effect op het loopback-signaal ---
    IAudioEndpointVolume* vol = nullptr;
    hr = music->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                         (void**)&vol);
    float origVol = 1.0f;
    BOOL origMute = FALSE;
    if (SUCCEEDED(hr)) {
        vol->GetMasterVolumeLevelScalar(&origVol);
        vol->GetMute(&origMute);
        printf("[C] huidig endpoint-volume: %.0f%%, mute=%d\n",
               origVol * 100.0f, origMute);
    }
    double phase = 0.0, rms = 0.0;
    // toon aan, huidig volume
    drainLoopback(lbCap, 300, lbMix->nChannels, nullptr, sil, silRen, silBuf,
                  &phase);  // aanloop
    drainLoopback(lbCap, 1000, lbMix->nChannels, &rms, sil, silRen, silBuf,
                  &phase);
    printf("[C] RMS met toon, volume %.0f%%: %.5f\n", origVol * 100.0f, rms);
    if (vol) {
        vol->SetMute(TRUE, nullptr);
        drainLoopback(lbCap, 300, lbMix->nChannels, nullptr, sil, silRen,
                      silBuf, &phase);
        drainLoopback(lbCap, 1000, lbMix->nChannels, &rms, sil, silRen,
                      silBuf, &phase);
        printf("[C] RMS met toon, GEMUTE: %.5f\n", rms);
        vol->SetMute(origMute, nullptr);
        vol->SetMasterVolumeLevelScalar(0.10f, nullptr);
        drainLoopback(lbCap, 300, lbMix->nChannels, nullptr, sil, silRen,
                      silBuf, &phase);
        drainLoopback(lbCap, 1000, lbMix->nChannels, &rms, sil, silRen,
                      silBuf, &phase);
        printf("[C] RMS met toon, volume 10%%: %.5f\n", rms);
        vol->SetMasterVolumeLevelScalar(origVol, nullptr);
        vol->SetMute(origMute, nullptr);
        printf("[C] volume/mute hersteld naar %.0f%%/mute=%d\n",
               origVol * 100.0f, origMute);
        vol->Release();
    }

    sil->Stop();
    silRen->Release();
    sil->Release();
    lb->Stop();
    lbCap->Release();
    lb->Release();
    CoTaskMemFree(lbMix);
    exc->Stop();
    excRen->Release();
    exc->Release();
    spdif->Release();
    music->Release();
    enu->Release();
    timeEndPeriod(1);
    printf("\nklaar\n");
    return 0;
}
