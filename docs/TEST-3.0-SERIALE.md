# OrbitOS 3.0 — test seriale su Cardputer ADV

Data: 2026-11-27. Porta: COM5 (USB-Serial/JTAG). Firmware: build
`3.0.0`, 1.640.816 B, partizione app 2,75 MB (42% libero).
Patch deauth applicata con `tools/patch_raw_tx.py` prima del flash.

## Selftest (`ORBIT_DIAG_SELFTEST=ON`)

```
RESULT display            PASS
RESULT framebuffer        PASS  135x240, sprite DMA 16bpp (63 KB)
RESULT codec-es8311       PASS  I2C 0x18
RESULT nvs                PASS
RESULT sd                 PASS
RESULT sd-content         PASS  music=70 ir=2257 games=5
RESULT littlefs           PASS
RESULT lfs-content        PASS  games=3
RESULT audio-begin        PASS
RESULT audio-play         PASS  pos=1488ms (MP3 reale)
RESULT wifi-scan+raw      PASS  reti=2, deauth-tx=OK, beacon-tx=OK
RESULT ble-scan           PASS  4 dispositivi
heap after radios         143 KB
settings                  bright=128 theme=0 rot=0 nick=lore_1 (import 2.x)
```

## Firmware di produzione

```
OrbitOS 3.0 IDF boot
Home
[DBG] heap libero=194796 minimo=189756
[DBG] blocco 8bit=131072 dma=131072
[DBG] sd totale=14891MB liberi=14189MB
[DBG] connessione WiFi...
wifi:connected with <SSID>
esp_netif_handlers: sta ip: 192.168.1.9
[DBG] wifi ok
```

## Giochi

- Wren `2048`, `breakout` e `flappy` da `/littlefs/games`: avvio con
  `heap=167844` (framebuffer 16bpp mantenuto + WiFi fermato, stack task
  24 KB); nessun errore di memoria.
- Giochi nativi: rotazione scelta con `width > height` (landscape),
  ripristino della rotazione utente all'uscita.
- Il test di uscita con ESC e la resa a schermo di ogni singolo gioco
  restano da verificare manualmente.

## Deauth

`deauth-tx=OK (ESP_OK)` e `beacon-tx=OK` con il binario patchato.
Senza patch IDF 5.4 risponde `ESP_ERR_INVALID_ARG` (filtro chiuso in
`libnet80211.a`); la patch sostituisce il prologo della funzione con
`movi.n a2, 0; retw.n` e `esptool elf2image` rigenera checksum e SHA-256.
