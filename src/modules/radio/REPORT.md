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
  Buffer: **511 byte** per il titolo e **255 byte** per la stazione (UTF-8,
  terminatore escluso). Anche un byte Latin-1 isolato a fine stringa viene
  traslitterato, non scartato.
- **Registrazione WAV** con `R` durante la riproduzione:
  - percorso `/audio/rec/radio_YYYYmmdd_HHMMSS.wav` (crea `/audio/rec` se
    serve);
  - PCM catturato dal callback weak `audio_process_extern` (16 bit, stereo,
    sample rate reale) in un ring buffer da **16 KB allocato sull'heap solo
    all'avvio della REC** e liberato allo stop;
  - scrittura su SD dal loop principale in `audioout::pump()` a fette da
    2 KB, al massimo 4 per pump (8 KB): il decoder riprende il controllo tra
    un pump e l'altro e non resta fermo su una scrittura unica da 16 KB;
  - campioni scartati per ring pieno esposti da `recordingDropped()` e
    mostrati in UI (avviso + contatore `drop` nella riga REC);
  - intestazione WAV da 44 byte scritta all'apertura e **riscritta alla
    chiusura** con la dimensione reale e il sample rate definitivo; `seek` e
    `write` dell'header sono controllati e un errore viene registrato;
  - indicatore `REC` lampeggiante, durata, spazio libero SD, avviso di
    salvataggio o errore;
  - stop con `R`, con `INVIO`/`ESC` o automaticamente all'uscita.
- **I2S robusto**: `audioout::beginPlayback()` reinstalla il driver I2S0 del
  decoder (stessa configurazione della libreria) prima di ogni stream. Se
  `M5.Mic` stava usando I2S0 viene fermato; poiché `Mic.end()` resetta il
  codec ES8311, il percorso DAC viene riacceso con un ciclo
  `Speaker.begin()`/`Speaker.end()` che non lascia I2S1 installato.
  `audioout::endPlayback()` ferma lo stream, finalizza il WAV, **disinstalla
  I2S0** e restituisce lo speaker a M5Unified (`Speaker.begin()` su I2S1, per
  i beep della UI).
- **Stato senza allocare il decoder**: la voce *Stato / ferma* usa
  `audioout::decoderRunning()`, che legge solo lo stato dell'eventuale
  decoder esistente. Non costruisce `Audio` e non installa I2S0 quando la
  radio non è ancora stata usata, lasciando la porta al microfono.
- **Allocazione decoder sicura**: `new (std::nothrow) Audio()`; se la heap
  non basta `decoderReady()` ritorna false, `beginPlayback()` rinuncia senza
  dereferenziare nullptr e lo speaker torna alla UI. `decoder()` resta per i
  chiamanti che hanno già superato `beginPlayback()`/`decoderReady()`.
- **Orologio senza toccare il TZ**: SNTP avviato con `esp_sntp_*` solo se non
  è già attivo; nessuna chiamata a `configTime`/`setenv("TZ")`. I nomi WAV
  usano `gmtime_r`, quindi sono sempre **UTC** anche se Tools ha un fuso
  locale diverso.
- **HTTPS rifiutato**: un URL `https://` resta visibile in playlist ma, se
  selezionato, mostra un avviso esplicito (TLS senza PSRAM può esaurire la
  heap e riavviare) e non viene aperto. Solo `http://` viene riprodotto.
- Un solo oggetto `Audio`, sempre tramite `audioout::decoder()` dopo un
  `beginPlayback()` riuscito.

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
- Limiti: **8 KB** di file, **40 stazioni**, nome massimo **48 byte**, URL
  massimo **220 byte**; oltre 8 KB il file viene ignorato con avviso, senza
  troncamenti silenziosi. I limiti sono riepilogati nella voce *Stato/ferma*.
- URL accettati solo se iniziano con `http://` o `https://`. Gli URL HTTPS
  restano in elenco ma vengono **rifiutati al play** con un avviso heap: il
  decoder TLS senza PSRAM può esaurire la memoria e riavviare. Usare HTTP.

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
- Nome file: `radio_YYYYmmdd_HHMMSS.wav`, timestamp **UTC** via SNTP avviato
  al primo ascolto. `configTime` non viene usato: il TZ globale di altri
  moduli resta intatto e il nome è coerente con `gmtime_r`. Se l'orologio non
  è sincronizzato si usa uptime (`radio_19700101_HHMMSS.wav`); in caso di
  collisione si aggiunge `_2`, `_3`, … fino a `_1000`. Se non c'è un nome
  libero la REC viene rifiutata, senza sovrascrivere file esistenti.
