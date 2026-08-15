# Fase 2/3 — Passthrough en latency-metingen (2026-08-15)

Doel: Rocksmith-input → software → S/PDIF-output, kaal (geen processing), met meting
van latency, stabiliteit en drift. Alle metingen op 48 kHz, float32, gitaar aangesloten.

## Aanpak en gevonden problemen

### 1. Full-duplex PortAudio-stream (WASAPI): werkt, maar hoge vaste latency

`passthrough.py` opent één `sd.Stream` met verschillende input-/outputdevices.
Resultaten (8 s per configuratie, geen enkele underrun/overflow in geen enkele config):

| mode | blocksize | blok ms | gerapporteerde totale latency |
|---|---|---|---|
| shared | 512 | 10.67 | 54.7 ms |
| shared | 256 | 5.33 | 49.3 ms |
| shared | 128 | 2.67 | 46.7 ms |
| exclusive | 256 | 5.33 | 53.3 ms |
| exclusive | 128 | 2.67 | 50.7 ms |
| exclusive | 32 | 0.67 | 48.7 ms |

**Conclusie:** er zit een vaste vloer van ~47–55 ms in, onafhankelijk van blocksize en
mode; `latency='low'` veranderde niets; callbacks komen in bursts met 10 ms-periode.
PortAudio's WASAPI-backend bouwt bij duplex over twee devices een grote interne
adaptatie-ringbuffer. Deze route is een doodlopende straat voor low latency.

### 2. WDM-KS: laag maar onbetrouwbaar

- Losse streams openen soms prima (10 ms latency per kant), maar falen intermitterend
  met `Invalid device [-9996]`.
- Oorzaak: de S/PDIF-output is het Windows-default-device; de audio-engine (audiodg)
  claimt het KS-pin wisselend. KS vereist exclusieve pin-toegang.
- Duplex over twee WDM-KS-devices in één stream kan sowieso niet in PortAudio.

**Conclusie:** WDM-KS alleen bruikbaar als het endpoint géén Windows-default is.
Niet verder vervolgd; WASAPI Exclusive bleek beter én betrouwbaar.

### 3. Gesplitste WASAPI Exclusive-streams + ringbuffer: de winnaar

`split_passthrough.py`: aparte `InputStream` + `OutputStream` (beide WASAPI Exclusive)
gekoppeld met een blokken-deque. Prefill 2 blokken; cap (drop) bij overloop — nodig
omdat input (Rocksmith) en output (Realtek USB) op verschillende USB-klokken lopen.
Met een **expliciete numerieke latency-hint** (i.p.v. `'low'`) stort de vaste vloer in:

| blocksize | hint | stream in+out | ring (gem.) | totaal | underruns | drops | opmerking |
|---|---|---|---|---|---|---|---|
| 128 | 5 ms | 14.0 ms | 12.1 ms | 26.1 ms | 48 | 22 | ring te ruim (maxq 8, geen prefill-beleid) |
| **128** | **3 ms** | **10.7 ms** | **6.8 ms** | **17.5 ms** | **0** | 4 per 30 s | **stabiel; beste config** |
| 96 | 3 ms | 8.7 ms | 4.3 ms | 12.9 ms | 785 | 1098 | onbruikbaar |
| 64 | 2 ms | 7.3 ms | 2.7 ms | 10.0 ms | 1099 | 3746 | onbruikbaar |

30-secondenrun met beste config: **0 underruns, 4 drops** (≈ 1 per 7,5 s), CPU ~1,5%.

## Duiding

- **Beste stabiele meting: ~17,5 ms** software-pijplijn (input 5,3 + ring 6,8 + output
  5,3 ms), plus ADC in de kabel en DAC in de installatie (~1–2 ms) → **~19 ms totaal**.
  Onder het eerste doel (< 20 ms), nog niet op het streefdoel (~10 ms).
- **De drops zijn klokdrift**: input-USB-klok loopt iets sneller dan output-USB-klok;
  ± elke 7,5 s wordt één blok van 2,67 ms weggegooid (hoorbaar als hooguit een minuscuul
  tikje). Structurele oplossing voor de echte app: adaptieve micro-resampling (ASRC).
- **Python is de beperkende factor onder blocksize 128**: bij 96/64 samples exploderen
  underruns terwijl CPU-load laag blijft — dat is callback-jitter (GIL/GC/scheduling),
  geen rekenkracht. Voor het meetdoel van Fase 2 was Python de juiste keuze; voor
  kleinere buffers en de uiteindelijke app is een native implementatie nodig
  (C++/JUCE ligt voor de hand omdat NAM-core C++ is; direct WASAPI/IAudioClient3 kan de
  ring ook strakker maken dan PortAudio toelaat).

## Openstaande punten

1. **Fysieke round-trip-meting** ontbreekt: gerapporteerde latency is wat PortAudio
   claimt. Een echte meting vereist een loopback (S/PDIF-out → analoog → terug de
   Rocksmith-kabel in met een jack-adapter) of een gehoormatige beoordeling.
2. Subjectieve speeltest door Neil (voelt 17,5 ms direct genoeg?).
3. Exclusive mode kaapt de default output: Windows-geluid is stil tijdens een sessie.
   Voor de echte app is dat prima (of zelfs gewenst); anders default output verleggen.

## Reproduceren

```
uv run scripts/split_passthrough.py                 # beste config, 60 s
uv run scripts/split_passthrough.py --seconds 300   # speelsessie van 5 min
uv run scripts/passthrough.py --bench               # oude duplex-benchmark
```
