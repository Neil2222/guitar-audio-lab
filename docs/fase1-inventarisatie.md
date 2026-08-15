# Fase 1 — Inventarisatie (2026-08-15)

Alle gegevens hieronder zijn daadwerkelijk uitgelezen van deze pc (Windows 11 Pro
10.0.26200) via PowerShell (PnP/WMI/register) en Python 3.13 + `sounddevice`
(PortAudio). Geen aannames, tenzij expliciet gemarkeerd.

## 1. Rocksmith Real Tone Cable

| Eigenschap | Waarde |
|---|---|
| USB-ID | `USB\VID_12BA&PID_00FF` (samengesteld device: audio + HID) |
| Windows-naam | "Microfoon (Rocksmith USB Guitar Adapter)" |
| Driver | Microsoft generieke `usbaudio` (WDM), 10.0.26100.8972 — **geen eigen driver, dus ook geen eigen ASIO** |
| Kanalen | **1 (mono)** — logisch, gitaar is mono |
| Sample rates (WDM-KS, echte hardwarerates) | 22.05 / 32 / 44.1 / **48 kHz** — geen hogere rates |
| Windows shared-format | 1ch, 48 kHz, 16-bit |
| WASAPI Shared | OK @ 48 kHz |
| WASAPI Exclusive | **OK**, 1ch @ 44.1 en 48 kHz (int16/int24/int32/float32 geaccepteerd door PortAudio-conversie; native is vrijwel zeker 16-bit) |

De rates die MME/DirectSound "ondersteunen" (t/m 192 kHz) zijn schijn: die API's
resamplen alles. WDM-KS toont de echte hardwarecapaciteiten.

## 2. Optische S/PDIF-output

| Eigenschap | Waarde |
|---|---|
| Endpoint | "Realtek Digital Output (Realtek USB Audio)" — **huidige Windows-default output** |
| Hardware | Onboard audio is een **USB-Realtek-codec**: `USB\VID_26CE&PID_0A08` (geen klassieke HD Audio-bus). VID 26CE = moederbordfabrikant-implementatie |
| Driver | Realtek Semiconductor, `RtkUsbAD_2395`, versie 6.4.0.2395 (10-7-2024) |
| Windows shared-format | 2ch, 48 kHz, 24-bit |
| WASAPI Exclusive | OK: 2ch @ 44.1 / 48 / 96 kHz, alle geteste bit depths |
| WDM-KS rates | 44.1 / 48 / 88.2 / 96 / 192 kHz |

Zelfde fysieke Realtek-device biedt ook: Luidsprekers (8ch), Hoofdtelefoon, 2× Microfoon
(momenteel niet actief/relevant).

## 3. Overige audio-devices (context)

- NVIDIA HDMI/DP-audio (VG248, LG ULTRAGEAR, Philips FTV) — monitors/TV
- Logitech G430 headset (USB), Blue Snowball mic (USB)
- Bluetooth: Sony WH-1000XM6, Bose Open Earbuds Ultra (niet verbonden tijdens meting)
- NVIDIA Virtual Audio Device, AMD Streaming Audio Device (virtueel)

## 4. Audio-API's en drivers

| API | Status op dit systeem |
|---|---|
| WASAPI Shared | Beschikbaar; beide relevante endpoints op 48 kHz → **geen resampling** in de mix |
| WASAPI Exclusive | **Werkt op zowel Rocksmith-input als S/PDIF-output** (getest via PortAudio-formatchecks) |
| WDM-KS | Beschikbaar, toont echte hardwareformaten |
| MME / DirectSound | Beschikbaar, hoge latency, alleen relevant als fallback |
| ASIO | Alleen **Realtek ASIO** ({A80362FF-...}) — bedient alleen het Realtek-device. De Rocksmith-kabel heeft géén ASIO. ASIO4ALL/FlexASIO niet geïnstalleerd |

Consequentie: één native ASIO-pad voor input+output bestaat niet. Kandidaat-routes:

1. **WASAPI Exclusive in + WASAPI Exclusive out** (via PortAudio) — geen extra software nodig
2. FlexASIO (WASAPI-backend) — zelfde onderliggende techniek, handig als we later
   bestaande DAW/NAM-standalone-software willen gebruiken
3. ASIO4ALL (WDM-KS-backend) — alternatief voor route 2

## 5. Systeeminstellingen

- Energieplan: **Gebalanceerd** — bij dropouts in Fase 2/3 is "Hoge prestaties" of
  min. processor state verhogen een kandidaat-optimalisatie (nog niets gewijzigd).

## 6. Latency-verwachting (theoretisch, nog te meten)

Vaste, onvermijdelijke componenten:
- USB Audio Class 1 framing: 1 ms per USB-frame + device-buffering (Rocksmith-kant,
  typisch enkele ms totaal)
- ADC/DAC + S/PDIF-transmissie + DAC in de geluidsinstallatie: ~1–2 ms

Variabel (waar wij aan kunnen draaien):
- WASAPI Shared: standaard 10 ms periodes → ~20+ ms rondgang; met
  `IAudioClient3` (low-latency shared) vaak 2.67 ms periodes mogelijk
- WASAPI Exclusive: buffers tot ~3 ms per kant haalbaar indien de drivers meewerken

Realistische eerste doelen: **< 20 ms totaal** moet makkelijk kunnen, **~10 ms** is het
streefdoel. Ter referentie: 10 ms ≈ 3,4 m afstand tot een gitaarversterker.

## 7. Risico's / aandachtspunten

1. **Rocksmith-kabel als bottleneck**: mono 16-bit/48 kHz is prima voor gitaar (dynamiek
   van een gitaarsignaal past ruim in 16 bit mits input gain goed staat). Het echte risico
   is de onbekende interne buffering van de kabel + generieke usbaudio-driver. Meten in
   Fase 2. De kabel heeft bewezen realtime-geschiedenis (Rocksmith 2014 zelf).
2. **Realtek USB-codec**: USB-audio aan de outputkant voegt eigen buffering toe;
   USB-in → USB-uit betekent twee onafhankelijke USB-klokken → clock drift, vereist
   een drift-strategie in de passthrough (kleine ringbuffer of resampler).
3. **S/PDIF-volume**: digitale uitgang heeft meestal geen volumeregeling; mastervolume
   moet in software (of op de geluidsinstallatie).

## 8. Volgende stap (Fase 2)

Minimale passthrough Rocksmith → S/PDIF met meting van buffer sizes, callback-timing,
underruns en round-trip-latency. Technologiekeuze:

- **Eerste experiment: Python + `sounddevice`** (PortAudio). Met blocksize 128–256 @
  48 kHz en een simpele copy-callback is Python haalbaar voor een *test* (callback doet
  vrijwel niets). GC-pauzes en de GIL maken Python ongeschikt voor het eindproduct,
  maar voor meten van de pipeline is het de snelste weg.
- **Zodra we DSP gaan doen (Fase 4): C++ (JUCE of rauwe PortAudio/RtAudio) of Rust
  (cpal)** — NAM-core is C++, dus JUCE/C++ ligt het meest voor de hand.

Meetplan Fase 2:
1. Passthrough WASAPI Shared @ 48 kHz, blocksize 256 → stabiliteit + underrun-telling
2. Zelfde met WASAPI Exclusive, blocksize omlaag tot het kraakt
3. Round-trip-latency fysiek meten: loopback-klik (output → mic/kabel) of PortAudio's
   gerapporteerde input/output-latency loggen + gehoormatige check
