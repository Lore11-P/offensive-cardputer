# orbit_ir

Modulo infrarossi di OrbitOS 3.0 per M5Stack Cardputer ADV (ESP32-S3).
Porta il comportamento utile di `src/modules/ir/IrMenu.cpp` del firmware 2.x
senza Arduino e senza IRremoteESP8266: parser Flipper `.ir`, conversione in
durate, trasmissione RMT nativa su **GPIO44**.

## API

```cpp
#include "orbit_ir/Ir.h"

orbit_ir::runMenu();   // menu "Infrarossi" (parent "Home")
```

Il componente non modifica `main`, `orbit_app` o la sdkconfig. Il parent deve
aggiungere `orbit_ir` alle dipendenze del proprio componente e chiamare
`orbit_ir::runMenu()`. Dipendenze del componente: `orbit_ui`, `orbit_hal`,
`orbit_store`, `orbit_mascot`, `esp_driver_rmt`, `driver`, `nvs_flash`.

## Database e path

All'apertura del menu vengono montati (se presenti):

- microSD: `orbit_store::mountSd()` -> `/sd`
- LittleFS: `orbit_store::mountLittleFs()` -> `/littlefs`

Struttura cercata:

```
/sd/ir/<categoria>/<marca>/<file>.ir
/littlefs/ir/<categoria>/<marca>/<file>.ir
/sd/ir/<marca>/<file>.ir              (albero piatto 2.x)
/littlefs/ir/<marca>/<file>.ir        (albero piatto 2.x)
```

- Le categorie sono enumerate dalle directory reali: TV, Clima, Audio, DVD,
  Proiettore, Console, Universale, Altro (e alias inglesi comuni). Una
  categoria compare solo se ha contenuto; le directory sconosciute compaiono
  con il loro nome.
- Le marche vengono lette da entrambi i filesystem, deduplicate per nome
  (case-insensitive). I file della stessa marca vengono fusi e deduplicati per
  nome; la microSD ha priorita' su LittleFS.
- L'albero piatto `/ir/<marca>/<file>.ir` resta visibile dentro **TV**, come
  nel 2.x.
- Un file per volta viene letto in un buffer al massimo 16 KB; un file piu'
  grande viene rifiutato con un messaggio. Non ci sono tabelle statiche grandi.

## Formato `.ir` supportato

Campi riconosciuti, in qualunque ordine dentro un blocco:

```ini
name: Power
type: parsed
protocol: NEC
address: 04 00 00 00
command: 08 00 00 00
```

```ini
name: Tasto
type: raw
frequency: 38000
duty_cycle: 0.330000
data: 9000, 4500, 560, 560, 560, 1690
```

- `address` e `command`: byte esadecimali little-endian come nel formato
  Flipper (`04 00 00 00` -> `0x04`). Un token singolo piu' lungo di un byte
  viene preso come valore completo.
- `type: raw`: `data` puo' essere su piu' righe e usare virgole o spazi. Le
  durate sono in microsecondi. Se sono tutte positive vengono interpretate
  alternando mark/space a partire dal mark; se ci sono valori negativi, il
  segno indica il livello (positivo = mark, negativo = space).
- `#` separa i comandi; commenti e righe `Filetype:`/`Version:` sono ignorati.
- I comandi senza `name:` o senza protocollo/raw valido vengono scartati o
  mostrati come `[n/d]`.

## Protocolli

Conversione in durate fedele a IRremoteESP8266 (stesse formule di
`encodeNEC`, `encodeSAMSUNG`, `encodeSony`, `encodeRC5`, `encodeRC6`,
`encodeLG`, `encodeJVC`, `encodeKaseikyo` e stesso `sendGeneric`):

| Protocollo Flipper | Note |
|---|---|
| `NEC`, `NECext` | 32 bit, header 8960/4480 us, bit 560/1680 us (1) e 560/560 us (0), gap a 108.08 ms |
| `Samsung`, `Samsung32` | 32 bit, header 4480/4480 us, bit 560/1680 us (1) e 560/560 us (0), gap a 108.08 ms |
| `Sony`, `SIRC`, `SIRC12/15/20` | 12/15/20 bit, header 2400/600 us, 1 = 1200/600 us, 0 = 600/600 us, gap a 45 ms |
| `RC5` | 13 bit Manchester, T = 889 us, 14 bit fisici, gap a 113.778 ms |
| `RC5X` | 7 bit di comando tramite field bit, 14 bit fisici |
| `RC6` | mode 0, 20 bit, header 2664/888 us, trailer bit doppio, gap 83.028 ms |
| `LG`, `LG2` | 28 bit, header 8500/4250 us, bit 550/1600 us (1) e 550/550 us (0) |
| `JVC` | 16 bit, header 8400/4200 us, bit 525/1725 us (1) e 525/525 us (0) |
| `Kaseikyo`, `Panasonic` | 48 bit, header 3456/1728 us, bit 432/1296 us (1) e 432/432 us (0), gap a 163.296 ms |
| `RCA` | 24 bit LSB-first, header 4000/4000 us, bit 500/2000 us (1) e 500/1000 us (0) |
| `raw` | durate del file, fino a 512 valori per comando |

