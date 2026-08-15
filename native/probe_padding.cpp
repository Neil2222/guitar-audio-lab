// Beslissende meting: hoe fijnmazig loopt de afspeelpositie van de Realtek
// S/PDIF-output in WASAPI Exclusive *polling* mode?
// We vullen een grote buffer met stilte en samplen GetCurrentPadding met hoge
// frequentie terwijl hij leegloopt. De stapgrootte = DMA-granulariteit.
// Build: cl /nologo /O2 /EHsc /std:c++17 probe_padding.cpp /link ole32.lib winmm.lib
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

static const int SR = 48000;

static IMMDevice* findRender(IMMDeviceEnumerator* enu, const wchar_t* needle) {
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)))
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

int main() {
    timeBeginPeriod(1);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enu)))
        return 1;
    IMMDevice* dev = findRender(enu, L"realtek digital");
    if (!dev) { printf("device niet gevonden\n"); return 1; }

    IAudioClient* client = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&client))) return 1;

    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = 2;
    f.Format.nSamplesPerSec = SR;
    f.Format.wBitsPerSample = 16;
    f.Format.nBlockAlign = 4;
    f.Format.nAvgBytesPerSec = SR * 4;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 16;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    // polling mode (geen event-flag), buffer ~85 ms
    REFERENCE_TIME dur = 850000;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, dur, 0,
                                    (WAVEFORMATEX*)&f, nullptr);
    if (FAILED(hr)) {
        printf("Initialize polling exclusive faalde: 0x%08lx\n",
               (unsigned long)hr);
        return 1;
    }
    UINT32 bufFrames = 0;
    client->GetBufferSize(&bufFrames);
    printf("buffer: %u frames (%.1f ms)\n", bufFrames, bufFrames * 1000.0 / SR);

    IAudioRenderClient* ren = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&ren)))
        return 1;
    BYTE* p = nullptr;
    if (FAILED(ren->GetBuffer(bufFrames, &p))) return 1;
    memset(p, 0, (size_t)bufFrames * 4);
    ren->ReleaseBuffer(bufFrames, 0);

    LARGE_INTEGER freq, t0, t;
    QueryPerformanceFrequency(&freq);
    client->Start();
    QueryPerformanceCounter(&t0);

    UINT32 lastPad = bufFrames;
    struct Step { double tMs; long drop; };
    std::vector<Step> steps;
    steps.reserve(4096);
    for (;;) {
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) break;
        QueryPerformanceCounter(&t);
        double ms = (t.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        if (pad != lastPad) {
            steps.push_back({ms, (long)lastPad - (long)pad});
            lastPad = pad;
        }
        if (pad == 0 || ms > 300.0) break;
        Sleep(0);  // strak pollen
    }
    client->Stop();

    printf("aantal positie-updates: %zu\n", steps.size());
    printf("eerste 40 stappen (tijd ms : afname frames):\n");
    for (size_t i = 0; i < steps.size() && i < 40; i++)
        printf("  %7.2f : %ld\n", steps[i].tMs, steps[i].drop);
    if (steps.size() > 2) {
        double span = steps.back().tMs - steps.front().tMs;
        printf("gemiddeld interval: %.2f ms, gemiddelde stap: %.0f frames\n",
               span / (steps.size() - 1),
               (double)bufFrames / steps.size());
    }
    timeEndPeriod(1);
    return 0;
}
