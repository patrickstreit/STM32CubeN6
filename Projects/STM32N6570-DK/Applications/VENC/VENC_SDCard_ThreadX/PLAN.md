# Plan: Dual-Stream (VC0+VC1) → Composite-H.264 auf SD-Karte

Arbeitsdokument für die Umsetzung auf diesem Branch. Ein lokaler Agent auf dem
Bench-PC (Hardware angeschlossen) arbeitet die Phasen ab und **trägt Messwerte
und Checkbox-Status hier ein**. Werkzeugkette und Stolperfallen: siehe
`.claude/skills/bench/SKILL.md` (Repo-Root) sowie `Tools/csi/README.md`.

## Kontext

Quelle: 2× IMX258 → Lattice CrossLink-NX LIFCL-40 (VIP-Board, N:1-CSI-2-
Aggregator-Referenzdesign) → CSI-2, 2 Lanes @1485 Mbit/s. **VC0 und VC1 je
1920×1080 RAW10 RGGB @49,6 fps** (vermessen, `Tools/csi/README.md`; die 48 fps
der alten Notiz stammen aus einem 500-ms-Fenster, M0 misst die Frameperiode zu
20,146 ms = 49,64 fps). Kein I²C zu den Sensoren. Encoder-Pfad heute: Pipe1
(Demosaic RGGB + CSC + Downsize) → VENC 720p30 YUYV → FileX/SD @2 Mbit/s;
Trace: ~27 ms/720p-Frame.

Ziel: beide Kameras aufzeichnen, 25–30 fps/Kamera bei ≥440×880, Bitrate
10 Mbit/s, möglichst kein Frame-Verlust am Input. **Nach M0/M1 erreichbar sind
24,8 fps/Kamera** — die Quelle verschachtelt beide Kanäle, mehr als die Hälfte
ihrer 49,64 fps ist mit einer zeitgeteilten Pipe nicht zu holen. Die 25-fps-
Untergrenze wird also um 0,2 fps verfehlt; wer sie braucht, muss die Quellrate
anheben oder auf CrossLink-Variante C (siehe Ausbaustufe).

Entschieden: **Composite-Stream 448×1792, eine Encoder-Instanz.** Die
Kamerabilder liegen als Segmente in einem Frame: heute 2×448×896 (Segment k bei
Y-Offset k·448·896, UV analog, NV12-Pitch = 448), später **4 VCs à 448×448 bei
identischer Gesamtauflösung 448×1792** — Buffer-Layout bleibt gleich.
**Korrektur aus M1: 440 geht nicht.** Der Pixel-Packer-Pitch ist ein Bytewert,
den die Hardware auf ein Vielfaches von 16 festlegt (`IS_DCMIPP_PIXEL_PIPE_PITCH`
prüft `(PITCH & 0xF) == 0`; die Asserts sind im Build aus, der Wert landete also
still falsch im Register). 448 erfüllt das, ist zugleich ein glattes Vielfaches
von 16 Pixeln und macht die Padding-Spalte überflüssig.
Performance-Check: 448×1792 = 28×112 = 3136 Makroblöcke, +4 % ggü. 880×880 —
vernachlässigbar. Gemeinsames ISP-Tuning für beide Kameras; Crop/Downsize im
DCMIPP (kein CrossLink-Build für den Einstieg).

### Die entscheidende Unbekannte (→ M0)

Lattice zum N:1-Design: „assigns a unique virtual channel ID to each channel
and data will be sent alternately between channels" — Granularität
unspezifiziert:

- **Frames seriell** (SOF₁ erst nach EOF₀): Pipe1-Alternierung kann potenziell
  **beide VCs vollständig** einfangen (bis 48 fps/Kamera, drop-frei); Grenze ist
  dann Switch-Latenz im Frame-ISR bzw. VENC-Durchsatz.
- **Paketweise verschachtelt**: während Pipe1 einen VC0-Frame captured, laufen
  VC1-Pakete vorbei → 24 fps/Kamera-Decke.

