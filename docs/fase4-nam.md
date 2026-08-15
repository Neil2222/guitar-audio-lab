# Fase 4 — NAM-integratie (2026-08-15)

Doel: Neural Amp Modeler in de bewezen 4,8 ms-pipeline. Resultaat: **werkt,
zonder meetbare latencytoename** — totaal blijft ~4,8 ms, NAM kost 0,13 ms/blok.

## NeuralAmpModelerCore

- Repo: https://github.com/sdatkinson/NeuralAmpModelerCore (MIT), gekloond in
  `third_party/NeuralAmpModelerCore` (submodules: Eigen, nlohmann/json,
  AudioDSPTools).
- API: `nam::get_dsp(pad)` → `unique_ptr<nam::DSP>`;
  `Reset(sampleRate, maxBufferSize)` (doet ook prewarm);
  `process(double** in, double** out, n)`; mono in/uit.
- Gebouwd met MSVC, geen CMake nodig: alle `NAM/*.cpp` + `NAM/wavenet/*.cpp`
  → `build_msvc/nam.lib`. Vereist `/std:c++20`, includes voor repo-root,
  `Dependencies/eigen` en `Dependencies/nlohmann`; `/arch:AVX2` voor Eigen.
- **Let op bij linken tegen nam.lib: `/WHOLEARCHIVE:` is verplicht** — de
  netwerkarchitecturen registreren zich via static initializers die de linker
  anders wegoptimaliseert ("No config parser registered for architecture").

## Benchmark (benchmodel, 2 s audio, buffers van 64)

| model | tijd | % realtime | per 3 ms-blok |
|---|---|---|---|
| wavenet_a1_standard (standaardarchitectuur) | 66 ms | 3,3% | 0,10 ms |
| wavenet_a2_max (zwaarste) | 87 ms | 4,4% | 0,13 ms |
| lstm | 6 ms | 0,3% | 0,01 ms |

DSP-budget per 3 ms-blok is ~2 ms → zelfs het zwaarste model gebruikt < 7%.
Ruimte zat voor cabinet-IR, gate en EQ erbij.

## guitar_amp.exe (native/guitar_amp.cpp)

Passthrough_ll-architectuur + NAM ertussen, plus veiligheden:

- **DC-blocker** (one-pole highpass ~20 Hz) na het model;
- **fade-in/fade-out** (~100/150 ms) bij start en einde van de sessie;
- **soft clip** boven 0,8 i.p.v. harde begrenzing;
- per-blok DSP-tijdmeting met budgetbewaking.

Meting met echt model (Bug333 Clean Cab):

```
geschat totaal software: 4.77 ms
NAM per blok: avg=0.131 ms  max=0.454 ms  budget=2.01 ms  overschrijdingen=0
```

## Les: testmodellen niet hoorbaar draaien

De `example_models/*.nam` in de NAM-repo zijn **testfixtures met willekeurige
gewichten**. Zo'n model werkt als extreme versterker van de ruisvloer:
resultaat was een harde statische brom + een luide kraak (DC-offset-stap) bij
Neil. De DC-blocker/fades vangen de kraak nu af, maar: alleen echte captures
gebruiken voor hoorbare tests.

## Echte modellen

Gedownload uit https://github.com/pelennor2170/NAM_models (communitycollectie)
naar `models/`:

| bestand | karakter |
|---|---|
| Phillipe P Bug333-Clean-Cab-ESR0,007.nam | clean, cabinet zit in de capture |
| Phillipe P VOXAC15-TopBoost.nam | VOX-chime/edge |
| Tim R JCM2000 Crunch.nam | Marshall-crunch |
| Helga B 5150 BlockLetter - Boosted.nam | high-gain metal |

Kanttekening: captures zonder cabinet in de keten klinken schel/fizzy op een
full-range stereo; dan is een **cabinet-IR** nodig (volgende stap; kandidaat:
`ImpulseResponse` uit AudioDSPTools, zit al in de submodules).

## Volgende experimenten

1. Speeltest van de vier modellen door Neil (`--model`, `--gain`, `--master`).
2. Cabinet-IR-convolutie toevoegen (AudioDSPTools) voor amp-only captures.
3. Noise gate vóór het model (Rocksmith-kabel heeft hoorbare ruisvloer).
4. Daarna richting echte app: device-/modelkeuze, presets, meters, ASRC.
