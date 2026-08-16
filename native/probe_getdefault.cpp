// Print het huidige default-renderdevice (eMultimedia).
// Build: cl /nologo /O2 /EHsc /std:c++17 probe_getdefault.cpp /link ole32.lib
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>
#include <cstdio>

#pragma comment(lib, "ole32.lib")

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&enu)))
        return 1;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(enu->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        IPropertyStore* st = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &st))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(st->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR)
                wprintf(L"default: %s\n", pv.pwszVal);
            PropVariantClear(&pv);
            st->Release();
        }
        IAudioEndpointVolume* v = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                    nullptr, (void**)&v))) {
            float vol = 0.0f;
            BOOL mute = FALSE;
            v->GetMasterVolumeLevelScalar(&vol);
            v->GetMute(&mute);
            printf("volume: %.0f%%  mute: %d\n", vol * 100.0f, mute);
            v->Release();
        }
        dev->Release();
    }
    enu->Release();
    return 0;
}
