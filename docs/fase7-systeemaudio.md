# Fase 7 — Systeemaudio meespelen (2026-08-16)

Implementatie van de in fase 6 gekozen architectuur (optie 3, gescheiden
endpoints): Spotify/YouTube/alles wat Windows afspeelt wordt als backing audio
in de engine gemixt, zonder het gitaarpad te raken.

## Architectuur

```
Spotify/YouTube → Windows-default = muziek-endpoint (VG248 HDMI, vol 100% + mute)
                        │ WASAPI-loopback + stille shared renderer
                        ▼
              capture-thread → lock-vrije ring (g.sring, 64k frames)
                        ▼  prefill ~50 ms, driftcorrectie ±1 frame/blok
gitaar 3 ms → NAM/IR/EQ ──mix (naast PlayerTrack)──→ S/PDIF exclusive (ongewijzigd)
```

## Onderdelen (alles in guitar_amp_gui.cpp)

- **EngineShared**: `sring` (65536 stereo-frames), `sringW` (vrijlopende
  frame-teller), `sysActive/sysMuted/sysVol/sysFillMs/sysStatus`.
- **sysCaptureMain** (eigen thread): loopback-client in mix-format van het
  muziek-endpoint + stille shared renderer (zonder renderer levert loopback
  0 frames — fase 6-meting). Bronrate ≠ 48 kHz → lineaire resampling
  (op dit systeem 48 kHz, dus passthrough). `AUDCLNT_BUFFERFLAGS_SILENT`
  wordt als stilte geschreven. Devicefout → status + nette zelfstop.
- **startSysAudio/stopSysAudio** (UI-thread): bewaart en herstelt
  default-device (IPolicyConfig::SetDefaultEndpoint, eConsole+eMultimedia,
  getest via `probe_setdefault.exe`: omzetten én herstel OK op deze build) en
  endpoint-volume/mute. Truc uit fase 6: **volume 100% + mute** = fysiek stil
  endpoint met vol loopbacksignaal.
- **Engine-mix**: in de renderlus naast de PlayerTrack-mix; eigen leesteller
  `sysR`, prefill 2400 frames (~50 ms), driftcorrectie max ±1 frame per blok
  (capaciteit ~333 frames/s, nodig ~5 bij 100 ppm). Consumptie = frames die
  naar het device gaan → muziek volgt automatisch de renderklok, ook door de
  bestaande engine-driftcorrectie heen. Mute consumeert door ("muziek loopt
  door"). Bufferstand naar UI (`sysFillMs`).
- **UI (Meespelen-tab)**: sectie SYSTEEMAUDIO met aan/uit, muziek-endpoint-
  dropdown (engine-output uitgesloten; leeg = automatisch eerste andere
  endpoint), Muziekvolume, Mute muziek, statusregel (buffer-ms / fouten).
  De bestandsspeler staat eronder als "BESTAND AFSPELEN".
- **Vangnetten**: engine-output wisselen naar het muziek-endpoint → sys-audio
  gaat automatisch uit; capture-thread sterft (endpoint weg) → UI herstelt
  default+mute alsnog; app-afsluiten → `stopSysAudio()` in het exit-pad.
- **settings.json**: `sys_device_id`, `sys_volume` (aan/uit is bewust niet
  persistent — default-device omzetten hoort een bewuste actie te zijn).

## Verificatie (klik-automatisering + screenshots + probes)

1. Toggle aan → default werd VG248, volume 100% + mute 1 (probe_getdefault).
2. 440 Hz-toon via Windows-default afgespeeld + engine gestart:
   - status "actief - muziekbuffer 48 ms" (doel ~50);
   - **out-meter −17 dB = toon hoorbaar in de mix**;
   - **engine onveranderd: exclusive, 10,2 ms software, 0 underruns, 0 drift.**
3. Stop + toggle uit → default terug op Realtek Digital, VG248 terug op
   100%/mute 0 (geverifieerd).
4. App-afsluiten netjes; settings bevatten sys-velden.

Klik-automatisering: PostMessage-kliks bleken onbetrouwbaar (de ImGui-Win32-
backend overschrijft de muispositie met GetCursorPos zodra het venster focus
heeft); echte input via SetCursorPos+mouse_event op GetWindowRect-coördinaten
werkt wel betrouwbaar.

## v1.1 — foolproof: sessie-gebonden gedrag (zelfde dag)

Eerste praktijktest van Neil legde een echte bug bloot: bij het wisselen van
muziek-endpoint (VG248 → G430) terwijl de engine draaide, ging het default-
device even via de oorspronkelijke default (= de exclusief bezette
engine-output). De YouTube-stream probeerde daarheen te verhuizen, stierf,
en bleef stil — "ik hoor nergens meer YouTube". Daarnaast bleek de volgorde
van handelingen (muziek aan vóór/na engine-start) uit te maken, en was niet
duidelijk waarom de eigen output niet in de endpoint-lijst staat.

Herontwerp:

- **Sessie-gebonden**: het vinkje is nu een intentie (persistent,
  `sys_enabled`). De omschakeling gebeurt pas bij engine-**Start** — éérst
  default omzetten (spelende streams verhuizen automatisch wég van de
  engine-output), dán pas exclusive claimen. Bij **Stop** (of engine-dood,
  met 4 s grace voor herstarts) gaat alles automatisch terug: volgorde kan
  niet meer fout.
- **retargetSysAudio**: endpoint wisselen tijdens een sessie zet de default
  rechtstreeks oud → nieuw, zonder tussenstap via de engine-output.
- **Default-bewaker**: elke ~150 frames wordt gecontroleerd of het default
  nog op het muziek-endpoint staat; zo niet → teruggezet (met statusmelding).
- **Signaaldetectie**: capture-thread publiceert een peak (`sysCapPeak`);
  is die > 2,5 s ~0 terwijl de sessie actief is, dan toont de UI "geen
  Windows-audio gedetecteerd — pauzeer en hervat je speler" (het recept
  voor een gestorven mediastream; die kunnen wij niet zelf herstarten).
