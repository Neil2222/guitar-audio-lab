// Probe: IAudioClient3 GetSharedModeEnginePeriod voor Rocksmith-in en Realtek-out.
// Build: cl /nologo /O2 /EHsc /std:c++17 probe_iac3.cpp /link ole32.lib
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "ole32.lib")

static void probe(IMMDeviceEnumerator* enu, EDataFlow flow, const wchar_t* needle) {
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(enu->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) return;
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev))) continue;
        IPropertyStore* store = nullptr;
        std::wstring name;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR)
                name = pv.pwszVal;
            PropVariantClear(&pv);
            store->Release();
        }
        std::wstring lower = name;
        for (auto& c : lower) c = towlower(c);
        if (lower.find(needle) != std::wstring::npos) {
            IAudioClient3* c3 = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL,
                                        nullptr, (void**)&c3))) {
                WAVEFORMATEX* mix = nullptr;
                c3->GetMixFormat(&mix);
                UINT32 def = 0, fund = 0, mn = 0, mx = 0;
                HRESULT hr = c3->GetSharedModeEnginePeriod(mix, &def, &fund,
                                                           &mn, &mx);
                wprintf(L"%s\n", name.c_str());
                if (SUCCEEDED(hr)) {
                    printf("  mix: %lu Hz %u ch, engine-periodes: default=%u "
                           "fundamental=%u min=%u max=%u frames "
                           "(min=%.2f ms)\n",
                           mix->nSamplesPerSec, mix->nChannels, def, fund, mn,
                           mx, mn * 1000.0 / mix->nSamplesPerSec);
                } else {
                    printf("  GetSharedModeEnginePeriod faalde: 0x%08lx\n",
                           (unsigned long)hr);
                }
                if (mix) CoTaskMemFree(mix);
                c3->Release();
            } else {
                wprintf(L"%s\n  IAudioClient3 niet beschikbaar\n", name.c_str());
            }
        }
        dev->Release();
    }
    coll->Release();
}

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enu)))
        return 1;
    probe(enu, eCapture, L"rocksmith");
    probe(enu, eRender, L"realtek digital");
    return 0;
}
