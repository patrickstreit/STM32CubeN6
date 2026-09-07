# Anforderungen an die CSI-2-Quelle (CrossLink) — Übergabe an den FPGA-Entwickler

Stand 2026-08-27. Grundlage: die Messungen M0–M3, M1-R und M2-R auf dem
STM32N6570-DK (Details und Rohdaten in `PLAN.md`). Dieses Dokument ist
selbsttragend: es nennt zuerst die harte Randbedingung, dann den
Architekturvergleich, dann die Anforderungen, die sich aus der empfohlenen
Variante ergeben.

Zielbild des Produkts: **4 Sensoren à mindestens 448×448 bei 30 fps**, als ein
H.264-Stream (Composite **1792×448**, Segmente nebeneinander) auf SD-Karte.

## 1. Die harte Randbedingung

Der STM32N6 hat im DCMIPP **genau einen Farbpfad**. Nur Pipe1 besitzt den ISP
(Demosaic, Schwarzwert, Gain, CCM, YUV-Wandlung); Pipe2 ohne ISP liefert bei
`PIPEDIFF=1` nur Rohbayer und kann ausserdem kein Semi-Planar-NV12 schreiben,
Pipe0 ist eine reine Dump-Pipe. Das ist im HAL-Code nachprüfbar
(`HAL_DCMIPP_PIPE_SetISPRawBayer2RGBConfig` und `..._SetYUVConversionConfig`
akzeptieren ausschliesslich `DCMIPP_PIPE1`; `HAL_DCMIPP_PIPE_SemiPlanarStart`
trägt den Vermerk „Only DCMIPP_PIPE1 allows semi-planar buffer").

Pipe1 filtert zu jedem Zeitpunkt **genau einen Virtual Channel** (Feld
`P1FSCR.VC`, zwei Bit, also VC0–VC3, kein „alle VCs").

Gemessen (M0): die heutige Quelle überträgt die Kanäle **paketweise
verschachtelt** — beide Frames laufen fast vollständig gleichzeitig über die
Leitung (Muster `S1 S0 E1 E0`, 149 Überlappungen in 149 Frames, keine einzige
serielle Frameübertragung). Während Pipe1 einen Kanal aufnimmt, ist der andere
also nicht „noch nicht da", sondern bereits vorbei. Daraus folgt hart:

> **Composite-Rate = Quellrate ÷ Anzahl gleichzeitig gesendeter Kanäle.**

M1/M1-R bestätigen das auf der Hardware: bei 49,7 fps Quellrate und zwei
Kanälen kommen exakt 24,85 fps je Kanal an, Keep-Rate 50,00 %, kein Fehlpaar
(`vc_mismatch=0`). Der Verlust ist strukturell, nicht implementierungsbedingt —
es gibt keine Firmware, die ihn behebt.

Bei vier Kanälen wären das **12,4 fps** statt der geforderten 30.

Alles andere auf der N6-Seite hat dagegen Luft (M2-R, an der echten Geometrie
gemessen): der Encoder braucht für 1792×448 **21,3 ms** je Frame aus PSRAM bzw.
15,4 ms aus AXISRAM, gegen ein 30-fps-Budget von 33,3 ms — 64 % bzw. 46 %. Der
Speicher passt exakt, der SD-Pfad trägt die 1,25 MB/s. **Der Engpass sitzt
ausschliesslich in der Quelle.**

## 2. Architekturvergleich

Vier Varianten, die die 30 fps erreichen könnten, plus der Ist-Zustand. Alle
Zahlen für 4 Sensoren à 448×448, RAW10, Link 2 Lanes × 2500 Mbit/s = 5 Gbit/s
brutto.

| | Was der FPGA sendet | Composite-Rate | Zeitversatz Segment 0→3 | Link-Last | N6-Firmware | Voraussetzung an die Sensoren |
|---|---|---|---|---|---|---|
| **A** (Ist) | 4 VCs, 1920×1080, verschachtelt, 49,7 fps | **12,4 fps** ✗ | 60 ms | 82 % | VC-Switch ×4 | keine |
| **A′** | wie A, aber 448×448 bei **120 fps/Kanal** | 30,0 fps ✓ | 25 ms | 19 % | VC-Switch ×4 | 120 fps je Kanal |
| **B** | **ein VC**, 4 Frames à 448×448 **nacheinander**, 120 Frames/s | 30,0 fps ✓ | 25 ms | 19 % | nur Adress-Flip | Frame-Start um je 90° versetzt |
| **C** | **ein VC, ein Frame 1792×448** bei 30 fps | 30,0 fps ✓ | **0** | **4,8 %** | **nichts** | **Genlock (gleiche Phase)** |
| D | zweite Pipe parallel nutzen | — | — | — | — | verworfen, siehe unten |

Erläuterungen zu den Spalten:

- **Zeitversatz** ist der Abstand zwischen der Aufnahme des ersten und des
  letzten Segments *eines Composite-Frames*. Bei A/A′/B werden die Segmente
  nacheinander eingefangen, bei 120 fps also im Abstand von je 8,33 ms. Ein
  Objekt, das sich durch die Szene bewegt, steht in den vier Segmenten dann an
  Positionen aus vier verschiedenen Zeitpunkten. Für eine spätere Auswertung
  über mehrere Kameras hinweg ist das der teuerste Posten der Tabelle.
- **Link-Last**: A′ und B übertragen 120 Frames je Sensor und Sekunde, von denen
  30 verwendet werden — 75 % der Leitungslast ist Verwurf. C überträgt genau,
  was gebraucht wird.
- **N6-Firmware**: bei C steht Pipe1 fest auf VC0; es bleibt der
  Ping-Pong-Adresswechsel, den jede Variante braucht. Der in Phase 1 geplante
  VC-Switch entfällt ersatzlos.

**Variante D, und warum sie nicht geht.** Naheliegend wäre, Pipe1 und Pipe2 mit
`PIPEDIFF=1` auf verschiedene VCs zu legen und so den Durchsatz zu verdoppeln.
Das scheitert doppelt: Pipe2 hat keinen Demosaic, könnte also nur bereits
fertiges YUV verarbeiten (der FPGA müsste dann den ganzen ISP übernehmen), und
Pipe2 kann kein NV12 schreiben, sondern nur gepacktes YUYV — zwei Segmente im
Composite hätten damit ein anderes Speicherformat als die anderen zwei, was sich
nicht zu einem Frame für einen Encoder zusammensetzen lässt. Selbst wenn man das
Composite komplett auf YUYV umstellte, bräuchte es immer noch 60 fps aus den
Sensoren, und der Ping-Pong-Puffer (2 × 1,60 MB) passt dann nicht mehr in die
2,77 MB AXISRAM. Für die Vollständigkeit geprüft, nicht empfohlen.

## 3. Empfehlung: Variante C

**Der FPGA setzt das Composite zusammen und schickt es als einen Frame auf einem
Virtual Channel.** C gewinnt in jeder Spalte gleichzeitig — es ist die einzige
Variante ohne Zeitversatz zwischen den Segmenten, sie braucht ein Zwanzigstel
der Leitungslast von A′/B, sie verlangt von den Sensoren nur 30 fps statt 120,
und sie macht die N6-Firmware *einfacher* als heute statt komplizierter.

Der scheinbare Preis — der FPGA muss Bilddaten puffern — ist durch die
Umspezifikation auf **Breitformat** klein geworden, und das ist der Punkt, der
diese Empfehlung trägt:

- **Nebeneinander (1792×448, so wie jetzt spezifiziert):** Composite-Zeile *n*
  ist die Aneinanderreihung der Zeilen *n* aller vier Kanäle. Sind die Sensoren
  in Phase, muss der FPGA nur die Zeilen der Kanäle 1–3 zwischenspeichern,
  während er Kanal 0 ausgibt: **je Kanal 2 Zeilen à 560 Byte, zusammen rund
  4,5 KB.**
- **Übereinander (448×1792, die frühere Fehlspezifikation):** der Composite gibt
  erst alle 448 Zeilen von Kanal 0 aus, dann die von Kanal 1 — deren Frame ist zu
  dem Zeitpunkt aber längst gelaufen. Das verlangt **Vollbildpuffer für drei
  Kanäle, rund 735 KB.**

Die Drehung ins Breitformat macht die FPGA-Seite also um rund den Faktor 170
billiger. Auf der N6-Seite kostet sie nichts: 1792×448 und 448×1792 haben
dieselben 3136 Makroblöcke, und 896×896 als Formkontrolle liegt 0,7 % daneben —
Laufstreuung (M2-R).

## 4. Anforderungen (Variante C)

### 4.1 Bildaufbau

| Nr. | Anforderung | Begründung / Prüfkriterium |
|---|---|---|
| C-1 | Ein Composite-Frame ist **1792 × 448** Pixel, ein einziges CSI-2-Frame (ein FS/FE-Paar, 448 Zeilen à 1792 Pixel). | Der DCMIPP nimmt einen Frame als Einheit; `probe` meldet Zeilenzahl und Bytes/Zeile und muss genau das anzeigen. |
| C-2 | Sensor *k* liegt in den Spalten **[k·448, (k+1)·448)**, Reihenfolge fest und über alle Frames stabil. | Die Zuordnung Segment↔Kamera wird in der Firmware nicht ermittelt, sondern vorausgesetzt. |
| C-3 | Datentyp **RAW10 (0x2b)**, Bayer-Phase **RGGB in jedem Segment**, Segmentbreite gerade. | Der ISP demosaikt über den ganzen Frame mit einer einzigen Phase; 448 ist gerade, damit läuft das Bayer-Raster über die Nahtstellen durch. RGGB ist gegen die heutige Quelle gemessen, nicht vom IMX335 geerbt. |
| C-4 | **30,0 fps**, konstant, keine ausgelassenen Frames. | Encoder braucht 21,3 ms von 33,3 ms; drop-freier Betrieb ist damit erreichbar und ist das Ziel. |
| C-5 | Zeilenlänge auf der Leitung: **2240 Byte** (1792 × 10 bit). | Liegt unter den heute akzeptierten 2400 Byte/Zeile, also im bewährten Bereich. |
| C-6 | Der Entwurf soll auf **1984 × 496** (4 × 496×496) parametrierbar bleiben. | Beide Werte erfüllen die 16-Byte-Bedingung des Pixel-Packers. Ob 496 kommt, entscheidet die N6-Speicheraufteilung, nicht der FPGA — heute passt nur 448. |

### 4.2 Sensor-Synchronisation

| Nr. | Anforderung | Begründung / Prüfkriterium |
|---|---|---|
| C-7 | Alle vier Sensoren laufen auf **einem gemeinsamen Takt** und mit identischen Timing-Registern, sodass die Framedauer exakt gleich ist und der Phasenversatz **konstant** bleibt (keine Drift). | Nur dann genügt Zeilenpufferung. Driften die Sensoren, wächst der Versatz bis zum Vollbild. |
| C-8 | Der Phasenversatz zwischen den Kanälen ist **≤ 8 Zeilen** (Ziel 0). | Puffer = Versatz × 560 Byte × 4 Kanäle; 8 Zeilen ≈ 18 KB. Bei grösserem Versatz bitte die tatsächliche Zahl melden, dann rechnen wir den Puffer neu. |
| C-9 | Der Restversatz zwischen erstem und letztem Segment ist **≤ 1 ms**. | Das ist die eigentliche Produktanforderung hinter C-7/C-8: die vier Bilder eines Composite sollen denselben Zeitpunkt zeigen. 1 ms entspricht 3 % einer Framedauer. |

Der IMX258 hat laut bisheriger Recherche keinen Hardware-Trigger-Eingang; der
gangbare Weg ist gemeinsamer MCLK aus dem CrossLink plus gleichzeitiger
I²C-Start. **Ob das den Versatz auf ≤ 8 Zeilen bringt, ist die eine offene
Machbarkeitsfrage dieses Entwurfs** — bitte zuerst klären, bevor die
Composite-Logik gebaut wird.

### 4.3 Bildgleichheit über die Segmente

| Nr. | Anforderung | Begründung / Prüfkriterium |
|---|---|---|
| C-10 | Alle vier Sensoren arbeiten mit **gleicher Belichtung, gleichem Gain und gleichem Weissabgleich**; keine unabhängige Auto-Belichtung je Sensor. | Der N6 hat einen ISP für den *ganzen* Composite. Eine segmentweise Korrektur ist hardwareseitig nicht vorgesehen — unterschiedlich belichtete Sensoren ergeben sichtbar unterschiedlich helle Segmente, die niemand mehr trennt. |
| C-11 | Belichtungssteuerung, falls nötig, **gemeinsam für alle vier** (ein Sollwert, gleichzeitig gesetzt). | Andernfalls „atmen" einzelne Segmente gegeneinander. |

### 4.4 Link und Betriebsverhalten

| Nr. | Anforderung | Begründung / Prüfkriterium |
|---|---|---|
| C-12 | **Maximal 2 Datenlanes** — der STM32N6-CSI-Empfänger kann nicht mehr (`IS_DCMIPP_NUMBER_OF_LANES` kennt nur 1 oder 2). | Häufige Fehlannahme; ein 4-Lane-Entwurf wäre am Empfänger nicht anschliessbar. |
| C-13 | Lane-Rate: 2500 Mbit/s beibehalten, **oder** die neue Rate ausdrücklich mitteilen. | Der Empfänger muss auf die Rate eingestellt werden. 2500 ist die einzige Rate, auf der diese Quelle je einen sauberen Link hatte; 1250 erzeugte rund 200 unkorrigierbare Header-ECC-Fehler je 500 ms. Der Nutzdatenbedarf beträgt nur 241 Mbit/s (4,8 %), eine niedrigere Rate ist also möglich — aber sie muss mit `probe <rate>` neu verifiziert werden, nicht angenommen. |
| C-14 | Auf dem Composite-VC **keine unbeanspruchten Datentypen**. Statusdaten, falls gewünscht, auf einem eigenen VC mit eigenem DT. | Heute laufen zusätzlich 0x12 (EMBEDDED) und 0x2f (RAW20) mit, die niemand abholt; der Empfänger meldet daraufhin dauerhaft IDERR. Über 2 × 120 s gemessen (M3) ist das nachweislich harmlos — kein Pipe-Overrun, kein ECC/CRC/Sync-Fehler, kein Datenschaden. Es kostet aber die Aussagekraft des Fehlerbits im Feld. |
| C-15 | Die Quelle muss den Link nach einem **Neustart des Empfängers** von sich aus wieder aufbauen (LP-11 erkennen und neu senden), ohne dass sie stromlos gemacht wird. | Heute tut sie das **nicht**: nach jedem Flashen des N6 bleibt `frames=0`, bis die Quelle power-cycled wird. Im Produkt heisst das: jeder N6-Reset kostet das Videosignal dauerhaft. Prüfkriterium: N6 resetten, Bild kommt binnen weniger Sekunden von allein zurück. |
| C-16 | Fällt ein Sensor aus, sendet der FPGA den Composite **weiter** und füllt das betroffene Segment definiert (z. B. schwarz), statt den Stream anzuhalten. | Sonst nimmt ein defekter Sensor die anderen drei mit. |

## 5. Wenn Genlock nicht machbar ist

Dann ist **B** der Rückfallplan, nicht A′: gleiche Leitungslast und gleicher
Zeitversatz wie A′, aber der FPGA sendet die vier Frames nacheinander auf einem
VC, und die N6-Firmware braucht keinen VC-Switch, sondern nur den
Adresswechsel, den sie ohnehin hat. B verlangt von den Sensoren keinen Gleich-,
sondern einen **um je 90° versetzten** Frame-Start (dann genügt auch dort
Zeilenpufferung) und 120 Frames/s auf der Leitung bei 30 fps je Sensor.

Zu klären wäre bei B zusätzlich, woran die Firmware erkennt, welcher Frame zu
welchem Sensor gehört, wenn einmal ein Frame ausfällt — bei C stellt sich die
Frage nicht, weil die Zuordnung räumlich im Frame steht.

**A′** (vier VCs, 120 fps, N6 multiplext) ist die letzte Wahl: gleicher
Zeitversatz und gleicher Verwurf wie B, aber zusätzlich die VC-Umschaltmechanik
im N6, die bei jedem Sensorausfall neu einrasten muss.

**A ohne Änderung an der Quelle erreicht 12,4 fps** und verfehlt die Vorgabe um
den Faktor 2,4. Zur Einordnung: bei vier Kanälen à 1920×1080 wäre der Link
ausserdem zu 82 % belegt, sodass die heutigen 49,7 fps je Kanal nicht einmal
gesichert sind — die 12,4 fps sind eine Obergrenze, keine Zusage.

## 6. Was der Empfänger mitbringt (Grenzen, gegen die geplant werden kann)

- CSI-2-Empfänger: max. **2 Datenlanes**, VC0–VC3, eine VC-Auswahl je Pipe.
- Genau **ein ISP** (Pipe1): Demosaic, Schwarzwert, Gain, CCM, Gamma, YUV.
- Pixel-Packer-Pitch: Vielfaches von 16 Byte, ≤ 0x7FFF. 1792 und 1984 erfüllen
  das, 1720 oder 1800 nicht.
- **Maximale Zeilenbreite der Pixel-Pipes: 4094 Pixel** (Crop und Downsizer
  haben 12-Bit-Felder, dokumentiert als „from 0 to 4094 pixels wide"). Bei
  448er-Segmenten wären das bis zu 9 Segmente nebeneinander — die Breite ist
  also nicht die Grenze; Speicher und Encoder sind es (siehe unten).
- Crop-Fenster bis 4094 × 4094, Downsizer im DCMIPP vorhanden — der FPGA muss
  also nicht pixelgenau auf 448 skalieren, ein Schnitt im N6 ist möglich.
- Encoder (Hantro VC8000NanoE): 1792×448 = 3136 Makroblöcke, **21,3 ms/Frame**
  gemessen; 10 Mbit/s H.264, Segmentnähte fallen wegen 448/16 = 28 auf
  Makroblockgrenzen.
- Speicher: Composite-NV12 im Ping-Pong (2,41 MB) plus Bitstream-Ring (427 KB)
  füllen die 2,77 MB AXISRAM exakt aus. **448×448 je Sensor ist damit heute die
  Obergrenze**; 496 verlangt eine Umverteilung der AXISRAM-Regionen auf der
  N6-Seite und ist keine FPGA-Frage.

## 7. Rückfragen, die den Entwurf entscheiden

1. **Bekommen die vier Sensoren einen gemeinsamen Takt und einen gleichzeitigen
   Start, und wie gross ist der verbleibende Phasenversatz in Zeilen?**
   Davon hängt ab, ob C mit ~4,5 KB Zeilenpuffer auskommt oder Vollbildpuffer
   braucht — und damit, ob C oder B gebaut wird.
2. Kann der CrossLink die Zeilen von vier Kanälen bei 24,1 Mpixel/s
   aneinanderreihen (das ist der gesamte Ausgabepixeltakt von C)?
3. Bleibt es bei RAW10, oder soll der FPGA demosaikieren? **Bitte bei RAW10
   bleiben** — der ISP im N6 ist vorhanden, getunt und kostet nichts.
4. Kann C-15 (Link-Wiederaufbau nach Empfänger-Reset) erfüllt werden? Das ist
   unabhängig von der gewählten Variante ein Produktfehler, solange es fehlt.
