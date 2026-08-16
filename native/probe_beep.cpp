// Claimt de Realtek S/PDIF EXCLUSIVE (zoals de engine) en speelt N korte
// piepjes (520 Hz, 0.25). Gebruik: probe_beep <aantal_piepjes>
// Doel: hoorbaarheidstest van de exclusive claim onder verschillende
// device-condities (default/niet-default, koud/warm).
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
    int beeps = argc > 1 ? atoi(argv[1]) : 1;
    int onMs = argc > 2 ? atoi(argv[2]) : 400;
    int offMs = argc > 3 ? atoi(argv[3]) : 250;
    int leadMs = argc > 4 ? atoi(argv[4]) : 0;
    timeBeginPeriod(1);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&enu);
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
        if (lower.find(L"realtek digital") != std::wstring::npos) dev = d;
        else d->Release();
    }
    coll->Release();
    if (!dev) {
        printf("S/PDIF niet gevonden\n");
        return 1;
    }
    IAudioClient* c = nullptr;
    dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c);
    WAVEFORMATEXTENSIBLE fmt = pcm16(2);
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 200000, 0,
                               (WAVEFORMATEX*)&fmt, nullptr);
    for (int retry = 0; hr == AUDCLNT_E_DEVICE_IN_USE && retry < 5; retry++) {
        Sleep(200);
        c->Release();
        c = nullptr;
        dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                      (void**)&c);
        hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 200000, 0,
                           (WAVEFORMATEX*)&fmt, nullptr);
    }
    printf("exclusive init: 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    UINT32 bufFrames = 0;
    c->GetBufferSize(&bufFrames);
    IAudioRenderClient* ren = nullptr;
    c->GetService(__uuidof(IAudioRenderClient), (void**)&ren);
    // priming: stilte
    BYTE* pb = nullptr;
    ren->GetBuffer(288, &pb);
    memset(pb, 0, 288 * 4);
    ren->ReleaseBuffer(288, 0);
    c->Start();

    // aanloopstilte (S/PDIF-receiver-lock) + piepjes met instelbaar ritme
    double phase = 0.0;
    const long lead = (long)((double)leadMs / 1000.0 * SR);
    const long cyc = (long)((double)(onMs + offMs) / 1000.0 * SR);
    const long onN = (long)((double)onMs / 1000.0 * SR);
    long total = lead + cyc * beeps;
    long pos = 0;
    long minPad = 1 << 30;
    while (pos < total) {
        UINT32 pad = 0;
        if (FAILED(c->GetCurrentPadding(&pad))) break;
        if ((long)pad < minPad) minPad = pad;
        if (pad < 288) {
            UINT32 need = 288 - pad;
            if (SUCCEEDED(ren->GetBuffer(need, &pb))) {
                int16_t* s = (int16_t*)pb;
                for (UINT32 i = 0; i < need; i++) {
                    long p = pos + i;
                    int16_t v = 0;
                    if (p >= lead && (p - lead) % cyc < onN) {
                        v = (int16_t)(sin(phase) * 0.25 * 32767);
                        phase += 2.0 * 3.14159265358979 * 520.0 / SR;
                    }
                    s[2 * i] = v;
                    s[2 * i + 1] = v;
                }
                ren->ReleaseBuffer(need, 0);
                pos += need;
            }
        }
        Sleep(1);
    }
    Sleep(300);
    c->Stop();
    printf("klaar: %d piepjes gespeeld, min padding=%ld (device consumeerde: "
           "%s)\n",
           beeps, minPad, minPad < 288 ? "JA" : "NEE");
    ren->Release();
    c->Release();
    dev->Release();
    enu->Release();
    timeEndPeriod(1);
    return 0;
}
