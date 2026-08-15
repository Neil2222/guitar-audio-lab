// Probe voor de Realtek ASIO-driver: kanalen, buffer sizes, latencies.
// Minimale IASIO-interfacedeclaratie (vtable-layout volgens Steinberg iasiodrv.h);
// we linken niet tegen de ASIO SDK.
// Build: cl /nologo /O2 /EHsc /std:c++17 probe_asio.cpp /link ole32.lib
#include <windows.h>
#include <cstdio>

#pragma comment(lib, "ole32.lib")

typedef long ASIOBool;
typedef long ASIOError;
typedef double ASIOSampleRate;

struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    long type;  // ASIOSampleType
    char name[32];
};

// IASIO erft van IUnknown; volgorde van virtuele methodes = SDK iasiodrv.h.
struct IASIO : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInput, long* numOutput) = 0;
    virtual ASIOError getLatencies(long* inputLat, long* outputLat) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize,
                                    long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(void* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(void* sPos, void* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(void* bufferInfos, long numChannels,
                                    long bufferSize, void* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

static const CLSID CLSID_RtkASIO = {
    0xA80362FF, 0xCE76, 0x4DD9, {0x87, 0x4A, 0x70, 0x4C, 0x57, 0xBF, 0x0D, 0x6A}};

int main() {
    // Apartment-threaded: de driver staat als ThreadingModel=Apartment geregistreerd
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    IASIO* asio = nullptr;
    // ASIO-drivers gebruiken hun CLSID ook als IID (Steinberg-conventie)
    HRESULT hr = CoCreateInstance(CLSID_RtkASIO, nullptr, CLSCTX_INPROC_SERVER,
                                  CLSID_RtkASIO, (void**)&asio);
    if (FAILED(hr)) {
        printf("CoCreateInstance faalde: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    if (!asio->init(GetDesktopWindow())) {
        char err[256] = {0};
        asio->getErrorMessage(err);
        printf("init faalde: %s\n", err);
        asio->Release();
        return 1;
    }
    char name[64] = {0};
    asio->getDriverName(name);
    printf("driver: %s (versie %ld)\n", name, asio->getDriverVersion());

    long nin = 0, nout = 0;
    asio->getChannels(&nin, &nout);
    printf("kanalen: in=%ld out=%ld\n", nin, nout);

    long mn = 0, mx = 0, pref = 0, gran = 0;
    asio->getBufferSize(&mn, &mx, &pref, &gran);
    printf("buffer: min=%ld max=%ld preferred=%ld granulariteit=%ld frames\n",
           mn, mx, pref, gran);

    ASIOSampleRate sr = 0;
    asio->getSampleRate(&sr);
    printf("sample rate: %.0f Hz  (48k ok: %s, 96k ok: %s)\n", sr,
           asio->canSampleRate(48000.0) == 0 ? "ja" : "nee",
           asio->canSampleRate(96000.0) == 0 ? "ja" : "nee");

    long lin = 0, lout = 0;
    asio->getLatencies(&lin, &lout);
    printf("latencies (zonder actieve buffers): in=%ld out=%ld samples "
           "(%.2f / %.2f ms @48k)\n",
           lin, lout, lin / 48.0, lout / 48.0);

    for (long i = 0; i < nin + nout; i++) {
        ASIOChannelInfo ci{};
        ci.channel = i < nin ? i : i - nin;
        ci.isInput = i < nin ? 1 : 0;
        if (asio->getChannelInfo(&ci) == 0)
            printf("  %s %ld: '%s' (sampletype %ld)\n",
                   ci.isInput ? "in " : "out", ci.channel, ci.name, ci.type);
    }
    asio->Release();
    return 0;
}