**Beantwortet (M0, 2026-08-26): verschachtelt.** Die beiden Frames laufen fast
vollständig gleichzeitig über die Leitung — VC1 startet, 6,5 µs später startet
VC0, beide übertragen 19,75 ms lang, dann enden sie in derselben Reihenfolge
(Muster `S1 S0 E1 E0`, 149 Überlappungen in 149 Frames, keine einzige serielle
Frameübertragung). Damit gilt die 1/2-Decke: **≈24,8 fps pro Kamera**, und M1
zeigt, dass die Zeitmultiplex-Mechanik genau das auch erreicht — der Verlust ist
strukturell, nicht implementierungsbedingt.

## Hardware-Fakten (Rechercheergebnis, mit Quellenlage)

- Nur **Pipe1 hat den ISP** (Demosaic, BLC, Gain, CCM, YUV) — „instantiated
  only once" (ST-Wiki). Pipe2 unabhängig (PIPEDIFF=1) liefert nur Raw-Bayer →
  für 2 RAW-Kameras ist Zeitmultiplex von Pipe1 der einzige Farb-Pfad.
- **Per-Frame-VC-Switch ist auf dieser Hardware bewiesen**: `csi_preview.c`
  (`dual 0 1`) schreibt `P1FSCR.VC` + Zieladresse im Frame-Complete-ISR; das
  Shadow-Register latcht am Frame-Start, kein Pipe-Stop/Start nötig.
- VENC (Hantro VC8000NanoE): **kein eigener Kernel-Takt** (AXI-Domäne,
  400 MHz); 27 ms/720p ≈ Datasheet-Klasse (1080p15) → nahe Nominal. ST benennt
  Memory-Bandbreite als Bottleneck; „VENC liest Chroma doppelt → ø 16 bpp
  Input-Traffic".
- Encode-Zeit-Hebel: Auflösung (448×1792 = 3136 MBs → rechnerisch ~24 ms bei
  heutigen 7,5 µs/MB), NV12 statt YUYV, Input-Buffer AXISRAM statt uncached
  PSRAM, `enableCabac=2` (offiziell „Performance optimized": Intra CAVLC /
  Inter CABAC), `transform8x8Mode=0`. **Höhere Bitrate macht Encoding nicht
  schneller** (eher minimal langsamer); 10 Mbit/s unkritisch (API bis
  40 Mbit/s, Level 4.1 reicht).
- Budget: 448×1792@25 ≈ 59 % VENC-Auslastung → Luft bis ~30 fps Composite. Nach
  M1 liegt die Composite-Rate bei 24,8 fps (ein Composite = je ein Segment pro
  Kanal), also im günstigen Ast dieser Rechnung.
  **4 Sensoren: Gesamtauflösung bleibt 448×1792 → VENC-Budget unverändert**;
  limitierend ist nur die Capture-Seite (verschachtelt, durch M0 bestätigt:
  ~fps/4 pro Kamera, also ~12,4 fps).
- HW-Handshake (Slice-Mode) ist mit VC-Zeitmultiplex inkompatibel (one-way
  `venc_rdy`, FUSE_ERROR, Errata ES0620 §2.2.14) → Frame-Mode.
- Bildqualität: Encoder-Pfad hat weder BLC noch Gain noch CCM → „washed out"
  (siehe csi-README); die drei DCMIPP-Blöcke brauchen keinen Sensor.
- Speicher: AXISRAM 3,4 MB (`.noncacheable` 2,77 MB, davon 2 MB
  Bitstream-Ring), PSRAM 32 MB (**GCC-Linkerscript sieht fälschlich nur
  16 MB**), EWL-Pool 8 MB PSRAM. Composite-NV12 448×1792 = 1,20 MB ×2
  (Ping-Pong). Forum-Warnung: Ref-Frames in PSRAM → Timing-Artefakte.
- SD-Pfad bereits ertüchtigt (Queue 30, 64-Sektor-Cache, gepaddete Writes);
  2×10 Mbit/s ≈ 2,5 MB/s unkritisch.

## Phase M — Messungen (zuerst; je mit maschinenlesbarem Konsolen-Report)

