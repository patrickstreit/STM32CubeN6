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

### Zielvorgabe (Stand 2026-08-27, korrigiert)

**Das Composite ist Breitformat, nicht hochkant.** Die frühere Annahme
448×1792 (Segmente übereinander) war eine Fehlspezifikation; richtig ist
**1792×448** mit den Segmenten nebeneinander.

Endausbau: **4 Sensoren à mindestens 448×448 bei 30 fps** → Composite
**1792×448**. Der Zweisensor-Aufbau auf dem Tisch bildet dasselbe Composite mit
**2 Segmenten à 896×448** ab — gleiche Gesamtauflösung, gleicher Pitch, gleiche
Makroblockzahl, nur eine andere Segmentbreite. Geprüft werden soll ausserdem, ob
**496×496** je Sensor (Composite 1984×496) noch Marge hat.

Bitrate 10 Mbit/s, möglichst kein Frame-Verlust am Input.

**Was sich dadurch ändert — Kurzfassung** (Herleitung in M2-R und in der
Ausbaustufe):

| | alt (448×1792, 2 Sensoren, 24,8 fps) | neu (1792×448, 4 Sensoren, 30 fps) |
|---|---|---|
| Makroblöcke | 3136 | 3136 — **unverändert** |
| Encode-Zeit/Frame | 20,8 ms (hochgerechnet) | **21,3 ms (gemessen)** |
| Budget je Frame | 40,3 ms → 52 % | 33,3 ms → **64 %** |
| Speicher | Ping-Pong + Ring = NOCACHE exakt | **unverändert**, Bytes identisch |
| Segment im Speicher | zusammenhängend, reiner Adress-Flip | **Fenster in breiterem Frame**, Pitch ≠ Breite |
| Capture-Rate | 24,8 fps (= Quelle/2) | **12,4 fps** (= Quelle/4) → **Ziel verfehlt** |

Die ersten vier Zeilen sind unkritisch. Die fünfte ist ein kleiner
Mechanikwechsel in der Capture-Konfiguration, in M1-R auf der Hardware belegt.
**Die sechste ist der eigentliche Bruch:** 30 fps aus vier zeitgeteilten VCs
verlangt eine Quellrate von 120 fps je Kanal; die Quelle liefert 49,64. Damit ist **CrossLink-Variante C
(FPGA-Composite) keine Ausbaustufe mehr, sondern Voraussetzung** — siehe
Ausbaustufe und den Vermerk beim GATE.

*Historisch (alte Zielvorgabe, von M0/M1 beantwortet):* 25–30 fps/Kamera bei
≥440×880, zwei Kameras. Erreichbar waren **24,8 fps/Kamera** — die Quelle
verschachtelt beide Kanäle, mehr als die Hälfte ihrer 49,64 fps ist mit einer
zeitgeteilten Pipe nicht zu holen.

Entschieden: **Composite-Stream 1792×448, eine Encoder-Instanz.** Die
Kamerabilder liegen als Segmente **nebeneinander** in einem Frame: heute
2×896×448 (Segment k in den Spalten [k·896, (k+1)·896), UV analog, NV12-Pitch =
1792), später **4 VCs à 448×448 bei identischer Gesamtauflösung 1792×448** —
Buffer-Layout und Bytezahl bleiben gleich.

**Was die Drehung an der Capture-Konfiguration ändert.** Übereinander gestapelt
war jedes Segment ein zusammenhängender Speicherblock; Pitch = Segmentbreite,
und der Kanalwechsel war nichts als ein Adress-Flip. Nebeneinander ist jedes
Segment ein **Fenster in einem breiteren Frame**: der Pitch muss die
*Composite*-Breite tragen (1792), während der Downsizer weiterhin nur 896 (bzw.
448) Pixel je Zeile liefert, und die Zieladresse von Segment k beginnt k·896
Bytes weiter rechts. Die Hardware kann das ohne Zutun — `P1PPM0PR`/`P1PPM1PR`
sind Bytezähler unabhängig von der aufgenommenen Breite, und die HAL schreibt
für YUV420_2 denselben Wert in beide, was für NV12 genau richtig ist (die
Chroma-Ebene ist so breit wie die Luma-Ebene und halb so hoch, der
Spalten-Offset ist derselbe). Es ist aber der eine Teil der Mechanik, den M1 in
der gestapelten Fassung nie ausgeübt hat → **M1-R**.

