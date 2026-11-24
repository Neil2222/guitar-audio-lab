# Onderzoek: van kale passthrough tot gitaarversterker

Het onderzoek achter Guitar Audio Lab, per fase. De uitwerking staat in de fase-documenten in deze map; hier de status,
de kernbevindingen van fase 1 en de diagnostische scripts en tools.

## Status

- **Fase 1 — Inventarisatie: afgerond** (2026-08-15), zie [fase1-inventarisatie.md](fase1-inventarisatie.md)
- **Fase 2/3 — Passthrough + latency-optimalisatie: afgerond** (2026-08-15), zie
  [fase2-passthrough.md](fase2-passthrough.md) (Python/PortAudio, ~17,5 ms)
  en [fase3-native.md](fase3-native.md) (native C++/WASAPI,
  **~4,8 ms software, 0 underruns**). Speeltest: `native\passthrough_ll.exe --seconds 300`
- **Fase 4 — Amp modelling (NAM): eerste integratie werkt** (2026-08-15), zie
  [fase4-nam.md](fase4-nam.md). `native\guitar_amp.exe` draait NAM-modellen
  in de 4,8 ms-pipeline (NAM kost 0,13 ms/blok). Vier echte captures in `models/`.
  Speeltest: `native\guitar_amp.exe --seconds 300 --model "models\Tim R JCM2000 Crunch.nam"`
- **Fase 5 — GUI: v2.0** (2026-08-15), zie [fase5-gui.md](fase5-gui.md).
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
- **Fase 6/7 — Systeemaudio meespelen: werkt** (2026-08-16), zie
  [fase6-onderzoek-systeemaudio.md](fase6-onderzoek-systeemaudio.md) (de opties) en
  [fase7-systeemaudio.md](fase7-systeemaudio.md) (de bouw): Windows-audio via loopback op een tweede
  endpoint in de engine gemixt, het gitaarpad ongewijzigd.
- **Kalibratie en volume gelijk**: het ingangsniveau van de kabel in dBu bij 0 dBFS, zodat elk model het
  niveau krijgt waarvoor het is opgenomen (als het model dat vermeldt; anders een aanname van 15 dBu), en een
  output-normalisatie zodat modellen even hard klinken. Daarbij een adaptieve outputbuffer.

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
