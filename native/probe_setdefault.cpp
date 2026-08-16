// Probe: werkt IPolicyConfig::SetDefaultEndpoint op deze Windows-build?
// Zet het default-renderdevice kort op een ander endpoint en herstelt direct.
// Build: cl /nologo /O2 /EHsc /std:c++17 probe_setdefault.cpp /link ole32.lib
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

static std::wstring name(IMMDevice* dev) {
    std::wstring n = L"?";
    IPropertyStore* st = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &st))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(st->GetValue(PKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR)
            n = pv.pwszVal;
        PropVariantClear(&pv);
        st->Release();
    }
    return n;
}

static std::wstring defaultId(IMMDeviceEnumerator* enu) {
    std::wstring out;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(enu->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            out = id;
            CoTaskMemFree(id);
        }
        wprintf(L"  default: %s\n", name(dev).c_str());
        dev->Release();
    }
    return out;
}

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator* enu = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&enu)))
        return 1;
    printf("voor:\n");
    std::wstring orig = defaultId(enu);
    // ander actief endpoint zoeken
    std::wstring other;
    IMMDeviceCollection* coll = nullptr;
    enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n && other.empty(); i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev))) continue;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            if (orig != id) other = id;
            CoTaskMemFree(id);
        }
        dev->Release();
    }
    coll->Release();
    if (other.empty()) {
        printf("geen tweede endpoint\n");
        return 1;
    }
    IPolicyConfig* pc = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr,
                                  CLSCTX_ALL, __uuidof(IPolicyConfig),
                                  (void**)&pc);
    printf("CoCreateInstance(PolicyConfig): 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    hr = pc->SetDefaultEndpoint(other.c_str(), eConsole);
    printf("SetDefaultEndpoint(other, eConsole): 0x%08lx\n",
           (unsigned long)hr);
    hr = pc->SetDefaultEndpoint(other.c_str(), eMultimedia);
    printf("SetDefaultEndpoint(other, eMultimedia): 0x%08lx\n",
           (unsigned long)hr);
    printf("na omzetten:\n");
    std::wstring now = defaultId(enu);
    bool switched = (now == other);
    // herstellen
    pc->SetDefaultEndpoint(orig.c_str(), eConsole);
    hr = pc->SetDefaultEndpoint(orig.c_str(), eMultimedia);
    printf("herstel: 0x%08lx\n", (unsigned long)hr);
    printf("na herstel:\n");
    std::wstring back = defaultId(enu);
    printf("\nresultaat: omzetten %s, herstel %s\n",
           switched ? "GELUKT" : "MISLUKT",
           back == orig ? "GELUKT" : "MISLUKT");
    pc->Release();
    enu->Release();
    return 0;
}
