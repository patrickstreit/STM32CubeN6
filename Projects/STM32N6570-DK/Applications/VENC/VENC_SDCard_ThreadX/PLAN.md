# Plan: Dual-Stream (VC0+VC1) → Composite-H.264 auf SD-Karte

Arbeitsdokument für die Umsetzung auf diesem Branch. Ein lokaler Agent auf dem
Bench-PC (Hardware angeschlossen) arbeitet die Phasen ab und **trägt Messwerte
und Checkbox-Status hier ein**. Werkzeugkette und Stolperfallen: siehe
`.claude/skills/bench/SKILL.md` (Repo-Root) sowie `Tools/csi/README.md`.

## Kontext

Quelle: 2× IMX258 → Lattice CrossLink-NX LIFCL-40 (VIP-Board, N:1-CSI-2-
Aggregator-Referenzdesign) → CSI-2, 2 Lanes @1485 Mbit/s. **VC0 und VC1 je
1920×1080 RAW10 RGGB @48 fps** (vermessen, `Tools/csi/README.md`). Kein I²C zu
den Sensoren. Encoder-Pfad heute: Pipe1 (Demosaic RGGB + CSC + Downsize) → VENC
720p30 YUYV → FileX/SD @2 Mbit/s; Trace: ~27 ms/720p-Frame.

Ziel: beide Kameras aufzeichnen, 25–30 fps/Kamera bei ≥440×880, Bitrate
10 Mbit/s, möglichst kein Frame-Verlust am Input.

Entschieden: **Composite-Stream 440×1760, eine Encoder-Instanz.** Die
Kamerabilder liegen als Segmente in einem Frame: heute 2×440×880 (Segment k bei
Y-Offset k·440·880, UV analog, NV12-Pitch = 440), später **4 VCs à 440×440 bei
identischer Gesamtauflösung 440×1760** — Buffer-Layout bleibt gleich.
Performance-Check: 440×1760 = 28×110 = 3080 Makroblöcke (440 nicht 16er-aligned
→ 8-px-Padding-Spalte), +2 % ggü. 880×880 — vernachlässigbar. Gemeinsames
ISP-Tuning für beide Kameras; Crop/Downsize im DCMIPP (kein CrossLink-Build für
den Einstieg).

### Die entscheidende Unbekannte (→ M0)

Lattice zum N:1-Design: „assigns a unique virtual channel ID to each channel
and data will be sent alternately between channels" — Granularität
unspezifiziert:

- **Frames seriell** (SOF₁ erst nach EOF₀): Pipe1-Alternierung kann potenziell
  **beide VCs vollständig** einfangen (bis 48 fps/Kamera, drop-frei); Grenze ist
  dann Switch-Latenz im Frame-ISR bzw. VENC-Durchsatz.
- **Paketweise verschachtelt**: während Pipe1 einen VC0-Frame captured, laufen
  VC1-Pakete vorbei → 24 fps/Kamera-Decke.

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
- Encode-Zeit-Hebel: Auflösung (440×1760 = 3080 MBs → rechnerisch ~23 ms bei
  heutigen 7,5 µs/MB), NV12 statt YUYV, Input-Buffer AXISRAM statt uncached
  PSRAM, `enableCabac=2` (offiziell „Performance optimized": Intra CAVLC /
  Inter CABAC), `transform8x8Mode=0`. **Höhere Bitrate macht Encoding nicht
  schneller** (eher minimal langsamer); 10 Mbit/s unkritisch (API bis
  40 Mbit/s, Level 4.1 reicht).
- Budget: 440×1760@24 ≈ 55 % VENC-Auslastung → Luft bis ~30 fps Composite.
  **4 Sensoren: Gesamtauflösung bleibt 440×1760 → VENC-Budget unverändert**;
  limitierend ist nur die Capture-Seite (verschachtelt: ~fps/4 pro Kamera).
- HW-Handshake (Slice-Mode) ist mit VC-Zeitmultiplex inkompatibel (one-way
  `venc_rdy`, FUSE_ERROR, Errata ES0620 §2.2.14) → Frame-Mode.
- Bildqualität: Encoder-Pfad hat weder BLC noch Gain noch CCM → „washed out"
  (siehe csi-README); die drei DCMIPP-Blöcke brauchen keinen Sensor.
- Speicher: AXISRAM 3,4 MB (`.noncacheable` 2,77 MB, davon 2 MB
  Bitstream-Ring), PSRAM 32 MB (**GCC-Linkerscript sieht fälschlich nur
  16 MB**), EWL-Pool 8 MB PSRAM. Composite-NV12 440×1760 = 1,16 MB ×2
  (Ping-Pong). Forum-Warnung: Ref-Frames in PSRAM → Timing-Artefakte.
- SD-Pfad bereits ertüchtigt (Queue 30, 64-Sektor-Cache, gepaddete Writes);
  2×10 Mbit/s ≈ 2,5 MB/s unkritisch.

## Phase M — Messungen (zuerst; je mit maschinenlesbarem Konsolen-Report)

Alle M-Kommandos geben einen Report mit festem Präfix und key=value-Zeilen aus
(Stil des `probe`-Reports, z. B. `=== PHASE RESULT ===` … `=== END ===`), damit
die Auswertung skriptbar ist. Zusätzlich TraceX-Events (neue Instr-IDs in
`Tools/instrumentation/instrumentation.yaml`) für Perfetto.