Alle M-Kommandos geben einen Report mit festem Präfix und key=value-Zeilen aus
(Stil des `probe`-Reports, z. B. `=== PHASE RESULT ===` … `=== END ===`), damit
die Auswertung skriptbar ist. Zusätzlich TraceX-Events (neue Instr-IDs in
`Tools/instrumentation/instrumentation.yaml`) für Perfetto.

- [x] **M0 — VC-Phasen-/Serialisierungsmessung** (CsiProbe-Build, neues
  Kommando `phase [n]`, `Appli/Core/Src/csi_phase.c`): per-VC SOF/EOF-Callbacks
  (`HAL_DCMIPP_CSI_StartOfFrameEventCallback`/`EndOfFrameEventCallback`) mit
  DWT-Cycle-Timestamps über n Frames. Kein Pixel-Pipe wird angefasst — die
  Frame-Delimiter sind Short Packets, die der Receiver pro VC meldet, egal ob
  eine Pipe den Kanal konsumiert.
  **Ergebnis (`phase 120`, 148 Frames je VC über 3,0 s):** `verschachtelt`.
  Muster `S1 S0 E1 E0` durchgehend, 149 Overlaps, 0 serielle Frames.
  Gap (Ende VC0-Frame → Start nächster VC1-Frame) = **387,5 µs**
  (min 386,7 / max 388,0). Transferdauer = **19,75 ms** je VC
  (VC0 19,726–19,772 ms, VC1 19,732–19,778 ms) bei **20,146 ms** Frameperiode
  → 49,64 fps je Kamera, Jitter 46,6 µs. VC0 startet 6,5 µs nach VC1 und läuft
  parallel zu ihm: die Leitung ist zu 98 % der Zeit mit beiden Kanälen
  gleichzeitig belegt.
- [x] **M1 — Drop-freier Alternating-Capture ohne VENC-Pfad** (Kommando
  `mux <vcA> <vcB> [s]`, `Appli/Core/Src/csi_mux.c`): Pipe1 alternierend
  (VC-Switch + Semiplanar-Adressen im Frame-Complete-ISR, Mechanik aus
  `csi_preview.c`), Crop 1920×1080→540×1080+690+0 + Downsize→448×896, Ziel =
  Composite-Segmente (NV12, Pitch 448, Segment k bei Y-Offset k·448·896).
  Report: captured fps je VC vs. Quellrate, Drop-Zähler, `P1CFSCR`-Verifikation,
  ISR-Switch-Latenz.
  **Akzeptanz:** bei serieller Quelle 0 Drops über ≥60 s; sonst dokumentierte
  1/2-Rate ohne Zusatzverluste. → **erfüllt** (verschachtelte Quelle, exakt
  1/2-Rate, keine Zusatzverluste).
  **Ergebnis (`mux 0 1 60`, 60,06 s):** VC0 = **24,82 fps**, VC1 = **24,80 fps**
  bei 49,64 fps Quellrate je Kanal → Keep-Rate 50,00 % / 49,96 %.
  Drops = 1491 / 1492, **vollständig strukturell**: 2981 Pipe-Frames in 2982
  Quell-Frameperioden, d. h. die Pipe hat in jeder Frameperiode genau einen
  Kanal aufgenommen und über 60 s nur eine einzige Periode verpasst.
  `vc_mismatch = 0` (der Switch greift jedes Mal), 0 Pipe-Overruns.
  ISR-Switch selbst: **0,3 µs** avg (max 0,9 µs); vom Link-EOF bis zum
  vollzogenen Switch **5,7 µs** (max 6,0 µs) — gegen 387,5 µs verfügbaren Gap
  also ~380 µs Reserve. Die Switch-Latenz ist nicht der Engpass, die
  Verschachtelung der Quelle ist es. Beide Segmente tragen echtes Bild
  (Luma-Mittel 122 bzw. 148, zwei verschiedene Kameras).
