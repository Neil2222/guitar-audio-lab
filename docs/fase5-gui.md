# Fase 5 — GUI (2026-08-15)

Eerste grafische schil rond de NAM-engine: `native\guitar_amp_gui.exe`.

## Architectuurkeuze

Native GUI in dezelfde exe als de audio-engine, met **Dear ImGui** (MIT,
`third_party/imgui`, Win32 + DX11-backend). Overwogen alternatieven:

- lokale web-UI (mini-server-conventie): afgewezen — vergt IPC tussen
  C++-engine en server, en de engine moet toch native blijven;
- Win32-dialogen: afgewezen — meters/sliders zijn daar onhandig.

De engine (identiek aan `guitar_amp.cpp`: exclusive capture-event als klok,
polling render, DC-blocker, fades, soft clip) draait op een eigen
"Pro Audio"-thread. UI ↔ engine communiceert uitsluitend via `std::atomic`s
(gain, master, meters, stats) — geen locks op het audiopad (alleen een mutex
voor foutmeldingen, buiten de realtime-lus).

## Functies v1

- modelkeuze (dropdown, scant `models\*.nam`, ververs-knop; wisselen tijdens
  het spelen herstart de engine met het nieuwe model);
- START/STOP met fade-in/uit;
- input gain (0–4x) en master (0–1,5) live;
- peak-meters in/uit (dB, kleurzones, decay);
- statusregel: software-latency (3 ms + gemeten padding), NAM ms/blok,
  underruns, driftcorrectie, late wakeups;
- foutafhandeling: device bezet, model-laadfouten (in het rood in de UI).

## Verificatie

Gestart, via geautomatiseerde klik op START getest en met screenshots
gecontroleerd: engine meldt 4,8 ms software, NAM 0,12 ms/blok, 0 underruns;
meters bewegen live.

## Bouwen

`native\build.ps1` bouwt alles; de GUI-sectie compileert ImGui-bronnen mee en
linkt `nam.lib` met `/WHOLEARCHIVE` en `/SUBSYSTEM:WINDOWS`.

## v1.1 — naadloze modelwissel (zelfde dag)

Probleem (gemeld door Neil): modelwissel tijdens het spelen liet het geluid
wegvallen tot handmatige STOP/START. Oorzaak: de wissel herstartte de hele
engine, en de nieuwe exclusive-init botste op het nog-niet-vrijgegeven device
(AUDCLNT_E_DEVICE_IN_USE) waardoor de engine-thread stierf.

Oplossing:

- **hot-swap**: modelwissel herstart de engine niet meer. Een loader-thread
  laadt + prewarmt het nieuwe model en zet het in `pendingModel` (atomic);
  de engine pikt het op een blokgrens op, met korte fade-in en gereset
  DC-blocker. Het oude model gaat via `retiredModel` naar de UI-thread voor
  de delete (nooit geheugenbeheer op het audiopad). Audio loopt door;
  underruns blijven 0 (geverifieerd met klik-automatisering + screenshots,
  driftteller liep door = geen herstart).
- **robuustere start**: DEVICE_IN_USE bij Initialize wordt nu tot 5x opnieuw
  geprobeerd (met re-Activate, 200 ms interval) — vangt het geval
  "START direct na STOP".

## v1.2 — stemmer + meespeel-speler (zelfde dag)

Twee nieuwe secties in de GUI:

**Stemmer** — YIN-pitchdetectie (venster 2048, W=1024, tau 40–1024 ≈
47–1200 Hz, CMND-drempel 0,12, parabolische verfijning, RMS-gate −54 dB).
De engine schrijft rauwe pre-gain-inputsamples naar een lock-vrije ring
(16384 floats); de UI-thread rekent er ~10x/s op. Toont nootnaam + octaaf,
afwijking in cents (groen binnen ±5), en een balkmeter.

**Meespelen** — audiospeler gemixt in de engine-output:
- decodering via **Media Foundation** (mp3/wav/flac/m4a/wma), gevraagd om
  float/48k/stereo; levert de reader dat niet, dan eigen Hermite-resampling;
- track als `PlayerTrack` via hetzelfde pending/retired-patroon als modellen;
  positie overleeft engine-herstart (`resumePos`), track wordt bij STOP
  teruggegeven aan pending;
- mixing op de engine-thread: gitaar (mono→L/R) + track (stereo × volume),
  soft clip op de som; play/pauze/herhaal/volume/seek via atomics;
