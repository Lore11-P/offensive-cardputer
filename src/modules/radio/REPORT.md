# Feature 02 — Radio: playlist SD, metadati ICY, registrazione WAV

## 1. Cosa è stato implementato

- **Playlist personale** `/radio/stations.txt` sulla microSD, caricata
  all'ingresso nel menu e ricaricabile a caldo con la voce *Ricarica playlist*
  o con il tasto `R`. Se il file manca, è vuoto, è troppo grande o non contiene
  stazioni valide, si usano le 5 stazioni predefinite con un avviso.
- **Metadati ICY** tramite i callback weak `audio_showstreamtitle` e
  `audio_showstation` della libreria ESP32-audioI2S. Artista/titolo (o nome
  della stazione) compare nella schermata di ascolto; se supera la larghezza
  del display scorre a marquee. I caratteri non ASCII vengono traslitterati
  (é→e, à→a, virgolette tipografiche, ecc.) perché il font ORBIT è ASCII.
- **Registrazione WAV** con `R` durante la riproduzione:
  - percorso `/audio/rec/radio_YYYYmmdd_HHMMSS.wav` (crea `/audio/rec` se
    serve);
  - PCM catturato dal callback weak `audio_process_extern` (16 bit, stereo,
    sample rate reale) in un ring buffer da **16 KB** in RAM;
  - scrittura su SD dal loop principale in `audioout::pump()`, mai dal
    callback del decoder;
  - intestazione WAV da 44 byte scritta all'apertura e **riscritta alla
    chiusura** con la dimensione reale e il sample rate definitivo;
  - indicatore `REC` lampeggiante, durata, spazio libero SD, avviso di
    salvataggio o errore;
  - stop con `R`, con `INVIO`/`ESC` o automaticamente all'uscita.
- **I2S robusto**: `audioout::beginPlayback()` reinstalla il driver I2S0 del
  decoder (stessa configurazione della libreria) prima di ogni stream, così
  la radio funziona anche dopo che `M5.Mic` ha usato o fermato I2S0.
  `audioout::endPlayback()` ferma lo stream, finalizza il WAV, **disinstalla
  I2S0** e restituisce lo speaker a M5Unified.
- Un solo oggetto `Audio`, sempre tramite `audioout::decoder()`.

File modificati: `src/modules/radio/RadioMenu.cpp`, `src/utils/AudioOut.cpp`,
`include/AudioOut.h`, più questo `REPORT.md`. Nessuna modifica a `main.cpp`,
`platformio.ini`, `ModuleFlags.h`, `Config.cpp` o altri moduli.

## 2. Formato della playlist

Una stazione per riga, separatore `|` oppure `,`:

```
# commento (anche con ;)
SomaFM Groove Salad|http://ice1.somafm.com/groovesalad-128-mp3
SomaFM Drone Zone,http://ice1.somafm.com/dronezone-128-mp3
Radio Paradise, http://stream.radioparadise.com/mp3-128
```

- Righe vuote, `#` e `;` sono ignorate.
- CRLF e BOM UTF-8 gestiti.
- Con il formato a virgola, la virgola separatrice è quella seguita da
  `http://`/`https://`; le virgole dentro l'URL restano intatte.
- Limiti: **8 KB** di file e **40 stazioni**; oltre 8 KB il file viene
  ignorato con avviso, senza troncamenti silenziosi.
- URL accettati solo se iniziano con `http://` o `https://`. Il decoder non
  ha un client TLS leggero: gli stream HTTP sono fortemente preferiti.

## 3. Comandi

Elenco stazioni:

| Tasto | Azione |
|---|---|
| `;` / `.` | su / giù nell'elenco (scorre oltre le 4 righe visibili) |
| `INVIO` | ascolta la stazione selezionata / esegue la voce |
| `R` | ricarica la playlist dalla SD |
| `ESC` | esce dal menu Radio |
| voci | `Ricarica playlist`, `Stato / ferma`, `WiFi` |

In riproduzione:

| Tasto | Azione |
|---|---|
| `R` | avvia / ferma la registrazione WAV |
| `;` / `.` | volume + / − |
| `INVIO` o `ESC` | ferma e torna all'elenco |

## 4. Formato WAV

- RIFF/WAVE PCM, 44 byte di intestazione.
- **16 bit, 2 canali, little-endian** (`L`, `R` interleaved).
- Sample rate reale dello stream: il valore viene aggiornato dal callback e
  riscritto nell'header alla chiusura (la libreria parte da un default di
  16000 Hz, non attendibile).
- Gli stream mono vengono salvati come stereo duplicando il campione su L/R.
- Gli stream a 8 bit non vengono catturati (la radio usa MP3/AAC 16 bit).
- Spazio: circa **10,1 MB al minuto** a 44,1 kHz stereo. Il limite RIFF a 32
  bit copre circa 6,7 ore; prima dello start serve almeno 4 MB liberi.
- Il PCM è catturato **prima del gain del decoder**: volume 0 non rende muta
  la registrazione. Non passa nemmeno dai filtri IIR finali, quindi può
  differire leggermente da ciò che esce dallo speaker.