- [ ] **M0 — VC-Phasen-/Serialisierungsmessung** (CsiProbe-Build, neues
  Kommando `phase [n]`): per-VC SOF/EOF-Callbacks
  (`HAL_DCMIPP_CSI_StartOfFrameEventCallback`/`EndOfFrameEventCallback`) mit
  DWT-Cycle-Timestamps über n Frames. Report: Ordnungsmuster (überlappen sich
  VC0/VC1-Frames?), Inter-VC-Gap min/avg/max, Frame-Transferdauer je VC,
  Jitter.
  **Ergebnis:** `seriell | verschachtelt`, Gap = ___ µs, Transferdauer = ___ ms
- [ ] **M1 — Drop-freier Alternating-Capture ohne VENC-Pfad** (Kommando
  `mux <vcA> <vcB> [s]`): Pipe1 alternierend (VC-Switch + Semiplanar-Adressen
  im Frame-Complete-ISR, Mechanik aus `csi_preview.c`), Crop 1920×1080→ROI +
  Downsize→440×880, Ziel = Composite-Segmente (NV12, Pitch 440, Segment k bei
  Y-Offset k·440·880). Report: captured fps je VC vs. Quellrate, Drop-Zähler,
  `P1CFSCR`-Verifikation, ISR-Switch-Latenz (Trace).
  **Akzeptanz:** bei serieller Quelle 0 Drops über ≥60 s; sonst dokumentierte
  1/2-Rate ohne Zusatzverluste.
  **Ergebnis:** VC0 = ___ fps, VC1 = ___ fps, Drops = ___
- [ ] **GATE (User-Entscheid):** Zeigen M0/M1 serielle Frames, aber 2×48/50 fps
  sind für drop-freien Capture zu schnell (Switch-Latenz, ISP-Durchsatz), wird
  **zuerst der CrossLink auf 25 fps pro Sensor angepasst** und M1 wiederholt —
  erst mit sauberem Capture-Ergebnis weiter in den VENC-Pfad.
- [ ] **M2 — VENC-Zeitmodell** (Encoder-Build): Baseline 720p YUYV (~27 ms),
  dann einzeln: (a) NV12 (`DCMIPP_PIXEL_PACKER_FORMAT_YUV420_2` +
  `H264ENC_YUV420_SEMIPLANAR`, Pfade existieren), (b) `input_frame` in AXISRAM
  (Bitstream-Ring temporär verkleinern), (c) `enableCabac=2`,
  (d) `transform8x8Mode=0`, (e) Bitrate 2 vs. 10 Mbit/s.
  **Ergebnis:** µs/MB je Variante → welche Composite-Rate (24/25/30/48)
  verträgt 440×1760; finale Buffer-Platzierung.
- [ ] **M3 — Störpakete**: 0x12/0x2f-Pakete und permanenter IDERR korrumpieren
  den Pipe1-Semiplanar-Capture nicht (im Preview bekannt harmlos; einmal im
  Encoder-Setup verifizieren).

## Phase 1 — Composite-Capture produktiv

Dateien: `Appli/Core/Src/dcmipp_app.c`, `venc_app.c`, `venc_h264_config.*`
(neue Config `venc_h264_config_440x1760_Frame.h`; **Segmentanzahl
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

## Phase 2 — Encoder 440×1760 NV12 @ 10 Mbit/s

- [ ] `H264EncConfig`: 440×1760, `frameRate` = gemessene Composite-Rate,
  Level 4.1, NV12-Preproc; Rate-Ctrl: `bitPerSecond=10_000_000`, `hrdCpbSize`
  anpassen, `gopLen` ≈ Framerate; Coding nach M2 (`enableCabac`,
  `transform8x8Mode`).
- [ ] Buffer-Layout nach M2: Composite-Buffer und/oder EWL-Ref-Frames nach
  AXISRAM; Bitstream-Ring auf 1–1,5 MB; EWL-Pool 8 → ~4 MB; Linkerscript-Fix
  PSRAM 16→32 MB (`STM32N657XX.ld`).
- [ ] Verifikation: Encode-Zeit-Trace; `H264ENC_FUSE_ERROR`-frei ≥10 min;
  ffprobe/ffplay: 440×1760, Ziel-fps, ~10 Mbit/s.

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
| A. N6-VC-Zeitmultiplex (dieser Plan) | 24–48 fps/Kamera je nach M0-Ausgang, kein FPGA-Build | FW-Arbeit hier; Switch-Mechanik bewiesen |
| B. FPGA Frame-Interleave auf 1 VC | entlastet N6 (kein VC-Switch), gleicher Durchsatz wie A | ~1 Woche FPGA, kein Durchsatzgewinn → lohnt allein kaum |
| C. FPGA Composite (Segmente in einem Frame) | volle Sensorrate, perfekte Paarung, drop-frei; nötig falls M0 „verschachtelt" zeigt und 4 Sensoren > fps/4 pro Kamera brauchen | setzt Sensor-Genlock voraus (IMX258 ohne HW-Trigger → gemeinsamer Takt + gleichzeitiger I²C-Start vom CrossLink; Machbarkeit 1–2 Tage abklären). Ohne Genlock bräuchte der FPGA Full-Frame-Puffer (2,6 MB) > CrossLink-EBR. Implementierung grob 1–3 Wochen inkl. HW-Iterationen |

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