- [ ] **GATE (User-Entscheid): die ursprüngliche Bedingung greift nicht.** Er war an
  „M0/M1 zeigen serielle Frames" geknüpft; die Quelle ist verschachtelt, und die
  Switch-Latenz (5,7 µs gegen 387,5 µs Gap) ist gerade nicht das Problem. Eine
  CrossLink-Anpassung auf 25 fps pro Sensor würde den Capture nicht drop-frei
  machen, sondern nur auf 12,5 fps pro Kamera halbieren — sie ist hier also
  kontraproduktiv.
  **Offener User-Entscheid stattdessen:** 24,8 fps/Kamera akzeptieren und mit
  M2/Phase 1 weitermachen (Empfehlung — der Rest des Pfads ist davon unberührt),
  oder vorher CrossLink-Variante C (FPGA-Composite) angehen, wenn die vollen
  49,6 fps bzw. später >12,4 fps bei 4 Sensoren gebraucht werden.
- [ ] **M2 — VENC-Zeitmodell** (Encoder-Build): Baseline 720p YUYV (~27 ms),
  dann einzeln: (a) NV12 (`DCMIPP_PIXEL_PACKER_FORMAT_YUV420_2` +
  `H264ENC_YUV420_SEMIPLANAR`, Pfade existieren), (b) `input_frame` in AXISRAM
  (Bitstream-Ring temporär verkleinern), (c) `enableCabac=2`,
  (d) `transform8x8Mode=0`, (e) Bitrate 2 vs. 10 Mbit/s.
  **Ergebnis:** µs/MB je Variante → welche Composite-Rate (24/25/30/48)
  verträgt 448×1792; finale Buffer-Platzierung. Zielrate steht nach M1 fest:
  24,8 fps Composite.
- [ ] **M3 — Störpakete**: 0x12/0x2f-Pakete und permanenter IDERR korrumpieren
  den Pipe1-Semiplanar-Capture nicht (im Preview bekannt harmlos; einmal im
  Encoder-Setup verifizieren).

### Rohdaten der Messungen (2026-08-26, CsiProbe-Build)

Reproduzierbar von einem kalten Board aus, `dutpower serve` läuft dabei in
einem eigenen Terminal:

```powershell
./Tools/debug/stm32n6-gdb.ps1 -Build -FlashAppli -Preset CsiProbe
./Tools/debug/csi-console.ps1 -OnPromptCommand 'dutpower cycle' `
    -WaitFor "first frame after" -Sequence "link 280000"
./Tools/debug/csi-console.ps1 -OnPromptCommand 'dutpower cycle' `
    -Sequence "probe 2500"                       # Geometrie für 'mux'
./Tools/debug/csi-console.ps1 -Sequence "phase 120"
./Tools/debug/csi-console.ps1 -Sequence "mux 0 1 60"
```

`phase` braucht keinen Power-Cycle, `mux` auch nicht — beide laufen auf dem
Link, den `link` hochgezogen hat. Zeiten sind Mikrosekunden mit einer
Nachkommastelle, Quelle ist DWT->CYCCNT bei 800 MHz.

Auf diesem Bench-PC ist `dutpower` nicht als Tool installiert; es läuft aus
seinem Repo-venv, also `C:\repositories\dutpower\.venv\Scripts\dutpower.exe`
statt `dutpower`. Und COM16 muss frei sein — ein serieller Monitor in VS Code
hält den Port sonst fest und `csi-console.ps1` kommt nicht daran.

```
=== PHASE RESULT ===
cpu_hz=800000000        vc_mask=0x3           events=594
events_capped=0         window_ms=3003        channels=2
order=interleaved       overlaps=149          same_vc_runs=0
pattern=S1 S0 E1 E0 S1 S0 E1 E0 S1 S0 E1 E0 S1 S0 E1 E0 S1 S0 E1 E0
vc0.frames=148  vc0.sof=149  vc0.eof=148  vc0.fps_x100=4928
vc0.xfer_us   min 19726.0  avg 19752.4  max 19772.1   (n=148)
vc0.period_us min 20119.2  avg 20146.1  max 20165.8   jitter 46.6
vc1.frames=148  vc1.sof=149  vc1.eof=148  vc1.fps_x100=4928
vc1.xfer_us   min 19731.5  avg 19757.9  max 19777.6   (n=148)
vc1.period_us min 20119.2  avg 20146.1  max 20165.8   jitter 46.6
gap_inter_us  min   386.7  avg   387.5  max   388.0   (n=148)
gap_intra_n=0           gap_0to1_us_avg=387.5
=== END ===
```