**Korrektur aus M1: 440 geht nicht.** Der Pixel-Packer-Pitch ist ein Bytewert,
den die Hardware auf ein Vielfaches von 16 festlegt (`IS_DCMIPP_PIXEL_PIPE_PITCH`
prüft `(PITCH & 0xF) == 0` und `<= 0x7FFF`; die Asserts sind im Build aus, der
Wert landete also still falsch im Register). Diese Bedingung trifft jetzt die
Composite-Breite: 1792 erfüllt sie, ebenso die Segment-Offsets 0/896 bzw.
0/448/896/1344 — und für die 496er-Variante auch 1984 und 0/496/992/1488.
Performance-Check: 1792×448 = 112×28 = 3136 Makroblöcke — dieselbe Zahl wie
gestapelt, die Drehung kostet also von vornherein nichts, und M2-R bestätigt
das gemessen. Gemeinsames ISP-Tuning für beide Kameras; Crop/Downsize im
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
- Encode-Zeit-Hebel — **von M2 auf zwei zusammengeschrumpft**: Auflösung
  (1792×448 = 3136 MBs → M2-R misst 21,3 ms bei 6,80 µs/MB am
  Zielbetriebspunkt, aus AXISRAM 15,4 ms bei 4,90 µs/MB)
  und **Input-Buffer AXISRAM statt uncached PSRAM (−31 %, auf beiden gemessenen
  Szenen)**. NV12 statt YUYV, `enableCabac=2` (offiziell „Performance
  optimized": Intra CAVLC / Inter CABAC) und `transform8x8Mode=0` liegen alle
  im Rauschen — auf der bewegten Szene innerhalb von 0,6 %. Die
  Recherche-Erwartung, hier Zeit zu holen, hat die Messung nicht bestätigt.
  Ebenso widerlegt: „höhere Bitrate macht Encoding nicht schneller" — 10 Mbit/s
  ist schneller als 2 Mbit/s, statisch um 11 %, auf bewegtem Bild noch um 3 %.
  10 Mbit/s ist also nicht nur unkritisch (API bis 40 Mbit/s, Level 4.1 reicht),
  sondern der günstigere Betriebspunkt.
- Budget bei der Zielrate 30 fps (33,3 ms/Frame): 1792×448 kostet **21,3 ms
  → 64 %** aus PSRAM, **15,4 ms → 46 %** aus AXISRAM (M2-R, gemessen an der
  echten Geometrie). **Die Form des Frames kostet nichts**: 896×896 mit
  derselben Makroblockzahl liegt 0,7 % daneben.
  **4 Sensoren: Gesamtauflösung bleibt 1792×448 → VENC-Budget unverändert**;
  limitierend ist allein die Capture-Seite (verschachtelt, durch M0 bestätigt:
  Quellrate/4, also ~12,4 fps — die 30-fps-Vorgabe verfehlt das um Faktor 2,4
  und verlangt CrossLink-Variante C).
- HW-Handshake (Slice-Mode) ist mit VC-Zeitmultiplex inkompatibel (one-way
  `venc_rdy`, FUSE_ERROR, Errata ES0620 §2.2.14) → Frame-Mode.
- Bildqualität: Encoder-Pfad hat weder BLC noch Gain noch CCM → „washed out"
  (siehe csi-README); die drei DCMIPP-Blöcke brauchen keinen Sensor.
- Speicher: AXISRAM 3,4 MB (`.noncacheable` 2,77 MB), PSRAM 32 MB —
  **Linkerscript korrigiert** (deklarierte vorher 16 MB; die oberen 16 MB sind
  am Board verifiziert, Schreibzugriffe bei +16 MB und +32 MB−16 bleiben stehen
  und aliasen nicht auf 0x90000000). EWL-Pool 8 MB PSRAM.
  **Aufteilung nach M2 endgültig:** Composite-NV12 1792×448 = 1,20 MB im
  Ping-Pong (2,41 MB) *und* der Bitstream-Ring (427 KB) in AXISRAM, zusammen
  exakt die 2,77 MB. Der Ring muss dorthin, weil der **SDMMC-DMA nicht aus
  PSRAM lesen kann** (Transfers werden angenommen, aber ein Teil schliesst nie
  ab → 10-s-Timeout im FileX-Treiber); die Encode-Zeit hätte ihn erlaubt.
  Forum-Warnung: Ref-Frames in PSRAM → Timing-Artefakte — für einen Umzug ist
  jetzt kein AXISRAM mehr übrig.
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
- [x] **M1-R — derselbe Capture in der Breitformat-Geometrie** (2026-08-27,
  `mux 0 1 60`). Die Drehung macht aus jedem Segment ein **Fenster in einem
  breiteren Frame**: Pitch = 1792 gegen einen 896 Pixel breiten Capture, und
  Segment 1 beginnt 896 Bytes weiter rechts statt ein ganzes Segment weiter
  hinten. Genau diesen Fall hat die gestapelte Fassung nie ausgeübt, deshalb
  M1 noch einmal.

  **Die Adressen im Report zeigen die Geometrie direkt:**
  `crop=1920x960+0+60` → `segment=896x448` → `composite=1792x448`,
  `pitch=1792`, `composite_bytes=1204224`. Y-Basis 0x906bb800, Segment 1 bei
  0x906bbb80 — **Abstand 0x380 = 896 Bytes**, also eine Spaltenverschiebung und
  kein Segmentsprung. Chroma genauso: 0x9077f800 / 0x9077fb80, ebenfalls 896
  auseinander, und die Chroma-Ebene beginnt 0xC4000 = 1792·448 nach der
  Luma-Ebene. Das ist Zeile für Zeile das NV12-Layout, das der Encoder erwartet.

  **Ergebnis, gegen M1 gehalten:**

  | | M1 (448×1792, gestapelt) | M1-R (1792×448, nebeneinander) |
  |---|---|---|
  | VC0 / VC1 captured | 24,82 / 24,80 fps | **24,85 / 24,85 fps** |
  | Keep-Rate | 50,00 % / 49,96 % | **50,00 % / 50,00 %** |
  | `vc_mismatch` | 0 | **0** |
  | Pipe-Overruns | 0 | **0** |
  | ISR-Switch | 0,3 µs avg (max 0,9) | **0,3 µs avg (max 1,1)** |
  | EOF → Switch vollzogen | 5,7 µs avg (max 6,0) | **5,8 µs avg (max 6,0)** |
  | Luma-Mittel je Segment | 122 / 148 | **96 / 83** |

  Nichts bewegt sich. 2986 Pipe-Frames in 2986 Quell-Frameperioden über 60,06 s,
  `eof_unassociated=0`, `p1cfscr` = `p1fscr` = 0x2b. **Der Pixel-Packer schreibt
  in ein Fenster genauso zuverlässig wie in einen zusammenhängenden Block**, und
  die Switch-Mechanik interessiert das nicht.

  *Zum Luma-Mittel:* die beiden Werte sind unterschiedlich (96 gegen 83) und
  beide plausibel — zwei Kameras auf dieselbe Szene. Der Prüfer musste dafür
  umgebaut werden: er lief vorher gerade durch den Speicher, was nebeneinander
  liegende Segmente ineinander mitteln würde und ein nie beschriebenes Segment
  lebendig aussehen liesse. Jetzt tastet er zeilenweise innerhalb des Fensters
  ab. Dass zwei verschiedene Werte herauskommen, ist deshalb ein Beleg, dass
  die Segmente sich nicht überlappen.

- [x] **GATE (User-Entscheid): 24,8 fps/Kamera sind akzeptiert** (Patrick,
  2026-08-26). Die ursprüngliche Bedingung greift nicht: sie war an „M0/M1
  zeigen serielle Frames" geknüpft, die Quelle ist verschachtelt, und die
  Switch-Latenz (5,7 µs gegen 387,5 µs Gap) ist gerade nicht das Problem. Eine
  CrossLink-Anpassung auf 25 fps pro Sensor würde den Capture nicht drop-frei
  machen, sondern nur auf 12,4 fps pro Kamera halbieren — sie wäre hier
  kontraproduktiv.
  **Folge für den Rest des Plans:** Composite-Rate = 24,8 fps, das ist der
  günstige Ast des VENC-Budgets. Die vollen 49,6 fps blieben nur über
  CrossLink-Variante C (FPGA-Composite) erreichbar; das ist damit vertagt, nicht
  verworfen — spätestens bei 4 Sensoren (~12,4 fps/Kamera) kommt die Frage
  wieder.
  > **Überholt durch die korrigierte Zielvorgabe vom 2026-08-27.** Der Entscheid
  > galt für zwei Sensoren und eine 25-fps-Untergrenze; dort fehlten 0,2 fps.
  > Die neue Vorgabe lautet 4 Sensoren bei 30 fps, und dieselbe Mechanik liefert
  > dann 12,4 fps — kein Rundungsfehler mehr, sondern ein Faktor 2,4. Die
  > Zustimmung deckt den Endausbau also nicht ab; „bei 4 Sensoren kommt die
  > Frage wieder" ist eingetreten. Siehe Ausbaustufe: Variante C wird damit
  > Voraussetzung.
- [x] **M2 — VENC-Zeitmodell** (Encoder-Build, neues Kommando `bench [n]` plus
  `cfg`/`format`/`inbuf`/`cabac`/`t8x8`/`bitrate`, `Appli/Core/Src/venc_bench.c`):
  Baseline 720p YUYV (~27 ms), dann einzeln: (a) NV12
  (`DCMIPP_PIXEL_PACKER_FORMAT_YUV420_2` + `H264ENC_YUV420_SEMIPLANAR`),
  (b) `input_frame` in AXISRAM (Bitstream-Ring temporär verkleinert),
  (c) `enableCabac=2`, (d) `transform8x8Mode=0`, (e) Bitrate 2 vs. 10 Mbit/s.
  Gemessen wird ausschliesslich `H264EncStrmEncode()` auf Live-Kamerabildern
  (ein Standbild machte Inter-Frames unrealistisch billig), Ausgabe wird
  verworfen, damit die SD-Karte nicht mitmisst.
  Gemessen auf **zwei Szenen**: erst statisch (Raum, unbewegt), dann mit einem
  Bildschirm mit Bewegtbild vor der Kamera. Die erste Serie allein wäre
  irreführend gewesen — auf einem unbewegten Bild sind Inter-Frames billig, und
  die Zahlen sind dort eine Untergrenze.
  **Ergebnis (720p = 3600 MB, µs/MB; statisch je 200 Frames in zwei
  Durchläufen, bewegt 200 Frames; (b) 60 Frames, weil die Stage-Kopie den Lauf
  auf 5,5 fps drückt):**

  | Variante | statisch (P1 / P2) | bewegt | 3136 MBs, bewegt | max. Composite-Rate |
  |---|---|---|---|---|
  | Baseline YUYV, cabac=1, t8x8=1, 2 Mbit/s | 7,24 / 7,19 | 6,85 | 21,5 ms | 46,6 fps |
  | (a) NV12 | (7,59) / 7,07 | 6,83 | 21,4 ms | 46,7 fps |
  | (c) `enableCabac=2` | 7,24 / 7,20 | 6,87 | 21,5 ms | 46,4 fps |
  | (d) `transform8x8Mode=0` | 7,31 / 7,30 | 6,86 | 21,5 ms | 46,5 fps |
  | **(e) 10 Mbit/s — der Zielbetriebspunkt** | 6,45 / 6,42 | **6,64** | **20,8 ms** | **48,0 fps** |
  | (b) NV12 aus PSRAM (Kontrolle) | 6,99 | 6,76 | 21,2 ms | 47,2 fps |
  | (b) NV12 aus AXISRAM | **4,83** | **4,62** | **14,5 ms** | **69,0 fps** |

  Der statische P1-Wert von (a) ist eingeklammert: in diesem Lauf steckt ein
  einzelner 150-ms-Ausreisser (SD-Nachlauf), P2 ist der saubere.

  **Die bewegte Szene wirkt in beide Richtungen — das ist kein Widerspruch,
  sondern die Ratenregelung.** Bei 2 Mbit/s wird das Encoding *schneller*
  (7,2 → 6,85): eine schwerere Szene bekommt bei festem Bitbudget einen höheren
  QP, und ein höherer QP heisst weniger Koeffizienten. Bei 10 Mbit/s, wo das
  Budget grosszügig ist und der QP niedrig bleibt, wird es *langsamer*
  (6,42 → 6,64, +3,4 %). Nur der zweite Wert misst die Szene; der erste misst
  die Regelung.
  **Merke für jede weitere Messung:** Bits/Frame taugen hier nicht als
  Schwierigkeitsmass. Die Regelung trifft ihr Budget punktgenau — 10 Mbit/s ÷
  30 fps = 333 kbit/Frame, gemessen 334 — und zwar unabhängig davon, was vor
  der Kamera passiert, bis hin zur zugehaltenen Linse.

  Auf der bewegten Szene liegen Baseline, (a), (c) und (d) innerhalb von 0,6 %
  (6,83–6,87) — enger als auf der statischen. **Format, Entropiecoder und
  8×8-Transform kosten praktisch nichts**, und das gilt jetzt auch dort, wo es
  überhaupt Residuum zu codieren gibt. Nur zwei Dinge bewegen die Zeit
  wirklich:
  1. **Woher der Encoder das Bild liest.** Aus AXISRAM statt PSRAM sind es
     31 % weniger Encode-Zeit — statisch 6,99 → 4,83, bewegt 6,76 → 4,62 µs/MB,
     also −30,9 % bzw. −31,7 %. Der Effekt hängt nicht am Bildinhalt, was ihn
     erwarten liess: der Encoder ist beim Lesen der Quelle speicherlimitiert,
     nicht rechenlimitiert.
  2. **Die Bitrate — auf der statischen Szene mit umgekehrtem Vorzeichen als
     erwartet.** Statisch ist 10 Mbit/s 11 % *schneller* als 2 Mbit/s. Auf der
     bewegten Szene schrumpft der Vorsprung auf 3 % (6,64 vs. 6,85), weil dort
     beide Effekte gegeneinander laufen. In keinem Fall ist die hohe Bitrate
     teurer — die Recherche-Erwartung „höhere Bitrate kostet Zeit" ist damit
     auf beiden Szenen widerlegt.

  *Die Spalte „3136 MBs" ist eine Hochrechnung aus µs/MB. **M2-R misst sie
  direkt** — an der echten Geometrie und nach der Korrektur auf Breitformat;
  die Hochrechnung hält (21,3 statt 20,8 ms, Differenz szenenbedingt).*

  **Antwort auf die Planfrage:** das Composite passt bei 24,8 fps mit grossem
  Abstand. Budget 40,3 ms/Frame; am Zielbetriebspunkt (10 Mbit/s, bewegte
  Szene) sind es 20,8 ms → **52 % Auslastung**, über alle gemessenen Varianten
  und beide Szenen nie mehr als 22,9 ms → 57 %. Selbst 30 fps (33,3 ms) und
  44 fps wären drin; der Engpass bleibt die Capture-Seite aus M0/M1, nicht der
  Encoder.
  **Vorbehalt:** beide Szenen sind Bench-Szenen, keine Einsatzszenen. Ein
  detailreicheres Bild kostet mehr, und der Encoder-Pfad hat weder BLC noch
  Gain noch CCM (siehe Bildqualität oben), liefert also kontrastärmer als der
  Probe-Pfad. Der Sicherheitsabstand von 43 % trägt das, aber die Zahl ist
  szenenabhängig und nicht auf die dritte Stelle zu nehmen.
  **Bitstream-Ring: PSRAM genügt.** Gemessen mit identischem Ring (512 KB) an
  beiden Adressen, sonst gleicher Konfiguration, direkt hintereinander:

  | Ring | 2 Mbit/s | 10 Mbit/s |
  |---|---|---|
  | AXISRAM (`.noncacheable`) | 7,10 µs/MB | 6,33 µs/MB |
  | PSRAM | 7,11 µs/MB | 6,36 µs/MB |

  Unterschied 0,1 % bzw. 0,5 % — innerhalb der Laufstreuung. Der Encoder
  schreibt den Bitstream sequenziell und in kleinen Mengen (bei 10 Mbit/s rund
  42 KB je Frame gegen 1,2 MB, die er als Quelle *liest*); das Lesen ist der
  Flaschenhals, nicht das Schreiben. **Damit gehört der Ring nach PSRAM und
  die NOCACHE-Region ganz dem Composite-Buffer** — was die Ping-Pong-Frage
  erst lösbar macht. Presets `DebugSmallRing` / `DebugBitstreamPsram`.

  **…aber der Ring darf trotzdem nicht nach PSRAM — die SD-Karte verbietet es.**
  Der Encode-Zeit-Vergleich oben misst nur, wer den Ring *schreibt*. Wer ihn
  *liest*, ist der SDMMC-DMA, und der kommt mit der XSPI-gemappten PSRAM nicht
  zurecht. Gemessen mit `sdbench 200`, identischer 512-KB-Ring an beiden
  Adressen:

  | Ring | 2 Mbit/s | 10 Mbit/s | Blocktransfers |
  |---|---|---|---|
  | AXISRAM | 3,2 ms avg / 30,4 ms max | 5,5 ms avg / 31,5 ms max | 278/278 und 299/299 abgeschlossen |
  | PSRAM | **213 ms avg / 4,63 s max** | **23,2 ms avg / 626 ms max** | **88 von 91 abgeschlossen** |

  Die Zähler sagen, was passiert: `blk.rejected=0` — die HAL nimmt jeden
  Auftrag an — aber `blk.calls=91` gegen `blk.completions=88`. Drei Transfers
  melden nie Fertigstellung, der FileX-Treiber läuft in sein
  `FX_STM32_SD_DEFAULT_TIMEOUT` (10 s), und das erzeugt die
  Sekunden-Ausreisser bei einem Minimum von 431 µs. Im AXISRAM-Arm kommt jeder
  einzelne Transfer an, und `blk.from_psram=0`.
  FileX übergibt den Nutzpuffer teilweise direkt an den Treiber
  (`blk.from_psram=65` von 91) und teilweise seinen eigenen Medienpuffer
  (`from_axisram=26`) — nur die direkten Transfers aus PSRAM hängen.
  **Damit ist die Ringplatzierung entschieden und zwar nicht von der
  Encode-Zeit:** der Bitstream-Ring bleibt in AXISRAM, weil der SD-Pfad nicht
  aus PSRAM heraus DMA-en kann.
  *Ausweg, falls der Platz später doch gebraucht wird:* FileX zwingen, immer
  über seinen Medienpuffer zu gehen. Das kostet eine Kopie von ~50 KB je Frame
  aus PSRAM (bei den gemessenen 9,4 MB/s rund 5 ms) und gibt 1,6 MB frei. Nicht
  umgesetzt, nur notiert.

  **Speicherbudget der NOCACHE-Region (2769K = 2 835 456 B):**
  (Die Bytezahlen sind von der Drehung auf Breitformat unberührt — 448×1792 und
  1792×448 belegen dasselbe.)

  | Belegung | Bedarf | passt |
  |---|---|---|
  | Composite NV12 1792×448, einfach | 1 204 224 B | ja, 1,55 MB frei |
  | Composite NV12, **Ping-Pong (2×)** | 2 408 448 B | **ja**, 427 KB frei |
  | Composite YUYV 1792×448, einfach | 1 605 632 B | ja, 1,17 MB frei |
  | Composite YUYV, Ping-Pong (2×) | 3 211 264 B | **nein**, 376 KB zu viel |

  Das entscheidet die Formatfrage: **NV12, weil nur damit Ping-Pong in AXISRAM
  passt.** Als Zeitargument taugt das Format nicht (0,6 % Unterschied), als
  Platzargument schon.

  **Die 427 KB, die dabei frei bleiben, sind genau der Bitstream-Ring** — und
  der muss dort liegen, siehe oben. Bei 10 Mbit/s und 24,8 fps sind das rund
  50 KB je Frame, der Ring fasst mit `VENC_OUTPUT_BLOCK_NBR = 4` also vier
  Frames Polster gegen einen SD-Stall. Der längste gemessene Schreibvorgang ist
  31,5 ms, knapp eine Frameperiode — das Polster reicht mit Reserve. Die
  NOCACHE-Region geht damit **exakt auf**: 2 408 448 + 427 008 = 2 835 456 B.
  Kein Platz mehr für den EWL-Ref-Frame-Umzug, der in Phase 1 als Option steht;
  der bleibt in PSRAM.

  **Finale Buffer-Platzierung:** Composite-Buffer nach AXISRAM. Er misst
  448·1792·1,5 = 1,20 MB und passt damit in die NOCACHE-Region (2,77 MB) neben
  einen verkleinerten Bitstream-Ring. Das ist keine Notwendigkeit mehr (PSRAM
  reicht rechnerisch), aber 7,5 ms Reserve je Frame, die nichts kosten.
  **Nicht per CPU-Kopie:** die DCMIPP muss direkt dorthin schreiben. Die in (b)
  gemessene Stage-Kopie kostet **146,5 ms je Frame** (1,38 MB → 9,4 MB/s), weil
  beide Regionen uncached sind — sie war nur das Messmittel, nie der Vorschlag.
- [x] **M3 — Störpakete**: 0x12/0x2f-Pakete und permanenter IDERR korrumpieren
  den Pipe1-Semiplanar-Capture nicht (im Preview bekannt harmlos; einmal im
  Encoder-Setup verifizieren). Neues Kommando `errors [s]`
  (`VENC_APP_WatchErrors`): zählt über ein Fenster, was CSI-Empfänger, DCMIPP
  und Encoder melden, während die Pipeline aufzeichnet.
  **Ergebnis (je 120 s, NV12, Aufzeichnung auf SD, bei 2 und 10 Mbit/s):
  bestätigt harmlos.**

  | | 2 Mbit/s | 10 Mbit/s |
  |---|---|---|
  | `csi.id_err` | 11 847 von 11 848 Stichproben | 11 529 von 11 533 |
  | `csi.ecc` / `crc` / `sync` / `spkt` / `watchdog` / `phy` | **je 0** | **je 0** |
  | `dcmipp.pipe_errors` / `global_errors` | **0 / 0** | **0 / 0** |
  | `fuse_errors` | **0** | **0** |

  Der IDERR steht permanent an — er ist kein Ereignis, sondern ein Zustand —
  und darunter bleibt über zwei Minuten alles still: kein Pipe-Overrun, kein
  AXI-Fehler, keine DCMIPP/VENC-Desynchronisation. Die Störpakete beansprucht
  weiterhin niemand, und der Semiplanar-Capture nimmt keinen Schaden.

  **Was der Report sonst noch zeigt und was es nicht ist:** `ring_full` = 278
  bzw. 1413, und in beiden Läufen ist `encode_errors` exakt dieselbe Zahl — das
  heisst, *jeder* gezählte Encode-Fehler war ein voller Ausgangsring, keiner ein
  Encoder-Fehler. Der SD-Pfad kommt bei den heutigen 49,7 fps aus einem
  einzelnen VC nicht nach, bei 10 Mbit/s deutlich weniger als bei 2. Das ist
  eine Durchsatzfrage, keine Korruption, und der Zielzustand entlastet sie: das
  Composite läuft mit 24,8 fps, also rund 60 % der heutigen SD-Last
  (1,24 statt 2,07 MB/s). Der SD-Pfad ist damit aber der nächste Kandidat für
  den Engpass — er sollte in Phase 3 im Dauerlauf nachgemessen werden, nicht
  angenommen.
  Der Rest der Differenz zwischen `frames_received` (5961) und
  `frames_encoded` (4416) sind Frames, die der Encoder nie gesehen hat: er
  läuft bei 2 Mbit/s mit 36,8 fps an seiner Decke, und das Event-Flag der
  Capture koaleszt, was in der Zwischenzeit ankommt. Auch das ist strukturell,
  kein Fehler.

- [x] **M2-R — Zeitmodell an der korrigierten Geometrie** (2026-08-27). Die
  Zielvorgabe wurde auf Breitformat und 30 fps korrigiert; M2 hatte die
  Composite-Zeit aus 720p über die Makroblockzahl hochgerechnet, was
  stillschweigend annimmt, dass die *Form* des Frames nichts kostet. Genau das
  war zu prüfen. Die Geometrie ist dafür ein Build-Parameter geworden
  (`-DVENC_GEOMETRY=1792x448`, Presets `Geom*`), der Encoder wird also
  tatsächlich auf die Zielgrösse konfiguriert und die DCMIPP skaliert die
  Quelle hinein — gemessen statt gerechnet.

  Alle Läufe direkt hintereinander gegen dieselbe Szene, NV12, `cabac=1`,
  `t8x8=1`, **10 Mbit/s** (der Zielbetriebspunkt), Ausgabe verworfen; 720p an
  beiden Enden als Klammer.

  | Geometrie | MBs | aus PSRAM | ms/Frame | aus AXISRAM | ms/Frame |
  |---|---|---|---|---|---|
  | 1280×720 (Referenz, Klammer) | 3600 | 6,97 µs/MB | 25,11 | 4,89 µs/MB | 17,62 |
  | **1792×448 — die Zielgeometrie** | 3136 | **6,80 µs/MB** | **21,33** | **4,90 µs/MB** | **15,37** |
  | 896×896 (Formkontrolle, gleiche MBs) | 3136 | 6,85 µs/MB | 21,48 | — | — |
  | 1920×512 (≙ 1984×496, s. u.) | 3840 | 6,94 µs/MB | 26,69 | 4,90 µs/MB | 18,82 |

  **1. Die Form kostet nichts.** 1792×448 und 896×896 haben dieselben 3136
  Makroblöcke und liegen 0,7 % auseinander — das ist Laufstreuung. Die Drehung
  von hochkant auf breit war damit für den Encoder ein Nicht-Ereignis, und die
  Hochrechnung aus M2 (20,8 ms) trifft die jetzt direkt gemessenen 21,3 ms auf
  2 % genau; der Rest ist die Szene.

  **2. Die Makroblockzahl trägt das Modell.** Aus AXISRAM sind es über alle
  drei Geometrien 4,89 / 4,90 / 4,90 µs/MB — auf drei Stellen unbewegt. Aus
  PSRAM streut es leicht (6,80–6,97), weil dort die Speicheranbindung
  mitspricht und nicht nur die Rechenarbeit.
  Die Klammer trägt das: 720p misst am Anfang der Serie 6,97 µs/MB und am Ende
  6,98 — 0,14 % Drift über alle acht Läufe. Was zwischen den Geometrien steht,
  ist also die Geometrie und nicht die Szene.

  **3. Das Budget bei 30 fps (33,3 ms/Frame):**

  | Composite | aus PSRAM | aus AXISRAM |
  |---|---|---|
  | 1792×448 (4× 448×448) | 21,3 ms → **64 %** | 15,4 ms → **46 %** |
  | 1984×496 (4× 496×496) | 26,7 ms → **80 %** | 18,8 ms → **57 %** |

  Beides passt, auch aus PSRAM. Der Vorbehalt aus M2 gilt unverändert: eine
  detailreichere Einsatzszene kostet mehr, und bei 80 % ist der Puffer schon
  dünn — 496 sollte deshalb nicht aus PSRAM encodiert werden.

  **Warum 1920×512 und nicht 1984×496:** die DCMIPP kann nur verkleinern, und
  die Quelle ist 1920 breit — 1984 ist von hier aus nicht herstellbar.
  1920×512 = 3840 Makroblöcke liegt 0,1 % neben den 3844 von 1984×496, ist also
  ein genauer Stellvertreter für das, was zählt.

- [x] **M2-R Speicher — 448 ist die Obergrenze, 496 nur mit Umbau.** Die
  Encode-Zeit erlaubt 496×496; der Speicher tut es nicht. NOCACHE-Region =
  2 835 456 B, Composite-NV12 = Breite·Höhe·1,5, Ping-Pong = zweimal das:

  | Segment (4 Sensoren) | Composite | MBs | NV12/Frame | Ping-Pong | + Ring 427 KB | passt |
  |---|---|---|---|---|---|---|
  | **448×448** | 1792×448 | 3136 | 1 204 224 B | 2 408 448 B | **2 835 456 B** | **ja, auf das Byte genau** |
  | 480×480 | 1920×480 | 3600 | 1 382 400 B | 2 764 800 B | 3 191 808 B | nein (Ring passt nicht mehr) |
  | 496×496 | 1984×496 | 3844 | 1 476 096 B | 2 952 192 B | 3 379 200 B | **nein** (schon der Ping-Pong ist 114 KB zu gross) |
  | 512×512 | 2048×512 | 4096 | 1 572 864 B | 3 145 728 B | 3 572 736 B | nein |

  Dass 448 exakt aufgeht, ist kein Zufall der Rundung, sondern die Grenze:
  aus 12·N² + Ring ≤ 2 835 456 folgt N ≤ 448,0 bei heutigem Ring und N ≤ 486
  selbst dann, wenn der Ring ganz verschwände — und verschwinden darf er nicht,
  der SDMMC-DMA liest nicht aus PSRAM (siehe oben).

  **Der Umbau, der 496 möglich machen würde.** Die NOCACHE-Region ist nicht
  alles, was in AXISRAM liegt; davor stehen RO_Region und RW_Region, und beide
  sind nur teilweise belegt (Debug-Build, aus dem Mapfile):

  | Region | Länge | belegt | frei |
  |---|---|---|---|
  | RO_Region | 512 000 B | 309 696 B | 202 304 B |
  | RW_Region | 583 680 B | 253 216 B | 330 464 B |
  | **zusammen frei** | | | **532 768 B** |

  Wandert dieser Platz ins Linkerscript-Segment NOCACHE, sind dort **3 368 224 B**
  möglich. Damit passt 496×496 (2 952 192 Ping-Pong + 416 032 Ring — knapp unter
  den heutigen 427 KB, also rund acht Frames Polster). **512×512 bliebe auch
  dann aus:** der Ping-Pong liesse nur noch 222 KB Ring übrig, und der Rest des
  Bilds hätte keinerlei Reserve mehr.

  **Der Preis ist real:** danach hat weder der Code noch der Heap in AXISRAM
  noch Luft, und die 532 KB sind eine Momentaufnahme dieses Builds — jede
  Codezeile mehr knabbert daran. **Empfehlung: bei 448×448 bleiben**, solange
  nicht jemand die 10 % mehr Kantenlänge ausdrücklich braucht. 448×448 passt
  ohne jeden Eingriff, lässt 46 % VENC-Budget übrig und ist gegenüber dem
  Endausbau ohnehin nicht das, was die Auflösung begrenzt — das ist die
  Capture-Rate.

  *Am Rand:* die SD-Last ändert sich durch die Auflösung nicht. Die Ratenregelung
  arbeitet in Bit pro Sekunde, nicht pro Pixel — 10 Mbit/s bleiben 1,25 MB/s,
  ob 448 oder 496. Wer bei 496 dieselbe Bildqualität je Pixel will, muss die
  Bitrate um 23 % anheben (auf ~12,3 Mbit/s, 1,54 MB/s); der SD-Pfad trägt das
  (gemessen 5,5 ms für ~50 KB aus AXISRAM).

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

Derselbe Lauf nach der Drehung auf Breitformat (M1-R, 2026-08-27, gleicher
Build-Preset `CsiProbe`, gleiche Kommandofolge):

```
=== MUX RESULT ===
cpu_hz=800000000  window_ms=60064
source=1920x1080  crop=1920x960+0+60  segment=896x448  composite=1792x448
format=NV12 pitch=1792  composite_base=0x906bb800  composite_bytes=1204224
seg0.vc=0  y=0x906bb800  uv=0x9077f800   seg1.vc=1  y=0x906bbb80  uv=0x9077fb80
src.vc0.frames=2986  cap.vc0.frames=1493  drops=1493  keep=50.00 %  luma_mean=96
src.vc1.frames=2986  cap.vc1.frames=1493  drops=1493  keep=50.00 %  luma_mean=83
src.fps=49.71 je Kanal        cap.fps=24.85 / 24.85
captured_total=2986           vc_mismatch=0      eof_unassociated=0
isr_switch_us     min 0.3  avg 0.3  max 1.1   (n=2986)
eof_to_switch_us  min 5.4  avg 5.8  max 6.0   (n=2986)
p1fscr=0x0000002b  p1cfscr=0x0000002b
dcmipp_error=0x00000900  csi_sr0=0x08183300
=== END ===
```

Der Unterschied steht in den Adressen: die Segmente liegen jetzt **896 Bytes**
auseinander statt ein ganzes Segment (0x62000), in Luma wie in Chroma, und der
Pitch trägt die Composite-Breite. Alles andere — Raten, Drops, Switch-Latenz,
`vc_mismatch`, `dcmipp_error` — ist innerhalb der Streuung identisch.

Nicht gemacht: die im Plan zusätzlich vorgesehenen TraceX-Events. Die
ISR-Latenzen stehen als min/avg/max im Konsolen-Report, dafür braucht es kein
Perfetto; neue Instr-IDs wären erst nützlich, wenn Capture und Encoder im selben
Trace korreliert werden müssen (M2/Phase 2).

### Rohdaten M2 (2026-08-26/27, Encoder-Build)

Der Encoder-Build kann alle M2-Varianten zur Laufzeit umstellen, ausser (b):
die Stage-Puffer in AXISRAM gibt es nur im Preset `DebugAxiInput`
(`-DVENC_M2_AXISRAM_INPUT=ON`, verkleinert nebenbei den Bitstream-Ring auf
1 MB). `bench` startet die Pipeline selbst, wenn sie steht, und stoppt sie
danach wieder — die Konsole pollt die UART byteweise ohne FIFO, und solange (b)
läuft, verliert sie jedes Kommando. Ein Board, das in dieser Konfiguration
weiterläuft, nimmt keinen Befehl mehr an; erst diese Symmetrie machte die
Messreihe automatisierbar.

```powershell
cmake --build --preset Debug            # bzw. DebugAxiInput für (b)
./Tools/debug/stm32n6-gdb.ps1 -FlashAppli -Preset Debug
C:\repositories\dutpower\.venv\Scripts\dutpower.exe cycle
./Tools/debug/csi-console.ps1 -Sequence "stop; format nv12; cabac 1; t8x8 1; bitrate 2000000; bench 200"
```

```
=== VENC RESULT ===
cpu_hz=800000000  width=1280  height=720  macroblocks=3600
pixel_format=NV12       input_src=axisram
cabac=1  transform8x8=1  bitrate=2000000  gop=30  framerate_cfg=30
window_ms=10875         frames=60
intra.n=2   intra.us_min=13828  intra.us_avg=13849  intra.us_max=13870
inter.n=59  inter.us_min=16960  inter.us_avg=17513  inter.us_max=17867
stage_copy.n=61  stage_copy.us_min=146364  stage_copy.us_avg=146540  stage_copy.us_max=146711
all.us_avg=17391        all.us_per_mb_x100=483   inter.us_per_mb_x100=486
max_fps_x100=5750       stream_kbytes=510        stream_kbit_per_s=384
composite_mbs=3136      composite_us=15146       composite_max_fps_x100=6602
=== END ===
```

**Für M2-R kommt die Geometrie dazu** (`-DVENC_GEOMETRY=WIDTHxHEIGHT`, Presets
`Geom1792x448`, `Geom896x896`, `Geom1920x512` und die `…Axi`-Varianten mit
AXISRAM-Input). Der Encoder wird damit auf die Zielgrösse konfiguriert und die
DCMIPP skaliert die 1920×1080-Quelle hinein; grösser als die Quelle geht nicht,
darum 1920×512 als Stellvertreter für 1984×496. Das Skript, das die Matrix
abfährt, steht im Scratchpad (`m2r.ps1`) und flasht je Variante neu.

Zwei Dinge, an denen die Automatisierung sonst scheitert:

- **Die Quelle braucht nach jedem Board-Neustart einen Power-Cycle.** Ohne
  `dutpower cycle` nach dem Flashen meldet jeder Lauf `frames=0` und
  `CSI SR0=0x00060000` — das sieht nach einem Geometrieproblem aus und ist
  keines; der CrossLink zieht den Link von sich aus nicht wieder hoch.
- **`-IdleMs` funktioniert nicht mehr**, seit die Firmware sekündlich
  `CPU Usage: …` schreibt: das Board wird nie still, jedes Kommando läuft in
  `-MaxSeconds`. Stattdessen `-WaitFor` benutzen — `'VENC: \d+x\d+ .*bit/s'`
  für die Konfigurationskommandos, `'=== END ==='` für `bench`.
- **`stm32n6-gdb.ps1` über `cmd` aufrufen**, wenn ein PowerShell-Skript es
  treibt. GDB schreibt auch im Erfolgsfall auf stderr, und PowerShell 5.1 macht
  daraus einen abbrechenden `NativeCommandError`, der den Rest des Skripts
  verschluckt.

Zum Vergleich derselbe Lauf mit `inbuf capture`: `all.us_avg=25166`,
`all.us_per_mb_x100=699`, `composite_max_fps_x100=4562`. Ein zweiter Durchlauf
reproduziert beide Seiten auf drei Stellen (25171 / 17396 µs, Stage-Kopie
146551 µs), und auf der bewegten Szene bleibt der Abstand gleich (24370 →
16640 µs, −31,7 %) bei unveränderter Stage-Kopie (146529 µs — sie ist ein
reiner memcpy und hängt nicht am Bildinhalt). Gleiche Bits je Frame
(8,1 vs. 8,5 KB), also ein fairer Vergleich; und weil zwischen zwei Frames im
AXISRAM-Lauf 164 ms statt 26 ms liegen, ist die Szene dort *stärker* verändert
— der 31-%-Vorteil ist eher konservativ als geschönt.

`stream_kbit_per_s` ist im (b)-Lauf niedrig (384), weil das Fenster die
Stage-Kopien enthält; die Ratenregelung ist auf 30 fps konfiguriert, real kamen
5,5 fps an. Für das Zeitmodell irrelevant, `us_per_mb` misst nur den Encode.

M3 wird mit `errors [s]` gemessen, das die Pipeline laufend voraussetzt:

```powershell
./Tools/debug/csi-console.ps1 -Sequence "stop; format nv12; bitrate 10000000; record; start"
./Tools/debug/csi-console.ps1 -Sequence "errors 120" -MaxSeconds 180
```

Die Zähler `csi.*` sind Polling-Stichproben, keine Pakete — nur ihre Anwesenheit
und ihr Verhältnis zur Fensterlänge bedeuten etwas. SOF/EOF werden bewusst nicht
gelöscht: die laufende Pipe hat ihren Interrupt-Handler auf denselben Flags, und
ein Löschen von hier würde ihr Frames stehlen.

**Die bewegte Szene** war ein Bildschirm mit laufendem Video vor der Kamera.
Das ist der brauchbarere Aufbau: er bewegt sich gleichmässig und von selbst,
also kann die ganze Matrix am Stück laufen, ohne dass jemand daneben stehen und
etwas hin und her schieben muss. Ein Mensch, der vor einer 1920×1080-Kamera
herumfuchtelt, bewegt zu wenig Bildfläche, um in der Encode-Zeit aufzutauchen —
der erste Anlauf dieser Messreihe ist genau daran gescheitert.

Zwei Sackgassen auf dem Weg dorthin, damit sie niemand noch einmal geht:
**Bits/Frame als Mass für die Schwierigkeit der Szene** funktioniert nicht (die
Ratenregelung pinnt sie, siehe M2 oben), und zwar auch nicht bei hoher Bitrate.
**Den Capture-Puffer per GDB auszulesen**, um zu sehen was die Kamera liefert,
funktioniert ebenfalls nicht: `monitor halt` reisst den CSI-Link mit, und was
danach im Puffer steht, ist ein eingefrorenes Standbild von unbestimmtem Alter.
Wer wissen will, was die Kamera sieht, nimmt den CsiProbe-Build und `grab`.

Zwei Firmware-Fehler sind bei M2 aufgefallen und behoben, beide unabhängig vom
Messziel:

- **EWL-Chunk-Tabelle wächst monoton.** `EWLFreeLinear()` gibt den Speicher
  zurück, lässt den Eintrag aber in `chunks[]` stehen und senkt `totalChunks`
  nie; `EWLInit()`/`EWLRelease()` auch nicht. Nach dem vierten
  Encoder-Neu-Init schreibt die Allokation über `chunks[MEM_CHUNKS]` hinaus →
  HardFault in `EWLMallocLinear()`. Behoben mit einem starken Override in
  `venc_h264_config.c`, das die Lücke schliesst (die Funktion ist `__weak`,
  Middleware bleibt unangetastet). Zehn Format-Wechsel hintereinander laufen
  jetzt durch.
- **Pipe-State bleibt nach hartem Stopp auf BUSY.** Wenn
  `HAL_DCMIPP_CSI_PIPE_Stop()` in den Timeout läuft, scheitert danach jedes
  `HAL_DCMIPP_PIPE_SetConfig()` auf Pipe 1 — ein rauher Stopp machte jeden
  weiteren Start unmöglich („DCMIPP pixel packer reconfiguration failed").
  Derselbe Handgriff wie in `csi_grab.c`: VC stoppen, State zurücksetzen.

## Phase 1 — Composite-Capture produktiv

Dateien: `Appli/Core/Src/dcmipp_app.c`, `venc_app.c`, `venc_h264_config.*`
(Geometrie über `-DVENC_GEOMETRY=1792x448`, seit M2-R ein Build-Parameter;
**Segmentanzahl parametrisiert**, damit die 4-VC-Stufe nur die Segmentliste
ändert).

> Bei CrossLink-Variante C entfällt dieser ganze Phasenblock: der FPGA liefert
> das fertige Composite auf einem VC, Pipe1 nimmt jedes Frame, und Phase 2/3
> bleiben unverändert. Nach der korrigierten Zielvorgabe ist C für den
> Endausbau ohnehin Voraussetzung — Phase 1 ist damit der Weg für den
> Zweisensor-Aufbau auf dem Tisch, nicht für das Produkt.

- [ ] VC-Switch + Adress-/Pitch-Konfiguration im Frame-Complete-ISR (beide VCs
  identische Geometrie → nur VC-Feld + Semiplanar-Adressen wechseln, keine
  weitere Pipe-Reconfig). **Pitch = Composite-Breite (1792), nicht
  Segmentbreite** — die Segmente liegen nebeneinander; Segment k beginnt
  k·SEG_W Bytes weiter rechts, in beiden Ebenen. Mechanik in M1-R belegt.
- [ ] Pairing-/Overflow-Logik: Composite komplett = alle Segmente desselben
  Ping-Pong-Slots gefüllt → `FRAME_RECEIVED_FLAG`; fehlt ein Segment, Inhalt
  des Vorgängers stehen lassen und Zähler tracen. `frame_received`/
  `GetNextFrame` auf Composite-Slots umstellen.
- [ ] LCD-Preview (Pipe2 shared) zeigt alternierende Segmente → deaktivieren
  oder als Debug-Ansicht dokumentieren.

## Phase 2 — Encoder 1792×448 NV12 @ 10 Mbit/s

- [ ] `H264EncConfig`: 1792×448, `frameRate` = Ziel-Composite-Rate (30 fps;
  Variante A auf dem Tisch liefert 24,8 fps bei zwei Sensoren),
  Level 4.1, NV12-Preproc; Rate-Ctrl: `bitPerSecond=10_000_000`, `hrdCpbSize`
  anpassen, `gopLen` ≈ Framerate; Coding nach M2: `enableCabac` und
  `transform8x8Mode` bleiben auf den Defaults (1 / 1) — M2 hat gezeigt, dass
  beide die Zeit nicht messbar bewegen, also entscheidet die Qualität.
- [ ] Buffer-Layout nach M2, die NOCACHE-Region geht exakt auf:
  **Composite-NV12 im Ping-Pong (2×1,20 MB = 2 408 448 B)** — die DCMIPP
  schreibt direkt dorthin, keine CPU-Kopie — **plus Bitstream-Ring
  (427 008 B)**, der dort bleiben *muss*, weil der SDMMC-DMA nicht aus PSRAM
  liest. `VENC_OUTPUT_BUFFER_SIZE` entsprechend setzen, `VENC_OUTPUT_BLOCK_NBR`
  bei 4 lassen (≈106 KB je Block gegen ~50 KB je Frame). EWL-Ref-Frames und
  EWL-Pool bleiben in PSRAM — dort ist nach dem Linkerscript-Fix Platz, in
  AXISRAM nicht mehr. Linkerscript-Fix PSRAM 16→32 MB: **erledigt**.
- [ ] Verifikation: Encode-Zeit-Trace; `H264ENC_FUSE_ERROR`-frei ≥10 min;
  ffprobe/ffplay: 1792×448, Ziel-fps, ~10 Mbit/s.

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
| A. N6-VC-Zeitmultiplex (dieser Plan) | **gemessen 24,8 fps/Kamera** bei 2 Sensoren (M0/M1), kein FPGA-Build | FW-Arbeit hier; Switch-Mechanik bewiesen und vermessen. **Erreicht die neue Zielvorgabe nicht** (siehe unten) |
| B. FPGA Frame-Interleave auf 1 VC | entlastet N6 (kein VC-Switch), gleicher Durchsatz wie A | ~1 Woche FPGA, kein Durchsatzgewinn → lohnt allein kaum |
| C. FPGA Composite (Segmente in einem Frame) | volle Sensorrate, perfekte Paarung, drop-frei; **die einzige Variante, die 4 Sensoren × 30 fps trägt** | setzt Sensor-Genlock voraus (IMX258 ohne HW-Trigger → gemeinsamer Takt + gleichzeitiger I²C-Start vom CrossLink; Machbarkeit 1–2 Tage abklären). Ohne Genlock bräuchte der FPGA Full-Frame-Puffer > CrossLink-EBR — bei 448×448-Segmenten allerdings nur noch ~300 KB je Kanal statt 2,6 MB, was die Frage neu stellt. Implementierung grob 1–3 Wochen inkl. HW-Iterationen |

### Warum Variante C jetzt Voraussetzung ist, nicht Ausbaustufe

Pipe1 ist der einzige Farbpfad und kann zu jedem Zeitpunkt genau einen VC
aufnehmen. M0 hat gemessen, dass die Quelle die Kanäle **paketweise
verschachtelt** überträgt — beide Frames laufen fast vollständig gleichzeitig
über die Leitung. Während Pipe1 einen Kanal einfängt, ist der andere also nicht
etwa noch nicht da, sondern bereits vorbei. Daraus folgt hart:

> Composite-Rate = Quellrate ÷ Anzahl Sensoren.

Bei 49,64 fps Quellrate:

| Sensoren | Composite-Rate (Variante A) | Ziel 30 fps | nötige Quellrate für 30 fps |
|---|---|---|---|
| 2 | 24,8 fps (gemessen, M1) | verfehlt | 60 fps/Kanal |
| 4 | **12,4 fps** | **um 59 % verfehlt** | **120 fps/Kanal** |

Der VENC hat für 30 fps Luft (M2-R: 64 % Auslastung), der Speicher passt, die
SD-Karte trägt es — **nur die Capture-Seite nicht**, und dort hilft keine
Firmware-Arbeit: die Grenze steckt in der Quelle. Zwei Auswege, beide ausserhalb
dieses Branches:

1. **Quellrate anheben** auf 120 fps/Kanal. Bei 448×448-Segmenten ist das
   datenratenseitig unkritisch (4 × 448×448 RAW10 × 120 fps ≈ 0,96 Gbit/s gegen
   5 Gbit/s Link), hängt aber daran, ob IMX258 und CrossLink so konfigurierbar
   sind. Offene Frage an den FPGA-/Sensor-Verantwortlichen.
2. **Variante C.** Der FPGA setzt das Composite selbst zusammen und schickt es
   als einen VC; Pipe1 nimmt dann jedes Frame und die Composite-Rate ist die
   Sensorrate. 30 fps verlangt dann nur noch 30 fps aus den Sensoren.

Die N6-Firmware wird so gebaut, dass die Capture-Seite austauschbar ist: der
Composite-Buffer ist identisch, egal ob per VC-Multiplex oder später per
FPGA-Composite befüllt. Bei Variante C entfällt lediglich der VC-Switch aus
Phase 1 — das Buffer-Layout, der Encoder und der SD-Pfad aus Phase 2/3 bleiben
Zeile für Zeile dieselben. Sensor-Sync ist zugleich Voraussetzung für das
spätere Ziel „Input-Rate = Encode-Rate ohne Drop".

### Trade-off der Quell-Architekturen → `FPGA-Anforderungen.md`

Die Frage „welche Architektur ist das Optimum" ist gegen die Messungen
durchgerechnet; das Ergebnis steht als eigenständiges Übergabedokument in
**[`FPGA-Anforderungen.md`](FPGA-Anforderungen.md)** (Varianten, Begründung,
nummerierte Anforderungen C-1…C-16, Rückfragen). Kurzfassung:

| | Was die Quelle sendet | Composite | Versatz Seg. 0→3 | Link-Last | N6-Firmware |
|---|---|---|---|---|---|
| A (Ist) | 4 VCs, 1920×1080, verschachtelt | 12,4 fps ✗ | 60 ms | 82 % | VC-Switch ×4 |
| A′ | 4 VCs, 448×448 @ 120 fps | 30 fps | 25 ms | 19 % | VC-Switch ×4 |
| B | 1 VC, 4 Frames à 448×448 nacheinander | 30 fps | 25 ms | 19 % | nur Adress-Flip |
| **C** | **1 VC, ein Frame 1792×448 @ 30 fps** | **30 fps** | **0** | **4,8 %** | **nichts** |

C gewinnt in jeder Spalte gleichzeitig, und die Umspezifikation auf Breitformat
ist genau der Grund, warum es jetzt auch auf der FPGA-Seite die billigste
Variante ist: nebeneinander genügen **Zeilenpuffer (~4,5 KB)**, übereinander
hätte es **Vollbildpuffer (~735 KB)** gebraucht — die Segmente der Kanäle 1–3
müssen dort ein ganzes Frame lang warten, bis sie an die Reihe kommen. Faktor
170, allein aus der Drehung.

Zwei Nebenbefunde aus derselben Analyse:

- **Die zweite Pipe hilft nicht.** Pipe1 und Pipe2 mit `PIPEDIFF=1` auf
  verschiedene VCs zu legen scheitert daran, dass Pipe2 weder Demosaic noch
  Semi-Planar-Ausgabe hat (im HAL nachprüfbar: beide Funktionen nehmen nur
  `DCMIPP_PIPE1`). Zwei Segmente in YUYV und zwei in NV12 ergeben keinen
  Encoder-Frame; ein reiner YUYV-Composite passt mit 2 × 1,60 MB nicht mehr in
  die 2,77 MB AXISRAM und bräuchte trotzdem 60 fps aus den Sensoren.
- **Der Empfänger kann nur 2 Lanes** (`IS_DCMIPP_NUMBER_OF_LANES`). Bei
  241 Mbit/s Nutzdaten für Variante C ist das reichlich — aber ein 4-Lane-Entwurf
  auf der Quellseite wäre nicht anschliessbar, und das gehört in die
  Anforderungen, bevor jemand so plant.

## Verifikation (gesamt)

1. M0–M3- sowie M1-R-/M2-R-Reports/Traces (Konsole + Perfetto) hier in
   PLAN.md eintragen.
2. Phase 1: Zählerkonsistenz VC0/VC1/Composite über ≥60 s, keine unerklärten
   Drops. Zusätzlich seit der Drehung auf Breitformat: **die Segmente dürfen
   sich im Bild nicht überlappen** — Pitch und Spalten-Offset sind die einzige
   Stelle, an der ein Fehler ein plausibel aussehendes, aber falsches Bild
   erzeugt (ein zu kleiner Pitch schreibt schräg, ein falscher Offset
   überschreibt den Nachbarn).
3. Phase 2/3: 10-min-Aufnahme; ffprobe (**1792×448**, Ziel-fps, Bitrate);
   visuelle Prüfung aller Segmente (Objektiv abdecken → nur ein Segment
   dunkel).
4. Regressionscheck: Single-VC-720p-Build (Preset `Debug`, ohne
   `VENC_GEOMETRY`) baut und läuft.