- bestandskeuze via IFileOpenDialog; een opstartargument met een audiopad
  laadt direct (handig voor tests en snelkoppelingen).

Geverifieerd met een gegenereerde test-WAV (auto-load, duurweergave 0:10
correct) en de bezet-flow (tweede instantie krijgt nette rode melding).

## v1.3 — modellenbrowser + EQ (zelfde dag)

Richting bijgesteld met Neil: geen Rocksmith nabouwen (speler blijft zoals hij
is; YouTube/Spotify-integratie bewust buiten scope), wel het model-beheer
verbeteren en een EQ toevoegen.

**Meer modellen (online)** — inklapbare sectie:
- haalt de bestandslijst van de NAM_models-communitycollectie op via de
  GitHub-API (WinHTTP + nlohmann/json, cache in `models\catalog.txt`);
- zoekveld + virtuele lijst (ImGuiListClipper, max 2000 zichtbaar);
- per model een "ophalen"-knop → download van raw.githubusercontent.com naar
  `models\` (percent-encoding voor spaties), al-aanwezige modellen gemarkeerd;
- dropdown ververst automatisch na een download (selectie blijft behouden);
- "Map openen"-knop + verwijzing naar tonehunt.org voor handmatige aanvulling.
- Geverifieerd met klik-automatisering: 259 modellen opgehaald, download
  van een capture (407 KB) werkt, markering en refresh kloppen.

**EQ** — 3-bands post-NAM: Bass (low shelf 100 Hz), Mid (peaking 550 Hz,
Q 0,7), Treble (high shelf 3 kHz), elk ±12 dB (RBJ-biquads, transposed DF2).
Engine herrekent coefficienten alleen bij wijziging (eqDirty-atomic) en
slaat de filters over als alles op 0 staat. Zit tussen DC-blocker en master.

## v1.4 — mockup-layout, categorieën, TONE3000, gate (zelfde dag)

Grote verbouwing richting Neil's mockup (ChatGPT-ontwerp):

- **Layout**: zijbalk (DEVICES / STATUS / LEVELS) + hoofdpaneel met
  modelkeuze (met vorige/volgende-pijltjes), START/STOP, signaalketen-regel
  (Input gain, Master, **Gate**, EQ) en vijf tabbladen: Mijn modellen,
  TONE3000, Meespelen, Stemmer, Community (GitHub). Venster 1000×820.
- **Noise gate** in de engine (pre-NAM): envelope-follower (release ~40 ms)
  met zachte gain-smoothing (attack ~2 ms), drempel −90…−40 dB, −90 = uit.
- **Categorieën**: lokale modellen automatisch ingedeeld op
  merk-trefwoorden (Fender, Marshall, Vox, Peavey/EVH, Mesa, High-gain,
  Boutique, Pedalen, Bas, Overig); categoriekolom met tellingen + lijst.
- **TONE3000-integratie** (officiële API, OAuth 2.0 PKCE):
  - eigen SHA-256 + base64url voor de code challenge; loopback-listener op
    `127.0.0.1:53682` (Winsock) vangt de redirect; browser via ShellExecute;
  - tokens in `t3k.json` (naast `models\`), automatische refresh;
  - zoeken op `/api/v1/tones/search` (format=nam, sortering: meest
    gedownload / beste match / nieuwste / trending) met **populariteit**
    (downloads + favorites) in de resultatentabel;
  - download: `/api/v1/models?tone_id=` → `model_url` (voorkeur "standard")
    met Bearer-token → `models\`.
  - Setup voor de gebruiker: gratis account op tone3000.com → Settings →
    API Keys → publishable key met redirect-URI
    `http://127.0.0.1:53682/callback` → key in de app plakken.
  - **Let op**: end-to-end nog niet getest (vereist accountkey); de
    JSON-parsing is defensief geschreven; veldnamen/loopback-acceptatie
    kunnen een iteratie vragen.
- Populariteit in de GitHub-lijst is niet mogelijk (geen statistieken per
  bestand in een repo) — dat lost het TONE3000-tabblad op.

## v1.5 — outputdevice-keuze (zelfde dag)

- Zijbalk heeft nu een **Output-dropdown**: alle actieve render-endpoints
  (Hands-Free-belprofielen worden weggefilterd), plus "Automatisch (S/PDIF)".
  `@`-knop herscant (bv. na het koppelen van Bluetooth-oordopjes). Keuze wordt
  bewaard in `settings.json`; wisselen tijdens het spelen herstart de engine
  op het nieuwe device.
