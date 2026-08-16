# Fase 6 — Onderzoek: systeemaudio (Spotify/YouTube) meespelen (2026-08-16)

Vraag: kan normale Windows-audio (Spotify/YouTube/VLC) als backing audio door
de app gemixt worden, zonder de ~10 ms gitaarlatency op te geven? Alleen
onderzoek + geïsoleerde probes; engine ongewijzigd. Nieuwe probes:
`native\probe_shared_render.cpp` en `native\probe_loopback.cpp` (los gebouwd
met cl, buiten build.ps1 om).

## Metingen op dit systeem

### 1. IAudioClient3 / shared engine-periode (herbevestigd)

Beide devices (Rocksmith-capture én Realtek S/PDIF): engine-periode
default=fundamental=min=max = **480 frames = 10 ms**. Low-latency shared
(2,67 ms) bestaat op deze drivers niet.

### 2. WASAPI Shared render op de S/PDIF (probe_shared_render)

- Mix-format 48 kHz / 2ch / 32-bit float → zelfde rate als de engine, dus
  geen SRC nodig (alleen 16-bit→float-conversie door AUTOCONVERTPCM).
- **Padding-granulariteit: de shared engine consumeert in porties van exact
  480 frames elke 10,00 ms** (gemeten: 5 overgangen in 300 ms, allemaal 480).
- Minimaal stabiel padding-doel met de 3 ms-schrijfcadans van de engine
  (10 s per doel):

| target | resultaat |
|---|---|
| 288 (6 ms) | pad==0 elke cyclus → continu underrun |
| 480 (10 ms) | idem, onbruikbaar |
| **624 (13 ms)** | **stabiel**: min pad 144, 0× pad==0 |
| 768 (16 ms) | stabiel, min pad 288 |

- Gitaarlatency in shared ≈ 3 ms input + ~10 ms gemiddelde wachtrij
  (target 624) + ~10 ms engine-mix/device-pad ≈ **~20–23 ms totaal**.
  Ruim 2× de huidige ~10 ms → voelbaar slechter. GetStreamLatency
  rapporteert 0 (nutteloos op deze driver).

### 3. Loopback-gedrag terwijl S/PDIF exclusive bezet is (probe_loopback)

Actieve render-endpoints tijdens de meting: VG248 (NVIDIA HDMI),
LG ULTRAGEAR (NVIDIA HDMI), Realtek Digital (S/PDIF), Logitech G430.

| test | resultaat |
|---|---|
| A1: loopback op het exclusief bezette S/PDIF-endpoint | **faalt: 0x8889000a DEVICE_IN_USE** |
| A2: shared render (= Spotify) op het bezette S/PDIF-endpoint | faalt: DEVICE_IN_USE (Spotify kán daar dus sowieso niet spelen) |
| B: loopback op ander endpoint (VG248 HDMI) terwijl S/PDIF exclusive draait | **werkt**: init OK, 48 kHz float 2ch |
| B1: loopback zonder actieve renderer op dat endpoint | **0 frames** — loopback pompt alleen als er gerenderd wordt; de app moet zelf stilte renderen (bekende WASAPI-eigenschap) |
| B2: met stille shared renderer erbij | 96480 frames in 2 s ≈ 48 kHz, continu |
| C: endpoint-volume vs loopback | volume beïnvloedt het loopback-signaal (10% → RMS ×~0,02), **mute NIET** (RMS identiek aan 100%) |

Consequentie van C: **endpoint-volume 100% + mute = fysiek stil endpoint met
vol loopback-signaal.** (Gedrag van de NVIDIA HDMI-driver; bij een ander
muziek-endpoint opnieuw verifiëren.)

## Beoordeelde architecturen

1. **Alles WASAPI Shared** — Spotify werkt vanzelf, Windows mixt. Maar
   gitaar ~20–23 ms (meting 2). Afgewezen: verliest het hoofddoel.
2. **Loopback op hetzelfde S/PDIF-endpoint** — kan niet (A1), en is ook
   conceptueel dood: apps kunnen er toch niet naartoe spelen (A2).
3. **Gescheiden endpoints, zonder extra software** — Windows-default naar een
   tweede al aanwezig endpoint (HDMI-monitor), app doet daar
   loopback-capture + stille renderer, mixt in de bestaande engine-lus,
   gitaarpad blijft exclusive S/PDIF met ~10 ms. **Aanbevolen.**
4. **Virtueel audio-device (VB-Cable e.d.)** — zelfde architectuur als 3 maar
   met driverinstallatie; alleen nodig als er geen bruikbaar tweede endpoint
   is. Reserve-optie.
5. **Proces-loopback** (`VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`, Win10 20H1+)
   — per-proces capture (include/exclude-boom). Lost het defaultdevice-
   probleem niet op (app moet weten welk proces) en de exclude-variant is
   alleen zinvol als wij zelf shared zouden renderen. Niet nodig.

## Aanbevolen architectuur (nog niet gebouwd)

```
Spotify/YouTube/VLC → Windows default = HDMI-endpoint (volume 100% + mute)
                                │ WASAPI loopback (48k float stereo,
                                │ 10 ms-packets) + stille renderer
                                ▼
                        ring ~30-50 ms + driftcorrectie (drop/insert
                        of micro-resample; NVIDIA-klok ≠ Realtek-klok)
                                ▼
gitaar: Rocksmith 3 ms → NAM/IR/EQ ──mix (bestaande PlayerTrack-plek)──→
                        S/PDIF WASAPI Exclusive (ongewijzigd, ~10 ms)
```

- Gitaarpad: **exact ongewijzigd** (~10 ms). Muziekpad: ~30–60 ms achter
  "live" — irrelevant voor Spotify; bij YouTube-video theoretisch lipsync-
  offset, onder ~60 ms onmerkbaar.
- Asymmetrische latency is hier gewoon correct: de muziek is een continue
  stroom zonder interactie-eis; niets hoeft gelijkgetrokken te worden.
- Twee onafhankelijke klokken (HDMI vs Realtek-USB) → drift in de orde van
  ~100 ppm; met 30–50 ms ring en de bestaande drop/insert-aanpak (of ASRC,
  al op de ideeënlijst) onhoorbaar voor muziek.
- Default-device omschakelen kan de app zelf (IPolicyConfig, ongedocumenteerd
  maar de-facto standaard; alternatief: gebruiker zet het één keer handmatig).
  Windows verhuist spelende streams automatisch mee (stream routing).
- Feedback-loop is uitgesloten: wij renderen niet naar het gecapturede
  endpoint (alleen stilte) en het gitaarpad zit op een ander device.

## Aandachtspunten voor de implementatie (fase 7)

1. Loopback-capture heeft een eigen thread nodig (10 ms-packets) + lock-vrije
   ring naar de engine-thread; mixen op de bestaande PlayerTrack-plek.
2. Stille renderer op het muziek-endpoint niet vergeten (anders 0 frames).
3. Endpoint-keuze: HDMI-monitor zonder speakers is ideaal; mute-truc als
   vangnet. UI: dropdown "muziekbron-endpoint" + status.
4. Monitor-slaapstand/resolutiewissel kan het HDMI-endpoint laten verdwijnen
   → device-invalidation afhandelen (zoals engine-herstartpad).
5. Bij STOP van de engine: default-device en mute-status netjes terugzetten.
