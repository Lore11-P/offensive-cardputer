# Flasher CardputerFirmware v1.0

Pacchetto pronto all'uso per installare il firmware su M5Stack Cardputer ADV
(ESP32-S3, 8 MB flash) senza compilare nulla. Include 307 telecomandi IR,
BLE spam multibrand, chat BLE, Wi-Fi scanner, sei giochi e file browser microSD.

> **Questa confezione contiene la build OFFENSIVA** (`ENABLE_OFFENSIVE=1`):
> deauth Wi-Fi, evil twin/portal, BLE spam e jammer IR sono attivi. Usala solo
> su hardware e reti tuoi, in ambiente RF isolato e nel rispetto delle leggi
> locali. Per ripristinare la build pubblica esegui `.\build.ps1` senza flag.

## Contenuto

- `flash.ps1` / `flash.bat`: flashing da Windows con esptool.
- `build.ps1`: ricompila firmware e LittleFS e aggiorna i binari del flasher.
- `bin/`: `bootloader.bin`, `partitions.bin`, `firmware.bin`, `littlefs.bin`.
- `web/`: flasher browser ESP Web Tools con gli stessi binari.

## Flash da Windows (consigliato)

Requisiti: Windows 10/11 e Python 3.10+ nel PATH. Lo script installa
automaticamente `esptool` se manca; non serve PlatformIO.

```powershell
cd flasher
.\flash.bat
```

Con porta esplicita e cancellazione completa:

```powershell
.\flash.bat -Port COM5
.\flash.bat -Port COM5 -Erase
```

Se la porta non viene rilevata: tieni premuto **G0**, reinserisci il cavo USB,
ripeti il comando. Il baud rate predefinito è 1500000.

## Flasher web

Serve Chrome o Edge desktop. Le pagine `file://` non funzionano: avvia un
server locale nella cartella `web`.

```powershell
cd flasher\web
python -m http.server 8000
```

Apri <http://localhost:8000>, collega la Cardputer via USB e premi
**Installa firmware**. La pagina carica ESP Web Tools dal CDN, quindi serve
una connessione internet attiva. Il flasher usa gli offset:

| Offset | Componente |
|---|---|
| `0x0` | bootloader |
| `0x8000` | tabella partizioni |
| `0x10000` | firmware |
| `0x510000` | LittleFS 2,9 MB (icone + database IR) |

## Ricompilare i binari

Dalla cartella del progetto:

```powershell
cd flasher
.\build.ps1
```

Lo script installa PlatformIO se necessario, compila l'ambiente pubblico
`cardputer-adv`, genera LittleFS e copia i quattro artefatti in `bin/` e `web/`.

Per compilare e pubblicare la variante offensiva nel flasher:

```powershell
.\build.ps1 -Offensive
```

Attenzione: questo **sostituisce** i binari del flasher con la build offensiva.
Per tornare alla build pubblica esegui `.\build.ps1` senza parametri. I binari
offensivi restano anche in `.pio/build/cardputer-adv-offensive/`.

## Dopo il flash

- Monitor seriale a **115200 baud** per il log di avvio.
- Se il display resta nero, riprova con `-Erase` e controlla il cavo USB dati.
- La microSD è opzionale: il filesystem interno contiene già icone e database IR.
- Questa confezione include la build offensiva; rigenera con `.\build.ps1`
  per tornare alla variante pubblica senza deauth/spam/jammer.
