# Fase 3b — Native low-latency route (2026-08-15)

Doel: van ~17,5 ms (Python/PortAudio) naar ~10 ms. Uitkomst: **~4,8 ms software-
latency**, ruim voorbij het doel, via een onverwachte route. Alle code in `native/`,
gebouwd met MSVC (VS2022 Community, al aanwezig): `native\build.ps1`.

## Stap 1: native WASAPI Exclusive, event-driven (passthrough.cpp)

Rechttoe-rechtaan port van de Python-architectuur: capture- en render-thread,
beide exclusive event-driven op de minimale periode (3 ms / 144 frames, 16-bit),
SPSC-ringbuffer, MMCSS "Pro Audio".

Meting (met voldoende ring-headroom, `--target 4 --high 6`):

- capture-wakeups: **perfect regelmatig** (max 3,28 ms, 0 te laat)
- render-wakeups: **bursty** — max 10,3 ms, 28% te laat; de Realtek-driver
  signaleert events in ~10 ms-batches
- gevolg: ring moet ~4 buffers bufferen → totaal ~22,5 ms (0 underruns, maar
  te traag). Met krappe ring: oscillatie van underruns + drops.

De bottleneck is dus de **Realtek USB-outputdriver**, niet de kabel, niet de taal.

## Stap 2: alternatieve routes onderzocht

| Route | Resultaat |
|---|---|
| `IAudioClient3` shared low-latency (probe_iac3.cpp) | Dood spoor: beide drivers ondersteunen alléén 480 frames (10 ms) engine-periode |
| Realtek ASIO (probe_asio.cpp, mini-IASIO-host zonder SDK) | Dood spoor: vaste buffer 1024 frames (21,3 ms), alleen analoge "HD Audio"-kanalen, `canSampleRate(48k)` zegt zelfs nee |
| DMA-positie-granulariteit (probe_padding.cpp) | **Doorbraak**: `GetCurrentPadding` loopt per frame door (updates elke ~0,02 ms). De 10 ms-batching zit alleen in de event-signalering, niet in de hardware-positie |

## Stap 3: polling-architectuur (passthrough_ll.cpp)

Kern: de render-client in exclusive **polling** mode (geen events), en het strak
regelmatige capture-event (3 ms) als klok gebruiken. Eén realtime-thread, geen
ringbuffer:

1. wacht op capture-event (elke 3 ms, jitter < 0,3 ms);
2. lees 144 mono-frames, converteer, gain;
3. `GetCurrentPadding` → schrijf het blok direct in de render-buffer;
4. driftcorrectie op de padding-trend: te vol → oudste samples droppen;
   te leeg → opvullen met herhaald laatste sample.

Resultaten (60 s, padding-doel 288 frames):

```
latency  input=3.00 ms  output-padding avg=1.75 ms (min=41 max=92 frames)
geschat totaal software: 4.75 ms
wakeups=19997  underruns=0  driftdrops=0  opvulling=108 frames (drift ~86 ppm)
timeouts=0  wakeup max=3.27 ms  laat(>1.5x)=0
```

**~4,8 ms software** + ADC/DAC/USB-framing (~2-3 ms) → geschat **~7-8 ms**
mouth-to-ear. Ter vergelijking: 3 m van een gitaarversterker staan ≈ 9 ms.

## Kanttekeningen

- Padding-minimum is ~41 frames (0,85 ms) — krap. Onder zware systeemload kan
  een te laat capture-event een zelfherstellende glitch geven. Zo nodig
  `--target` verhogen (bv. 384 = +2 ms veiligheidsmarge).
- Exclusive mode kaapt beide devices: geen Windows-geluid tijdens een sessie,
  en apps die de (default-)microfoon vasthouden blokkeren de start
  (AUDCLNT_E_DEVICE_IN_USE, hr=0x8889000a) — zie hieronder.
- Rocksmith 2014 (Steam): geen driverconflict (kabel gebruikt generieke
  usbaudio), maar het spel claimt de kabel tijdens het draaien → niet
  gelijktijdig gebruiken.
- Aandachtspunt gevonden: een blijven hangen testproces hield de kabel bezet.
  Diagnose: register `CapabilityAccessManager\ConsentStore\microphone`
  (LastUsedTimeStop=0 = in gebruik).

## Consequenties voor de uiteindelijke app

- Taal/stack: **C++ + rauwe WASAPI** (geen PortAudio: die kan deze
  polling-architectuur niet uitdrukken; geen ASIO nodig).
- De passthrough_ll-lus is het skelet; DSP (NAM, IR, gate, EQ) komt straks in
  stap 2 van de lus, vóór het wegschrijven. Bij 3 ms blokken is het
  DSP-budget per blok ~2 ms — ruim voldoende voor NAM op een moderne CPU.
- Drift-correctie is nu sample-repeat/drop; voor de app upgraden naar een
  micro-resampler (fractionele delay) — onhoorbaar i.p.v. bijna-onhoorbaar.

## Reproduceren

```
native\build.ps1
native\passthrough_ll.exe --seconds 300 [--gain 2] [--target 288]
```