- **Endpoint-botsing**: wordt het muziek-endpoint de engine-output (ook via
  presets), dan kiest de app automatisch een ander endpoint.
- **Auto-pick** verkiest NVIDIA/monitor-endpoints (daar is de mute-truc
  gemeten); UI-tekst legt uit dat de eigen output bewust ontbreekt en dat
  de muziek via de app te horen is.

## v1.2 — migratie-wachttijd (zelfde dag)

Tweede praktijktest Neil: "YouTube draaide → Start → klapt eruit; eerst Start
en dan YouTube verversen werkt wel". Oorzaak: Chrome verhuist een spelende
stream pas na een **~2 s debounce** naar het nieuwe default-device; de engine
claimde de output al na ~1 s exclusive — precies middenin de migratie → de
stream sterft (en Chrome herstelt dat niet zelf; alleen pauze/hervat of
refresh helpt).

- **Migratie-wachttijd**: bij een vers gestarte sessie wacht de engine-thread
  3 s (interruptbaar) tussen capture-init en de exclusive render-claim.
  Geverifieerd: toon die al op de speakers speelde migreert nu vanzelf mee en
  klinkt na de wachttijd in de mix (out −21,6 dB, 0 underruns).
- Render-DEVICE_IN_USE-retries 5 → 15 (3 s) voor traag loslatende streams;
  sessie-grace 4 → 10 s (engine-start duurt nu tot ~6 s met wachttijd).
- `setDefaultEndpoint` zet nu ook eCommunications.
- Stilte-hint: drempel 0,003 → 0,01, tekst noemt ook "ververs de pagina".
- **Diagnostiek**: onderste statusregel toont een levende `wakeups`-teller.
  Neil zag één keer een toestand waarin ook de gitaar stil was terwijl de
  stats normaal leken; onopgehelderd (kandidaten: capture-stall vs
  S/PDIF-receiver-lock). Bij herhaling: kijk of de teller doorloopt
  (loopt door = capture leeft → dan zit het na de engine, bv. S/PDIF-lock).

## v1.3 — warm-hold tegen de "stille exclusive" (zelfde dag)