Non supportati (mostrati come `[n/d]`, nessun invio): `NEC42`, `Samsung36`,
`Pioneer` e qualunque nome non presente nella tabella. La conversione non
inventa frame a lunghezza diversa.

## Invio

- RMT TX: `rmt_new_tx_channel` su GPIO44, risoluzione 1 MHz,
  `mem_block_symbols = 64`, `trans_queue_depth = 4`.
- Portante: **38 kHz, duty 33%**, livello alto, modulazione attiva.
- `rmt_new_copy_encoder` + `rmt_transmit` + `rmt_tx_wait_all_done`.
- Il frame viene costruito in `rmt_symbol_word_t[]` (max 512 simboli). Le
  durate oltre 32767 us vengono spezzate in piu' simboli; l'ultimo simbolo ha
  `duration1 = 0`.
- Ogni pressione invia **3 frame** (1 + 2 ripetizioni, default 2.x). Il canale
  RMT viene creato al primo invio e rilasciato all'uscita dal menu.

Tasti nel menu comandi:

- `ENTER` invia il comando selezionato.
- `P` power, `M` mute, `+`/`=` volume su, `-` volume giu', `C` canale su,
  `X` canale giu' (invio immediato).
- `F` aggiunge/rimuove il telecomando dai preferiti.
- `ESC` torna indietro.

I comandi non supportati sono disabilitati e non trasmettono. I nomi vengono
confrontati senza distinzione maiuscole/minuscole.

## Preferenze NVS

Namespace `orbit-ir`:

- `last`: path completo dell'ultimo telecomando aperto. Se il file esiste
  ancora, il menu principale mostra `Riprendi <marca/modello>`.
- `fav`: path dei preferiti separati da `\n` (massimo 8). I path mancanti
  restano in NVS e ricompaiono quando la SD viene reinserita.

Se NVS non e' disponibile il modulo funziona lo stesso, senza persistenza.

## Jammer IR

Voce **Jammer IR** nel menu principale. Prima di trasmettere chiama
`ui::legalWarning("Jammer IR", ...)`; se l'utente rifiuta, non parte nulla.
Trasmette frame NEC casuali per **30 secondi** o fino a `ESC`, mostrando
pacchetti e tempo residuo. Se almeno un frame e' stato inviato assegna i punti
mascot dell'evento `IrJammer`. E' una funzione offensiva: usarla solo su
dispositivi propri e in ambiente isolato.

## Limiti noti

- Il ricevitore IR non e' popolato: **nessun learn/RX**. La voce non esiste.
- La portante e' fissa a 38 kHz anche per Sony (nominale 40 kHz) e RC5/RC6
  (nominale 36 kHz). Le durate dei bit restano corrette; la portante e' quella
  richiesta dal firmware 3.0.
- `frequency:` e `duty_cycle:` dei file raw vengono accettati ma non usati.
- File `.ir` massimo 16 KB, massimo 64 comandi visualizzati, massimo 512
  valori per comando raw e 4096 valori raw complessivi per file. Oltre il
  limite il comando diventa `[raw >512]` o `[n/d]`.
- Prima del parsing servono almeno 56 KB di heap libero; altrimenti compare
  "Memoria insufficiente" invece di rischiare un abort.
- Un frame raw la cui somma supera 2 secondi viene rifiutato per non bloccare
  il menu. Il frame RMT e' limitato a 512 simboli.
- La ricerca e' una sottostringa ASCII case-insensitive su marca e nome file;
  non usa i metadati dei tasti.
- I preferiti sono al massimo 8 e non hanno un editor: si aggiungono con `F`.
- Nessun test su hardware reale in questa sessione: il parser e i tempi sono
  stati verificati con un test host sui file `.ir` del repository; la
  trasmissione RMT e' verificata solo a livello di API/compilazione.
- I file oltre 16 KB (es. `Panasonic/TH-42PCZ85U.ir`, 39 KB) vengono rifiutati
  con un messaggio esplicito.
