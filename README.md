# Guitar Audio Lab

Onderzoeksproject: elektrische gitaar → Rocksmith Real Tone USB Cable → Windows 11 →
realtime processing → optische S/PDIF-output, met zo laag mogelijke latency.

Doel op termijn: eigen lichte gitaarsoftware rond bestaande open-source componenten
(Neural Amp Modeler, cabinet-IR's, e.d.). Eerst bewijzen dat de kale audio-pipeline werkt.

## Status

- **Fase 1 — Inventarisatie: afgerond** (2026-08-15), zie [docs/fase1-inventarisatie.md](docs/fase1-inventarisatie.md)
- **Fase 2/3 — Passthrough + latency-optimalisatie: afgerond** (2026-08-15), zie
  [docs/fase2-passthrough.md](docs/fase2-passthrough.md) (Python/PortAudio, ~17,5 ms)
  en [docs/fase3-native.md](docs/fase3-native.md) (native C++/WASAPI,
  **~4,8 ms software, 0 underruns**). Speeltest: `native\passthrough_ll.exe --seconds 300`
- **Fase 4 — Amp modelling (NAM): eerste integratie werkt** (2026-08-15), zie
  [docs/fase4-nam.md](docs/fase4-nam.md). `native\guitar_amp.exe` draait NAM-modellen
  in de 4,8 ms-pipeline (NAM kost 0,13 ms/blok). Vier echte captures in `models/`.
  Speeltest: `native\guitar_amp.exe --seconds 300 --model "models\Tim R JCM2000 Crunch.nam"`
- **Fase 5 — GUI: v2.0** (2026-08-15), zie [docs/fase5-gui.md](docs/fase5-gui.md).
  v1.5: outputdevice-keuze (elk render-device, incl. Bluetooth via shared-fallback).
  v2.0: **professionele restyling** (Segoe UI, donker cardthema) en een echte
  **bibliotheekbrowser** met categorieën, favorieten, zoeken en detailpaneel.
  v2.1: **presets** (complete instelling incl. model en outputdevice, `presets.json`)
  en compacte controls op één regel. v2.3: **metadata-bibliotheek** (zoeken op
  "fender clean", ratings, TONE3000-tags). v2.4: **cabinet-IR's** (map `irs\`,
  Cabinet-dropdown, IR in presets, TONE3000-IR-downloads).
  **`native\guitar_amp_gui.exe`**: zijbalk (devices/status/levels), modelkeuze met
  hot-swap en **categorieën**, **noise gate**, 3-bands EQ, stemmer (YIN),
  meespeel-speler (Media Foundation), **TONE3000-integratie** (OAuth PKCE, zoeken op
  populariteit, 1-klik-download) en de GitHub-communitylijst.
  Open: cabinet-IR, presets, ASRC, artwork (ComfyUI via NeilChat)

## Kernbevindingen Fase 1 (samenvatting)

- **Rocksmith Real Tone Cable** (`USB\VID_12BA&PID_00FF`): USB Audio Class-device op de
  generieke Microsoft `usbaudio`-driver. **Mono, max 48 kHz**, capture-only.
  Werkt in WASAPI Shared én WASAPI **Exclusive** (1ch @ 44.1/48 kHz).
- **Optische uitgang**: "Realtek Digital Output (Realtek USB Audio)" — de onboard audio is
  een **USB-gebaseerde Realtek-codec** (`VID_26CE&PID_0A08`, driver 6.4.0.2395).
  Stereo, 24-bit, ondersteunt WASAPI Exclusive tot 96 kHz. Is de huidige Windows-default output.
- **Windows shared mix**: beide endpoints staan op 48 kHz → geen resampling nodig als wij
  ook op 48 kHz werken.
- **ASIO**: alleen "Realtek ASIO" aanwezig (alleen voor het Realtek-device, niet voor de
  Rocksmith-input). Geen ASIO4ALL/FlexASIO geïnstalleerd.
- **Beste route voor Fase 2**: WASAPI (Shared eerst, daarna Exclusive) via een
  PortAudio-gebaseerde passthrough, 48 kHz, mono in → stereo uit.

## Bouwen (Windows)

Vereist: Visual Studio 2022 (C++-workload). Eenmalig:

```powershell
git clone --recurse-submodules https://github.com/Neil2222/guitar-audio-lab.git
cd guitar-audio-lab\third_party\NeuralAmpModelerCore
mkdir build_msvc; cd build_msvc
# vanuit een "x64 Native Tools"-prompt:
cl /std:c++20 /O2 /arch:AVX2 /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DNAM_ENABLE_A2_FAST ^
   /I.. /I..\Dependencies\eigen /I..\Dependencies\nlohmann /c ..\NAM\*.cpp ..\NAM\wavenet\*.cpp
lib /out:nam.lib *.obj
```

Daarna bouwt `native\build.ps1` alle tools, inclusief `guitar_amp_gui.exe`.
Let op bij het linken tegen nam.lib: `/WHOLEARCHIVE:` is verplicht (static
initializers registreren de netwerkarchitecturen).

**Niet in de repo** (zie `.gitignore`): persoonlijke instellingen/presets,
TONE3000-tokens (`t3k.json`), en gedownloade modellen/IR's — die haal je
zelf via de app binnen (TONE3000-account met publishable key, of de
GitHub-communitylijst).

De input is momenteel nog gekoppeld aan de Rocksmith Real Tone Cable
(devicenaam-match, mono/16-bit/48 kHz); een vrije inputkeuze zoals bij de
output staat op de roadmap.

## Structuur

```
scripts/   kleine diagnostische en testscripts (uv run scripts/<naam>.py)
native/    C++ WASAPI-tools (MSVC; bouwen met native\build.ps1)
docs/      onderzoeksdocumentatie per fase
```

Scripts draaien met [uv](https://docs.astral.sh/uv/) via inline script-dependencies:
`uv run scripts/probe_devices.py` — geen aparte venv-setup nodig.

## Scripts

| Script | Doel |
|---|---|
| `scripts/probe_devices.py` | Alle audio-devices per host-API + sample-rate-support |
| `scripts/probe_wasapi_exclusive.py` | WASAPI Exclusive formaat-support voor Rocksmith-input en S/PDIF-output |
| `scripts/probe_wdmks_open.py` | WDM-KS pin-open-test (toonde intermitterende `-9996` op default output) |
| `scripts/passthrough.py` | Full-duplex passthrough in één PortAudio-stream + `--bench` matrix (dood spoor: ~50 ms vloer) |
| `scripts/split_passthrough.py` | Losse WASAPI Exclusive in/out-streams + ringbuffer via PortAudio (~17,5 ms; door native route achterhaald) |
| `native/passthrough.cpp` | Native event-driven variant; legde de 10 ms-eventbatching van de Realtek-driver bloot |
| `native/probe_iac3.cpp` | IAudioClient3 engine-periodes (dood spoor: min 10 ms) |
| `native/probe_asio.cpp` | Realtek ASIO-probe zonder SDK (dood spoor: vaste 1024-buffer) |
| `native/probe_padding.cpp` | DMA-granulariteitsmeting (de doorbraak: positie loopt per frame) |
| `native/passthrough_ll.exe` | **De winnaar**: exclusive capture-event als klok + polling render, **~4,8 ms** |