- Nome file: `radio_YYYYmmdd_HHMMSS.wav`, timestamp UTC via SNTP avviato al
  primo ascolto. Se l'orologio non è sincronizzato si usa uptime
  (`radio_19700101_HHMMSS.wav`); in caso di collisione si aggiunge `_2`,
  `_3`, … per non sovrascrivere.
- Errori SD: short write, `getWriteError()` o spazio esaurito chiudono la
  registrazione, riscrivono l'header e mostrano un avviso senza fermare la
  radio.

## 5. Dimensione firmware

Build `python -m platformio run -e cardputer-adv-offensive`: **SUCCESS**.

| Voce | Valore |
|---|---|
| `firmware.bin` | **2.379.728 byte** (2,27 MiB) |
| Flash slot | 2.379.365 / 2.621.440 byte = **90,8 %** |
| RAM statica | 132.264 / 327.680 byte = **40,4 %** |
| di cui ring REC | 16.384 byte |

La feature aggiunge ~20,7 KB di flash e ~16,7 KB di RAM statica rispetto alla
baseline (2.358.193 byte di flash, 35,3 % RAM).

## 6. Test eseguiti

- Build di baseline e build finali ripetute: sempre SUCCESS.
- Verifica dei simboli nel firmware: `audio_process_extern`,
  `audio_showstreamtitle`, `audio_showstation` sono definiti nel firmware
  (i weak della libreria vengono sovrascritti correttamente).
- Intestazione WAV verificata con il modulo Python `wave`: 2 canali, 16 bit,
  44100 Hz, durata coerente con i byte dati.
- Richiesta HTTP raw identica a quella della libreria (Host, Icy-MetaData:1,
  Authorization vuota, **senza User-Agent**) verso:
  - `http://ice1.somafm.com/groovesalad-128-mp3` → `HTTP/1.0 200 OK`
  - `http://ice1.somafm.com/dronezone-128-mp3` → `HTTP/1.0 200 OK`
- Non è stato possibile eseguire test su hardware reale da questo ambiente
  (niente Cardputer/SD collegate): i test di ascolto e registrazione sono
  manuali, elencati sotto.

## 7. Limiti e rischi

- **User-Agent**: ESP32-audioI2S 2.3.0 non invia `User-Agent` (la riga è
  commentata in `Audio.cpp`, issue #363) e non espone un'API per impostarlo.
  Le prove raw senza UA su Groove Salad e Drone Zone hanno restituito 200,
  quindi il 403 segnalato non si è riprodotto. Se una stazione dovesse
  rispondere 403, servono un URL alternativo o una patch della libreria: non
  è possibile farlo dai soli file della feature.
- **HTTPS**: il parser accetta `https://`, ma il decoder usa TLS con
  `setInsecure()` e molta RAM; la documentazione del progetto consiglia HTTP.
- **Mic/I2S0**: la radio prende I2S0 all'avvio dello stream e lo rilascia
  all'uscita; con `M5.Mic` attivo non possono convivere. La reinizializzazione
  è pensata per il caso "mic usato prima della radio", non per l'uso
  contemporaneo.
- **Sample rate variabile** a metà stream: l'header usa l'ultimo rate noto;
  un cambio di rate durante la registrazione produrrebbe un WAV con durata
  errata. Non capita con gli MP3 CBR usati come predefiniti.
- **SD rimossa a caldo**: la registrazione viene chiusa al primo errore di
  scrittura, ma un'eventuale attesa del driver SD non è controllabile dal
  firmware.
- **Menu**: l'elenco stazioni è disegnato a mano invece di usare
  `MenuManager`; `setItems()` durante l'azione di una voce distruggerebbe la
  `std::function` in esecuzione, impedendo il refresh in-place richiesto per
  *Ricarica playlist*.
- **Orologio**: senza NTP il nome file usa il 1970 + uptime; la radio avvia
  SNTP al primo ascolto, quindi in genere il timestamp è corretto dopo pochi
  secondi. Il fuso è UTC.
- **Callback globali**: `audio_process_extern`, `audio_showstreamtitle` e
  `audio_showstation` sono definiti una sola volta in `AudioOut.cpp`. Altri
  moduli (es. Audio) non devono definirli, altrimenti il link fallisce.

## 8. Test manuali suggeriti

1. Creare `/radio/stations.txt` con 6+ stazioni, separatori misti, CRLF e
   qualche accento; verificare elenco, scorrimento con `;`/`.` e `R` di
   ricarica (footer `SD n`).
2. Rinominare il file e verificare il fallback alle 5 stazioni e l'avviso
   "file assente".
3. Ascoltare Groove Salad: comparsa e scorrimento di artista/titolo ICY.
4. Registrare 30 s con `R`, fermare con `R`, aprire il WAV sul PC: deve
   durare ~30 s, essere stereo 16 bit e avere audio anche a volume 0.
5. Con SD piena o rimossa durante la REC: avviso, nessun hang, WAV
   finalizzato con l'header corretto.
6. Dopo aver usato il modulo Audio/microfono, riaprire la radio: l'audio deve
   ripartire (reinit I2S0) e i beep della UI tornare all'uscita.
7. Premere ESC in riproduzione: ritorno al menu, beep UI funzionante,
   nessun file WAV lasciato aperto.
