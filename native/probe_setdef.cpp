// Zet het default-renderdevice op het eerste actieve endpoint dat de
// substring (arg 1, lowercase) bevat. Rollen: console+multimedia+comms.
// Gebruik: probe_setdef <substring>
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "ole32.lib")

struct DeviceShareMode;
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8"))
    IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT,
                                                      WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*,
                                                      WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64,
                                                          PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR,
                                                   DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR,
                                                   DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, INT,
                                                       const PROPERTYKEY&,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, INT,
                                                       const PROPERTYKEY&,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
class __declspec(uuid("870af99c-171d-4f9e-af0d-e63df40c2bc9"))
    CPolicyConfigClient;

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("gebruik: probe_setdef <substring>\n");
        return 1;
    }
    std::wstring needle;
    for (char* a = argv[1]; *a; a++) needle += (wchar_t)towlower(*a);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&enu);
    IMMDeviceCollection* coll = nullptr;
    enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    UINT n = 0;
    coll->GetCount(&n);
    std::wstring id;
    for (UINT i = 0; i < n && id.empty(); i++) {
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
            LPWSTR wid = nullptr;
            if (SUCCEEDED(d->GetId(&wid))) {
                id = wid;
                CoTaskMemFree(wid);
                wprintf(L"zet default: %s\n", name.c_str());
            }
        }
        d->Release();
    }
    coll->Release();
    enu->Release();
    if (id.empty()) {
        printf("endpoint niet gevonden\n");
        return 1;
    }
    IPolicyConfig* pc = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr,
                                  CLSCTX_ALL, __uuidof(IPolicyConfig),
                                  (void**)&pc);
    if (FAILED(hr)) return 1;
    hr = pc->SetDefaultEndpoint(id.c_str(), eConsole);
    if (SUCCEEDED(hr)) hr = pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    if (SUCCEEDED(hr)) pc->SetDefaultEndpoint(id.c_str(), eCommunications);
    printf("resultaat: 0x%08lx\n", (unsigned long)hr);
    pc->Release();
    return 0;
}
