// Zet endpoint-volume en mute: probe_vol <substring> <vol%> <mute 0|1>
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "ole32.lib")

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("gebruik: probe_vol <substring> <vol%%> <mute 0|1>\n");
        return 1;
    }
    std::wstring needle;
    for (char* a = argv[1]; *a; a++) needle += (wchar_t)towlower(*a);
    float vol = (float)atoi(argv[2]) / 100.0f;
    BOOL mute = atoi(argv[3]) ? TRUE : FALSE;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&enu);
    IMMDeviceCollection* coll = nullptr;
    enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
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
        if (lower.find(needle) != std::wstring::npos) {
            IAudioEndpointVolume* v = nullptr;
            if (SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume),
                                      CLSCTX_ALL, nullptr, (void**)&v))) {
                v->SetMasterVolumeLevelScalar(vol, nullptr);
                v->SetMute(mute, nullptr);
                wprintf(L"%s -> vol %.0f%%, mute %d\n", name.c_str(),
                        vol * 100.0f, mute);
                v->Release();
            }
            d->Release();
            break;
        }
        d->Release();
    }
    coll->Release();
    enu->Release();
    return 0;
}