- Errori SD: short write, `getWriteError()` o spazio esaurito chiudono la
  registrazione, riscrivono l'header e mostrano un avviso senza fermare la
  radio. L'esito della riscrittura dell'header viene controllato e loggato.

## 5. Dimensione firmware

**Dimensione integrata di riferimento (trunk con tutte le 10 feature):
~2.618.256 byte**, valore fornito dal supervisore. Il margine sullo slot OTA
da 2.621.440 byte è quindi di soli **~3,2 KB**: dopo il merge va ricontrollata
la dimensione del binario integrato prima del rilascio.

Build `python -m platformio run -e cardputer-adv-offensive` in questa
worktree (solo feature 02): **SUCCESS**.

| Voce | Valore |
|---|---|
| `firmware.bin` locale | **2.383.760 byte** (2,27 MiB) |
| Flash slot locale | 2.383.397 / 2.621.440 byte = **90,9 %** |
| RAM statica | 116.424 / 327.680 byte = **35,5 %** |
| Heap durante REC | +16.384 byte per il ring, liberati allo stop |

Rispetto alla baseline di questa worktree (2.358.193 byte di flash,
115.520 byte di RAM statica) la feature aggiunge ~25 KB di flash e **~904
byte** di RAM statica (soprattutto buffer metadati 512+256 byte). Il ring da
16 KB non è statico: viene allocato con `new (std::nothrow)` solo quando
parte la registrazione, dopo un controllo di heap, e liberato in
`finishRecording()`. Se l'allocazione fallisce la radio resta in ascolto e
avvisa "Registrazione non avviata".

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
- **Ciclo I2S verificato sul codice di M5Unified 0.3.0** (non su hardware):
  - `M5.Speaker` usa `I2S_NUM_1` su Cardputer ADV; decoder e `M5.Mic` usano
    `I2S_NUM_0`;
  - `Speaker.end()` scrive una lista di registri ES8311 vuota su questa
    board (`disabled_bulk_data = {0}`), quindi non spegne il codec: il
    decoder può pilotare lo stesso ES8311 via I2S0;
  - `Mic.end()` invece resetta l'ES8311 (`CSM POWER DOWN`). Per questo, se il
    mic era attivo, `beginPlayback()` esegue `Speaker.begin()` +
    `Speaker.end()` per riscrivere i registri DAC e poi libera subito I2S1;
  - `Mic.begin()` dopo la radio trova I2S0 libero, perché `endPlayback()` lo
    disinstalla.
- Non è stato possibile eseguire test su hardware reale da questo ambiente
  (niente Cardputer/SD collegate): i test di ascolto, registrazione e ciclo
  I2S sono manuali, elencati sotto.
- La voce *Stato / ferma* usa `decoderRunning()`: non deve comparire alcuna
  allocazione del decoder né I2S0 installato se la radio non è mai stata
  avviata (verificabile con il modulo Audio/microfono attivo).
- **TZ**: nessuna chiamata a `configTime`/`setenv("TZ")` nel modulo; SNTP è
  avviato solo con `esp_sntp_*` e i nomi usano `gmtime_r` (UTC).
- **HTTPS**: il parser mantiene la riga, `playStation()` la rifiuta prima di
  toccare Wi-Fi/I2S con un messaggio esplicito sull'heap.
- **Buffer metadati**: 512/256 byte, verificati a compilazione; la
  traslitterazione gestisce anche un byte Latin-1 isolato a fine stringa.

## 7. Limiti e rischi