- Engine: probeert eerst WASAPI **Exclusive** (met DEVICE_IN_USE-retry);
  lukt dat niet (bv. Bluetooth), dan **shared-fallback** met
  `AUTOCONVERTPCM|SRC_DEFAULT_QUALITY` en ruimer padding-doel (30 ms i.p.v.
  6 ms, want de shared engine consumeert in 10 ms-porties). De zijbalk toont
  de actieve modus; de latency-telling klopt automatisch mee.
- Getest: Logitech G430 (USB) draait exclusive met 10,4 ms totaal,
  0 underruns. Bluetooth (Bose) alleen te testen met gekoppelde oordopjes —
  verwachting: shared mode, plus 100–300 ms codec-latency van BT zelf
  (geen software-oplossing voor; prima voor oefenen, niet voor strak spelen).

## v1.6 — TONE3000-downloadregister (zelfde dag)

Gemeld door Neil: de TONE3000-tab wist niet welke tones al gedownload waren
(de GitHub-tab checkt op bestandsnaam, maar bij TONE3000 is de bestandsnaam
pas bekend na de download: titel + modelnaam).

- `t3k.json` bevat nu een `downloads`-register (tone-ID → bestandsnaam),
  bijgewerkt na elke download.
- De resultatentabel toont "al binnen" wanneer het ID in het register staat
  én het bestand nog bestaat (verwijderd bestand ⇒ knop komt terug).
- Terugval voor oudere downloads (van vóór dit register): titel-prefix-match
  tegen de lokale modellenlijst.

**Naschrift (debug):** de eerste v1.6-build bleek *stale* — een
tooling-beveiliging blokkeerde stilletjes het gecombineerde
build-commando (`Remove-Item` + `cmd /c` in één aanroep), waardoor een
oudere exe met een verse timestamp overbleef en de fix "niet leek te
werken". Gediagnosticeerd via obj-groottevergelijking (4,99 MB stale vs
5,19 MB vers). Les: build-stappen gescheiden uitvoeren en na een build de
artefactgrootte verifiëren, niet alleen de timestamp.

## v2.0 — professionele restyling + bibliotheekbrowser (zelfde dag)

Richtlijn van Neil: nette moderne utility-app, geen namaak-hardware; de
DAW-mockup is alleen inspiratie voor indeling. Engine ongewijzigd.

**Styling**
- Segoe UI 19px (+ semibold voor paneelkoppen) i.p.v. het pixelfont;
  vangnet naar het standaardfont als de TTF ontbreekt.