`gap_intra_n=0` heisst: es gab keine einzige Stelle, an der auf das Ende eines
Kanals der nächste Frame desselben Kanals folgte — die Kanäle wechseln sich
strikt ab, nur eben überlappend statt nacheinander.

```
=== MUX RESULT ===
cpu_hz=800000000  window_ms=60064
source=1920x1080  crop=540x1080+690+0  segment=448x896  composite=448x1792
format=NV12 pitch=448  composite_base=0x906bb800  composite_bytes=1204224
seg0.vc=0  y=0x906bb800  uv=0x9077f800   seg1.vc=1  y=0x9071d800  uv=0x907b0800
src.vc0.frames=2982  cap.vc0.frames=1491  drops=1491  keep=50.00 %  luma_mean=122
src.vc1.frames=2982  cap.vc1.frames=1490  drops=1492  keep=49.96 %  luma_mean=148
src.fps=49.64 je Kanal        cap.fps=24.82 / 24.80
captured_total=2981           vc_mismatch=0      eof_unassociated=0
isr_switch_us     min 0.3  avg 0.3  max 0.9   (n=2981)
eof_to_switch_us  min 5.4  avg 5.7  max 6.0   (n=2981)
p1fscr=0x0008002b  p1cfscr=0x0008002b
dcmipp_error=0x00000900  csi_sr0=0x48183300
=== END ===
```

`dcmipp_error=0x900` = `CSI_ERROR_DATA_ID | CSI_ERROR_SYNC`. Das Data-ID-Bit ist
der bekannte, harmlose IDERR (die 0x12/0x2f-Pakete beansprucht niemand, siehe
csi-README §4); das Sync-Bit stammt aus dem Kanal-Stopp am Ende von `probe`.
**Kein `PIPE1_OVR`**, und `status` zählt 0 Pipe-Fehler — der Capture-Pfad ist
über die 60 s nie übergelaufen. `errors 2000` direkt danach: id 0, ecc 0, crc 0,
sync 0, phy 0 bei 100 Frames — der Link ist sauber.

Nicht gemacht: die im Plan zusätzlich vorgesehenen TraceX-Events. Die
ISR-Latenzen stehen als min/avg/max im Konsolen-Report, dafür braucht es kein
Perfetto; neue Instr-IDs wären erst nützlich, wenn Capture und Encoder im selben
Trace korreliert werden müssen (M2/Phase 2).

## Phase 1 — Composite-Capture produktiv

Dateien: `Appli/Core/Src/dcmipp_app.c`, `venc_app.c`, `venc_h264_config.*`
(neue Config `venc_h264_config_448x1792_Frame.h`; **Segmentanzahl
parametrisiert**, damit die 4-VC-Stufe nur die Segmentliste ändert).

- [ ] VC-Switch + Adress-Flip im Frame-Complete-ISR (beide VCs identische
  Geometrie → nur VC-Feld + Semiplanar-Adressen wechseln, keine weitere
  Pipe-Reconfig).
- [ ] Pairing-/Overflow-Logik: Composite komplett = alle Segmente desselben
  Ping-Pong-Slots gefüllt → `FRAME_RECEIVED_FLAG`; fehlt ein Segment, Inhalt
  des Vorgängers stehen lassen und Zähler tracen. `frame_received`/
  `GetNextFrame` auf Composite-Slots umstellen.
- [ ] LCD-Preview (Pipe2 shared) zeigt alternierende Segmente → deaktivieren
  oder als Debug-Ansicht dokumentieren.

## Phase 2 — Encoder 448×1792 NV12 @ 10 Mbit/s