- **User-Agent**: ESP32-audioI2S 2.3.0 non invia `User-Agent` (la riga è
  commentata in `Audio.cpp`, issue #363) e non espone un'API per impostarlo.
  Le prove raw senza UA su Groove Salad e Drone Zone hanno restituito 200,
  quindi il 403 segnalato non si è riprodotto. Se una stazione dovesse
  rispondere 403, servono un URL alternativo o una patch della libreria: non
  è possibile farlo dai soli file della feature.
- **HTTPS**: il parser lascia l'URL in playlist per non nasconderlo, ma il
  play viene **rifiutato** con avviso: il decoder usa TLS con `setInsecure()`
  e senza PSRAM può esaurire la heap e riavviare. Solo `http://` è
  riproducibile.
- **Metadati**: i buffer sono 512 byte (titolo) e 256 byte (stazione); testi
  più lunghi vengono troncati dal callback. Il limite è documentato anche nel
  codice (`g_title`, `g_station`).
- **Decoder non allocabile**: se `new (std::nothrow) Audio()` fallisce,
  `decoderReady()`/`beginPlayback()` falliscono in modo controllato e la UI
  avvisa; nessun dereference di nullptr. La voce *Stato* non tenta
  l'allocazione.
- **REC e SD**: il ring è di 16 KB e `pump()` scrive al massimo 8 KB per
  chiamata in fette da 2 KB; se la SD è più lenta della rete il ring si
  riempie e i campioni persi sono mostrati in UI (`drop N`) e loggati alla
  chiusura. Allo stop il drain finale può arrivare a 16 KB in una volta.
- **Fuso orario**: SNTP non tocca più il TZ globale; i nomi WAV sono UTC.
  Se un altro modulo cambia TZ, i nomi restano coerenti perché usano
  `gmtime_r`. Senza sync resta il fallback 1970 + uptime.
- **Mic/I2S0**: la radio prende I2S0 all'avvio dello stream e lo rilascia
  all'uscita. Se `M5.Mic` stava già usando I2S0, `beginPlayback()` chiama
  `Mic.end()` (la registrazione del modulo Audio in corso viene interrotta),
  riaccende il DAC ES8311 e reinstalla il driver del decoder. Dopo
  `endPlayback()` il driver I2S0 è disinstallato, quindi
  `M5.Mic.begin()` può ripartire. L'uso **contemporaneo** di radio e
  microfono resta impossibile: è lo stesso periferico.
- **Heap per il ring REC**: 16 KB vengono allocati solo durante la
  registrazione, con controllo `ESP.getFreeHeap()` (margine 8 KB) e
  `new (std::nothrow)`. In caso di fallimento la REC non parte e la radio
  continua; il file parziale viene rimosso.
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
   finalizzato con l'header corretto; verificare anche che il ring venga
   liberato (heap che torna al valore pre-REC).
6. Premere ESC in riproduzione: ritorno al menu, beep UI funzionante,
   nessun file WAV lasciato aperto.
7. **Radio → Audio → Radio**: ascoltare la radio, uscire, aprire il modulo
   Audio e registrare un memo con `M5.Mic` (poi fermarlo), uscire e riaprire
   la radio. L'ascolto deve ripartire, la REC radio deve funzionare e i beep
   della UI devono essere udibili in entrambi i ritorni al menu.
8. **Mic già attivo → Radio**: avviare un memo, senza fermarlo aprire la
   radio. `Mic.end()` deve interrompere il memo, la radio deve prendersi
   I2S0 e suonare; all'uscita `M5.Mic.begin()` deve poter ripartire.
9. **Radio → uscita → beep**: dopo `endPlayback()` premere un tasto che
   produce un beep UI (es. cambio voce menu) senza riaprire la radio:
   `Speaker.begin()` su I2S1 deve ripristinare l'audio.
10. **TZ/Tools**: impostare un fuso non UTC in Tools, ascoltare la radio e
    registrare: il nome WAV deve essere UTC coerente e Tools deve mantenere
    il proprio fuso dopo l'uscita.
11. **HTTPS**: inserire una stazione `https://` in playlist, selezionarla:
    deve comparire l'avviso heap e la radio non deve tentare la connessione.
12. **Metadati lunghi**: stream con titolo >127 byte: deve comparire e
    scorrere fino a 511 byte, senza troncamenti a metà carattere.
13. **Drop REC**: usare una SD lenta o un file system molto occupato durante
    la REC; se il ring si riempie deve comparire `drop N` e l'avviso, senza
    bloccare l'ascolto.
14. **Collisione nomi**: creare molti WAV con lo stesso secondo/uptime; la
    numerazione deve arrivare a `_1000` e, esaurita, la REC deve essere
    rifiutata senza sovrascrivere.