Neil's oor-test van v1.2 loste het mysterie op: de migratie werkte (YouTube
kwam bij Stop vanzelf terug, zonder refresh), maar de exclusive S/PDIF-stream
zelf was **onhoorbaar** — terwijl padding/meters/stats normaal liepen (out
−21,6 dB op de meter, niets uit de speakers; ook de gitaar stil).

Patroon over alle sessies heen:
- exclusive claimen terwijl de Realtek nog default/actief is → geluid OK,
  en blijft OK als het default daarna omklapt (Neil's werkende volgorde);
- default éérst wegklappen, Realtek een paar seconden idle laten worden, en
  dán claimen → stille stream tot een stop/start.

Duiding: de Realtek-USB-codec parkeert/suspendt zodra default + streams weg
zijn; een exclusive claim die in dat gat valt treft een dode transmitter.
De 3 s-wachttijd van v1.2 mikte er precies in.

Fix: tijdens de migratie-wachttijd houdt de engine het outputdevice zelf
warm met een tijdelijke **stille shared stream** (AUTOCONVERTPCM, halfvol
gehouden), die pas milliseconden vóór de exclusive claim wordt gesloten;
een eventuele botsing met de eigen net-gesloten stream vangen de
DEVICE_IN_USE-retries op.

Fallback als dit nog niet afdoende is: USB-power-management van de codec
uitzetten (EnhancedPowerManagementEnabled=0 in Device Parameters van
`USB\VID_26CE&PID_0A08`) — vereist admin; nog niet gedaan.

## v1.4 — de echte oorzaak: hoorbaarheid hangt aan default-bij-claim (zelfde dag)

Doorbraak via twee metingen:

1. **Mix-diagnose** (out-meter meet nu de eindmix; statusregel toont
   "capture X dB | mix Y dB"): tijdens een stille sessie bleek capture −9 dB
   én mix −22 dB én out-meter −21 dB — de software werkte dus van begin tot
   eind correct; de onhoorbaarheid zit ná de software. (Belangrijke les: de
   oude out-meter mat alleen het gitaarpad, vóór de mixplek — eerdere
   "meter-successen" van de muziekmix waren zelfbedrog.)
2. **Piep-A/B-test met oren van Neil** (probe_beep met stilte-aanloop tegen
   receiver-lock): exclusive claim terwijl Realtek default is → hoorbaar;
   identieke claim terwijl default op VG248 staat → onhoorbaar, terwijl het
   device de data gewoon consumeert (padding draint normaal).

Conclusie: **de Realtek-USB-codec speelt een exclusive stream alleen hoorbaar
af als het endpoint op het claim-moment het Windows-default is.** Eenmaal
hoorbaar geclaimd mag het default daarna weg (Neil's werkende "dans" bewees
dat de hoorbaarheid dan blijft). Warm-hold (v1.3) hielp hier dus niets aan
en alle eerdere "wedge"-theorieën herleiden tot dit ene drivergedrag.

Fix (= Neil's dans geautomatiseerd):

- **Start**: eerst `startEngine` (claim terwijl Realtek nog default is; een
  spelende mediastream wordt door Windows' exclusive-prioriteit gekickt),
  daarna zet `sysPendingBegin` de sessie pas op zodra `running` — default →
  VG248, mute, capture. Chrome maakt zijn gekickte stream opnieuw aan op het
  nieuwe default; zo niet, dan wijst de gele hint op pauze/hervat/ververs.
- **Outputwissel/preset tijdens sessie**: `restartEngineForOutput` sluit de
  sessie (default terug), claimt, en hervat de sessie via hetzelfde
  pending-mechanisme.
- renderDelay/warm-hold uit het Start-pad (dode code, delay 0).

## v1.5 — DE echte oorzaak: USB-suspend maakt de eerste exclusive stream stil

De v1.4-theorie (default-bij-claim) sneuvelde ook: met de **Blue Snowball als
geautomatiseerde kamer-microfoon** (probe_mic.cpp; kalibratie: stilte
RMS 0,00000 vs hoorbare toon 0,0026-0,0029) bleek elke claim-conditie soms
hoorbaar en soms stil. Deterministische reproductie vond de echte dader:

- **Na ~minuten volledige audio-stilte suspendt de Realtek-USB-codec**
  (register: `EnhancedPowerManagementEnabled=1`, `DeviceSelectiveSuspended=1`
  op VID_26CE&PID_0A08).
- **De eerste exclusive stream op een gesuspende codec speelt ONHOORBAAR**:
  init OK, padding draint, stats normaal — maar hij wekt alleen de hardware.
  Meting: na 210 s stilte → claim+toon = RMS 0,00000; direct een tweede
  claim = RMS 0,00289 (hoorbaar). Shared afspelen wekt hem ook (daarom
  "loste" een YouTube-refresh alles op).
- Alle eerdere theorieën (default-status, kick, flip-timing, warm-hold)
  waren toevallige correlaties met stiltetijd. Sessies faalden juist vaak
  omdat ze na minuten stil overleggen/testen begonnen. Een stille shared
  stream (v1.3-warm-hold) wekt hem NIET — Windows' idle-detectie suspendt
  door bij digitale stilte; het weksignaal moet niet-stil zijn.

Fix in `engineMain`: **wake-claim** — vóór de echte render-claim altijd een
korte wegwerp-exclusive-claim die 400 ms een −60 dB-toon speelt (digitaal
niet-stil, akoestisch onhoorbaar), dan sluiten, 100 ms bezinken, echte claim
(DEVICE_IN_USE-retries vangen de overgang). Kost ~0,5 s per engine-start.
Tijdens een sessie blijft de codec wakker door de NAM-ruisvloer (niet-stil).

Optionele wortel-fix (niet uitgevoerd, vergt admin): USB-power-management
van de codec uit via `EnhancedPowerManagementEnabled=0` in Device Parameters
van `USB\VID_26CE&PID_0A08&MI_00`/`MI_06`.

Diagnosetools die dit vonden: probe_mic.cpp (kamer-RMS via Snowball),
probe_beep.cpp (exclusive claim + tonen met instelbaar ritme/aanloop),
probe_setdef/probe_vol/probe_rms.

## v1.6 — eindstand: naadloze volgorde + wake-claim

Eindtest Neil met v1.5: **muziek + gitaar samen hoorbaar** (flow: Start →
YouTube gekickt/gepauzeerd → play indrukken → alles speelt). v1.6 haalt die
laatste play-stap weg door de v1.4-volgorde terug te draaien (die was
gebaseerd op de weerlegde default-theorie): **sessie eerst** (default →
muziek-endpoint; spelende media migreren dan naadloos mee, gemeten met echte
YouTube), engine start met 2,5 s migratie-wachttijd, en de **wake-claim**
dekt de USB-suspend af. Engine-herstarts tijdens een sessie laten de sessie
gewoon doorlopen (pending-mechanisme en sessie-herstart bij outputwissel
weer verwijderd — de wake-claim maakt elke claim hoorbaar).

Gebruiksrecept (geen geheime volgorde meer):
1. muziek aan (YouTube/Spotify), vinkje "Windows-audio meespelen" aan;
2. Start → ± 4 s later speelt alles door de speakers, gitaar op ~10 ms;
3. hapert de muziek toch (gele hint verschijnt): één keer pauze/play of
   pagina verversen;
4. Stop → Windows-audio meteen weer normaal.

## Beperkingen / aandachtspunten

- De mute-truc is gemeten op NVIDIA-HDMI. Bij een ander soort muziek-endpoint
  (bv. USB-headset met hardwarevolume) kan mute wél het loopbacksignaal
  dempen; dan een endpoint zonder speakers kiezen of het volume laag zetten.
- Muziekpad loopt ~50 ms achter "live" — irrelevant voor muziek; bij
  YouTube-video theoretisch minimale lipsync-offset (onmerkbaar).
- Windows verhuist spelende streams automatisch mee bij de default-wissel
  (stream routing), dus Spotify hoeft niet herstart te worden.
- Nieuwe probes: `probe_setdefault.cpp`, `probe_getdefault.cpp` (naast de
  fase 6-probes); los gebouwd met cl, staan niet in build.ps1.