- [ ] `H264EncConfig`: 448×1792, `frameRate` = gemessene Composite-Rate (M1:
  24,8 fps),
  Level 4.1, NV12-Preproc; Rate-Ctrl: `bitPerSecond=10_000_000`, `hrdCpbSize`
  anpassen, `gopLen` ≈ Framerate; Coding nach M2 (`enableCabac`,
  `transform8x8Mode`).
- [ ] Buffer-Layout nach M2: Composite-Buffer und/oder EWL-Ref-Frames nach
  AXISRAM; Bitstream-Ring auf 1–1,5 MB; EWL-Pool 8 → ~4 MB; Linkerscript-Fix
  PSRAM 16→32 MB (`STM32N657XX.ld`).
- [ ] Verifikation: Encode-Zeit-Trace; `H264ENC_FUSE_ERROR`-frei ≥10 min;
  ffprobe/ffplay: 448×1792, Ziel-fps, ~10 Mbit/s.

## Phase 3 — SD/Aufzeichnung

- [ ] `sdcard_app.c`: `NB_FRAMES_PER_FILE` auf Zeitbasis der neuen Framerate;
  Bitrate-Monitor aktivieren; Dauerlauf (Dateirotation ohne Encoder-Stall —
  Queue 30 puffert ~2,4 s bei 10 Mbit/s).

## Phase 4 — Bildqualität

- [ ] Pipe1: `SetISPBlackLevelCalibrationConfig` (BLC), `SetISPExposureConfig`
  (per-Kanal-Gain/WB), `SetISPColorConversionConfig` (CCM) mit festen
  Startwerten; ein gemeinsamer Satz für beide Kameras. Optional später:
  SW-AE-Loop auf DCMIPP-Gains aus Pipe1-Statistik.

## Ausbaustufe (separat, nicht dieser Branch): CrossLink

Aufwandsschätzung der Varianten (angefragt):

| Variante | Nutzen | Aufwand/Risiko |
|---|---|---|
| A. N6-VC-Zeitmultiplex (dieser Plan) | **gemessen 24,8 fps/Kamera** (M0/M1), kein FPGA-Build | FW-Arbeit hier; Switch-Mechanik bewiesen und vermessen |
| B. FPGA Frame-Interleave auf 1 VC | entlastet N6 (kein VC-Switch), gleicher Durchsatz wie A | ~1 Woche FPGA, kein Durchsatzgewinn → lohnt allein kaum |
| C. FPGA Composite (Segmente in einem Frame) | volle Sensorrate, perfekte Paarung, drop-frei; **M0 zeigt „verschachtelt", damit ist dies der einzige Weg über 24,8 fps/Kamera hinaus** und ab 4 Sensoren (~12,4 fps/Kamera) voraussichtlich nötig | setzt Sensor-Genlock voraus (IMX258 ohne HW-Trigger → gemeinsamer Takt + gleichzeitiger I²C-Start vom CrossLink; Machbarkeit 1–2 Tage abklären). Ohne Genlock bräuchte der FPGA Full-Frame-Puffer (2,6 MB) > CrossLink-EBR. Implementierung grob 1–3 Wochen inkl. HW-Iterationen |

Die N6-Firmware wird so gebaut, dass die Capture-Seite austauschbar ist: der
Composite-Buffer ist identisch, egal ob per VC-Multiplex oder später per
FPGA-Composite befüllt. Sensor-Sync ist zugleich Voraussetzung für das spätere
Ziel „Input-Rate = Encode-Rate ohne Drop".

## Verifikation (gesamt)

1. M0–M3-Reports/Traces (Konsole + Perfetto) hier in PLAN.md eintragen.
2. Phase 1: Zählerkonsistenz VC0/VC1/Composite über ≥60 s, keine unerklärten
   Drops.
3. Phase 2/3: 10-min-Aufnahme; ffprobe (Auflösung/fps/Bitrate); visuelle
   Prüfung beider Segmente (Objektiv abdecken → nur ein Segment dunkel).
4. Regressionscheck: Single-VC-720p-Build (bestehende Config) baut und läuft.