- Eigen donker thema (`applyModernStyle()`): rustige charcoal-achtergrond,
  cards (#15181f) met subtiele borders (#292f3c), afgeronde hoeken,
  consistente padding/spacing, blauwe accenten, groene Start / rode Stop.
- Zijbalk = drie losse auto-hoogte cards (DEVICES / STATUS / LEVELS, met
  uitgelijnde key-value-rijen); hoofdpaneel = controls-card + tabs-card +
  statusregel onderaan. Venster 1240×880.

**Bibliotheekbrowser (Mijn modellen)**
- Drie panelen: categorieën (incl. "Alle" en "Favorieten" met tellingen) |
  modellenlijst met zoekveld | detailpaneel.
- Details: naam (semibold), type/categorie, maker (heuristiek op
  "Voornaam L"-prefix), bron (TONE3000-register / GitHub-catalogus /
  lokaal), bestandsgrootte, datum, bestandsnaam; knoppen Laden en
  favoriet-toggle.
- Dubbelklik in de lijst = model laden (hot-swap indien actief); geladen
  model gemarkeerd; favorieten met * en bewaard in `settings.json`.

Klik-verificatie op afstand bleek onbetrouwbaar (DPI/multi-monitor);
categorie-selectie aantoonbaar werkend, rest visueel geverifieerd.

## v2.1 — presets + compacte controls (zelfde dag)

**Presets** (gevraagd door Neil: "stemmen", "heavy metal over speakers",
"akoestisch koptelefoon"): een preset = naam + model + input gain + gate +
master + EQ + **outputdevice**, opgeslagen in `presets.json`. Preset-regel
bovenin de controls-card: kiezen = direct toepassen (sliders + atomics; bij
actieve engine hot-swap van het model, en engine-herstart als het
outputdevice wisselt). Knoppen: Opslaan als... (modal met naam),
Bijwerken (huidige stand in de actieve preset), Verwijder.

**Compacte controls** (feedback Neil: sliders van "een meter breed" waren
zonde van de ruimte): Gain / Gate / Master / Bass / Mid / Treble nu als zes
gelijke compacte sliders op één regel met de labels in de slider, plus EQ
reset; Start/Stop terug naar normale knophoogte. De bibliotheek krijgt de
vrijgekomen hoogte. Globale layout verder onaangeroerd (expliciete wens).

**Visie genoteerd voor volgende sessie** (van Neil): de bibliotheek als
kern — automatische metadata per .nam (merk/model/jaar/stijl/cab/mic),
zoeken op "Fender clean", ratings; bron-metadata kan bij TONE3000-downloads
direct uit de API (tags/makes/description) worden meegeslagen.

## v2.2 — apparaatvolume (zelfde dag)

Probleem (Neil): headset superzacht; de Windows-volumeslider regelde het
default-apparaat (S/PDIF) en niet de headset waar de engine exclusief op
speelt. Master hoger zetten is riskant omdat die stand mee terugverhuist
naar de speakers — al ondervangen presets dat (master zit in de preset).

Oplossing: **Apparaatvolume-slider** in de DEVICES-card, gekoppeld aan
`IAudioEndpointVolume` van het gekozen outputdevice (= de Windows-slider
van precies dát apparaat). Bij USB/analoge devices is dat hardwarevolume en
werkt het dus ook tijdens exclusive playback. Bij digitale uitgangen zonder
hardwarevolume toont de app de hint "(digitale uitgang: volume via Master)".
Poll elke ~30 frames; direct schrijven bij slepen. Diagnose bevestigde
meteen de oorzaak: de G430 stond op 29%.

## v2.3 — metadata-bibliotheek (zelfde dag)

Neil's visie uitgevoerd: de bibliotheek als lokale Tone3000-achtige library.

- **`models\metadata.json`**: per bestand merk / stijl / cab / mic / maker /
  bron / tags / rating (0–5).
- **Drie bronnen**, in volgorde van kwaliteit:
  1. TONE3000-API: nieuwe downloads krijgen makes/tags/creator direct mee;
     de knop **"Metadata ophalen"** (onder de categorieën) doet een backfill
     voor alle geregistreerde downloads via `GET /tones/{id}`;
  2. bestandsnaam-heuristiek (`deriveMeta`): stijl (clean/crunch/high-gain),
     mics (SM57/R121/414/…), cab (NxM-patroon), maker ("Voornaam L"-prefix),
     merk via de categorie-trefwoorden — vult alleen lege velden;
  3. handmatig: stijl-dropdown en rating-slider in het detailpaneel.
- **Zoeken over alles**: spatie-gescheiden termen matchen AND-gewijs op naam
  + alle metadata, dus "fender clean" en "marshall crunch" werken.
- Downloadthreads leveren metadata via een pending-map (mutex) af aan de
  UI-thread, die merged (incl. stijl-uit-tags) en opslaat — nooit
  bestands-I/O op het audiopad.
- Eerste run op Neil's 26 modellen: heuristiek pakte o.a. "Ampeg SVT 6x10"
  → Bas/6x10/SM57 en "DRRI Clean SM57 + Royer R-121" → Fender/Clean correct.

## v2.4 — cabinet-IR's (zelfde dag)

Grootste resterende klankwinst: veel captures zijn amp-only en klinken schel
op full-range; een cabinet-IR erachter lost dat op.

- **Engine**: IR-convolutie direct na het NAM-model (voor DC-blocker/EQ).
  Taps omgekeerd opgeslagen zodat de convolutie een contiguë dot-product is
  (AVX2-auto-vectorisatie); tail-buffer over blokgrenzen; max 4096 taps
  (85 ms). Zelfde pending/retired-hot-swap als het model, IR overleeft
  engine-herstart. Gemeten: +0,07 ms/blok bij 1024 taps — verwaarloosbaar.
- **IR-laden**: map `irs\` naast `models\`; decodering via de bestaande
  MF-decoder (mono-gemiddeld, energie-genormaliseerd zodat het volume niet
  verspringt). Leeg pad = IR uit.
- **UI**: Cabinet-rij onder Model (dropdown + Ververs); selectie in
  `settings.json` én in **presets** (dus "Metal speakers" kan een 4x12-IR
  bevatten en "Koptelefoon" een andere).
- **TONE3000**: formaat-schakelaar NAM-modellen / **IR's (cabinets)** in de
  zoektab — zelfde zoek/populariteit/download-flow, IR's landen in `irs\`.
- Getest met een delta-IR (transparant): auto-load uit settings, engine
  stabiel (0 underruns), correcte DSP-tijd.

## v2.5 — IR's in de bibliotheek + verwijderen (zelfde dag)

- **"Cabinets (IR's)"** als eigen ingang in de BIBLIOTHEEK-kolom: de
  middenlijst toont dan de `irs\`-map (zelfde zoekveld), dubbelklik =
  gebruiken als cabinet, actieve IR gemarkeerd; detailpaneel met grootte,
  datum en "Gebruik als cabinet".
- **Verwijderen van schijf** voor modellen én IR's via het detailpaneel,
  met bevestigingsdialoog (rode knop). Opruimlogica: favorieten/metadata
  mee verwijderd, selectie-indices hersteld op pad/label-match, en bij het
  verwijderen van de actieve IR wordt de convolutie netjes uitgeschakeld.
- End-to-end geverifieerd (IR geselecteerd, verwijderd, telling 8→7).

## v2.6 — branding via ComfyUI (zelfde dag)

- Artwork gegenereerd via Neil's lokale ComfyUI-API (poort 8188, SDXL base,
  RTX 5080): drie kandidaten, gekozen voor de neon-blauwe gitaar op donker
  navy (past exact bij het app-thema). Prompt-stijl: "minimalist flat app
  icon, neon blue, dark navy, clean vector".
- Nabewerking met System.Drawing: uitsnede, afgeronde hoeken met
  alfamasker, multi-size `app.ico` (256→16, PNG-entries) + `logo.png`.
- Integratie: icoonresource (`app.rc` → rc.exe → `app.res` in de link,
  `wc.hIcon`) voor Verkenner/taakbalk/venster; zijbalk-header met logo
  (WIC → D3D11-texture → `ImGui::Image`) + titel in semibold;
  bureaublad-snelkoppeling "Guitar Audio Lab".
- Besluit: drift-microresampler (ASRC) voorlopig geparkeerd — de huidige
  correctie is in de praktijk onhoorbaar (0 drops bij recente runs).

## v2.7 — draaiknoppen + amp-artwork (zelfde dag)

Neil's "voorlopig geen knopjes" uit v2.0 officieel verlopen; nu de basis
staat mag de app smoel krijgen.

- **Draaiknoppen**: eigen `Knob()`-widget, vector-getekend met ImDrawList
  (waardeboog in accentblauw, wijzer, label boven, waarde onder) — geen
  bitmaps, dus scherp op elke DPI en automatisch in themakleuren.
  Bediening: verticaal slepen, scrollwiel, dubbelklik = standaardwaarde.
  Gain/Gate/Master + Bass/Mid/Treble vervangen de compacte sliders.
- **Amp-artwork per categorie**: 11 illustraties via ComfyUI (SDXL,
  consistente stijl: donker/near-black met blauwe rim light; tweede pass
  nodig omdat de eerste ronde grijze achtergronden gaf). In
  `native\assets\amp_<slug>.png`; lazy geladen als D3D11-texture en getoond
  bovenin het detailpaneel op basis van de modelcategorie; de IR-weergave
  krijgt de 4x12-cabinetillustratie.

## v2.8 — bipolaire EQ, rig-thumbnails, over-dialoog (zelfde dag)

- **Bipolaire knobs**: `Knob()` kreeg een `bipolar`-modus — de waardeboog
  loopt vanaf 12 uur naar links of rechts, zodat ±dB in één oogopslag leest;
  toegepast op Bass/Mid/Treble.
- **Actieve-rig-thumbnails**: mini-illustraties (42×27) van de
  categorie-art naast de Model- en Cabinet-combo's — je ziet direct welk
  type amp en of er een cabinet actief is.
- **"Over Guitar Audio Lab"**: klik op het zijbalk-logo → modal met de
  Pignose-koffer-illustratie (ComfyUI, kandidaat 8), versie, en credits
  (NAM-core MIT, TONE3000, Dear ImGui, ComfyUI/SDXL-artwork).
  Asset: `assets\about.png`.

## Ideeën voor v2

- noise gate (met drempel-slider) vóór het model;
- cabinet-IR-keuze (AudioDSPTools `ImpulseResponse`);
- presets (Clean/Crunch/Heavy = model + gain + master), opslag in json;
- exe-icoon, snelkoppeling, minimaliseren naar tray;
- inputmeter pre-gain tonen (clip-indicator van de kabel zelf);
- ASRC i.p.v. sample-repeat/drop-driftcorrectie.
