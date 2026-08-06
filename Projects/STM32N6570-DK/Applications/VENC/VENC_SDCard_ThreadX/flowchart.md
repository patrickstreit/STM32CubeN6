# Video-Pipeline Zusammenfassung

Die Initialisierung der Kamera-Pipeline beginnt in `MX_DCMIPP_Init()`. Dort wird `DCMIPP_PIPE1` für den Aufnahme- und Encoderpfad konfiguriert: CSI-Input, YUV-Konvertierung, Downsize und das Ausgabeformat für den Encoder. Der eigentliche Start der Pipe1-Capture erfolgt später in `encoder_start()`, das über `dcmipp_config(GetInputFrame(NULL))` die erste Zieladresse für den Capture-Buffer setzt und danach den Encoder-Stream vorbereitet.

Der Vorschaupfad auf das LCD wird in `lcd_init()` aktiviert. Diese Funktion schaltet `DCMIPP_PIPE2` per `HAL_DCMIPP_PIPE_CSI_EnableShare()` auf denselben Kameraeingang, konfiguriert RGB565 plus Downsize, legt `lcd_frame` als Zielbuffer fest und startet die Pipe mit `HAL_DCMIPP_CSI_PIPE_Start()`. Über `BSP_LCD_SetLayerAddress()` wird derselbe Buffer direkt an den LTDC gebunden. Wenn `BSP_CAMERA_FrameEventCallback()` für `DCMIPP_PIPE2` aufgerufen wird, triggert der Code mit `BSP_LCD_Reload(0, BSP_LCD_RELOAD_VERTICAL_BLANKING)` das saubere Umschalten zur nächsten Darstellung.

Die eigentliche Aufnahme- und Encode-Kette läuft im Thread `venc_thread_func()`. Nach `VENC_APP_EncodingStart()` wartet der Thread auf `FRAME_RECEIVED_FLAG`. Dieses Flag wird in `BSP_CAMERA_FrameEventCallback()` gesetzt, sobald `DCMIPP_PIPE1` einen Frame fertiggestellt hat. Im selben Callback wird mit `dcmipp_set_memory_address(GetNextFrame(frame_received))` sofort der nächste Input-Buffer programmiert, sodass die Capture-Pipeline kontinuierlich weiterlaufen kann. In der aktuellen 720p-Frame-Konfiguration liegen die Eingabebuffer als `input_frame[2]` in PSRAM; jeder Buffer ist 1,843,200 Byte groß.

Für die H.264-Erzeugung verwendet `encode_frame()` den nächsten Capture-Buffer über `GetNextFrame(nb_encoded_frame)` und ruft anschließend `H264EncStrmEncode()` auf. Der Encoder-Arbeitsbereich liegt in `ewl_pool` mit 8 MiB in PSRAM. Der komprimierte Bitstream landet in `h264_bitstream`, das als non-cacheable Buffer mit insgesamt 2,560,000 Byte angelegt ist. Dieser Bereich wird beim Start von `venc_thread_func()` per `tx_block_pool_create()` als `venc_block_pool` in 8 Blöcke zu je 320,000 Byte aufgeteilt. Jeder erfolgreich encodierte Frame wird als `venc_output_frame_t` über `tx_queue_send()` in `enc_frame_queue` gestellt.

Der SD-Schreibpfad läuft separat in `sdcard_thread_func()`. Dieser Thread holt Daten mit `VENC_APP_GetData()`, das jeweils einen Eintrag aus `enc_frame_queue` liest und dabei den zuvor verwendeten Block wieder an `venc_block_pool` zurückgibt. Vor dem Schreiben wartet `sdcard_thread_func()` explizit auf einen ersten I-Frame, also auf den Rückgabewert `H264ENC_INTRA_FRAME`. Die eigentlichen Bitstream-Daten werden zunächst in `sd_write_buffer` gesammelt, einem 64-KiB-Puffer im `.bss`, und anschließend sektorweise über `VENC_FileX_write()` in das Dateisystem geschrieben. FileX nutzt zusätzlich `fx_sd_media_memory` als 32-KiB-Mediencache.

Der letzte Hardware-Schritt zur SD-Karte liegt unterhalb von FileX im SD-Treiber. `VENC_FileX_write()` ruft `fx_file_write()` auf, das schließlich im Glue-Layer `fx_stm32_sd_write_blocks()` auf `HAL_SD_WriteBlocks_DMA()` führt. Damit verlassen die H.264-Daten den Softwarepfad und werden per `SDMMC2`-DMA auf die SD-Karte geschrieben. Die Datei-Rotation erfolgt in `sdcard_thread_func()` immer erst dann, wenn `NB_FRAMES_PER_FILE` erreicht ist und gleichzeitig wieder ein I-Frame vorliegt.

```mermaid
%%{init: {"markdownAutoWrap": true, "flowchart": {"htmlLabels": true, "curve": "linear"}}}%%
flowchart TD
    A["IMX335 Sensor<br/>RAW10 source"] --> B["CSI Receiver<br/>DCMIPP shared input"]

    B --> P1["Pipe1<br/>YUV convert + downsize"]
    B --> P2["Pipe2 share<br/>RGB565 + downsize"]

    subgraph ENC["Pipe1 → Encode → SD"]
        direction TD
        P1 --> C["input_frame[2]<br/>2 × 1,843,200 B in PSRAM"]
        C -->|"FrameEventCallback<br/>next buffer programmed"| D["venc_thread<br/>GetNextFrame"]
        D --> E["VENC HW<br/>H264EncStrmEncode"]
        E --> E0["ewl_pool<br/>8 MiB in PSRAM"]
        E --> F["h264_bitstream / venc_block_pool<br/>8 × 320,000 B = 2,560,000 B<br/>non-cacheable"]
        F --> G["enc_frame_queue<br/>8 descriptors × 16 B in .bss"]
        G --> H["sdcard_thread<br/>VENC_APP_GetData"]
        H --> I["sd_write_buffer<br/>64 KiB in .bss"]
        I --> J["FileX media cache<br/>fx_sd_media_memory 32 KiB"]
        J --> K["fx_file_write + SD driver<br/>SDMMC2 DMA"]
        K --> L["SD card<br/>.h264 files"]
    end

    subgraph LCD["Pipe2 → LCD preview"]
        direction TD
        P2 --> M["lcd_frame<br/>LCD_DEFAULT_WIDTH × LCD_DEFAULT_HEIGHT × 2 B<br/>in PSRAM"]
        M --> N["BSP_LCD_SetLayerAddress<br/>LTDC layer base"]
        M -->|"FrameEventCallback Pipe2<br/>vertical blanking reload"| O["BSP_LCD_Reload<br/>VBlank sync"]
        N --> P["LTDC<br/>display fetch"]
        O --> P
        P --> Q["Board LCD<br/>live preview"]
    end
```