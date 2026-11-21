# 10 — ESP-NOW: file, comandi, clipboard mesh

## Obiettivo
Comunicazione diretta tra Cardputer senza router.

## Requisiti
- Modulo `src/modules/espnow/` con menu:
  - Invia file: scegli file SD, trasferimento a chunk con magic/versione/tipo,
    seq, CRC32, ACK e timeout; destinatario selezionabile o broadcast.
  - Inbox: file ricevuti in `/espnow/inbox/`, elenco, integrità, cancellazione.
  - Comandi: testo broadcast (`/espnow/commands.txt` storico), esecuzione
    solo di comandi whitelisted (es. `beep`, `led`, `msg`) per sicurezza.
  - Clipboard: testo condiviso tra peer, ultimo valore in NVS.
  - Peer: scan ESP-NOW, nome, MAC, RSSI, ultimo seen.
- Canale configurabile (default 1), pairing con nome; nessun router.
- Buffer piccoli, trasferimento a blocchi da ~200 byte, max file 512 KB.
- Rilasciare WiFi/ESP-NOW all'uscita e ripristinare lo stato.

## Accettazione
- Build OK.
- Due Cardputer (o simulatori) si vedono e si scambiano un file da 10 KB.
- CRC e ACK funzionano; file corrotto rifiutato.
- I comandi non whitelisted vengono ignorati.
- `REPORT.md` con protocollo, comandi, limiti, dimensione firmware.

## Attenzione
- Non toccare `main.cpp`, `platformio.ini`, `ModuleFlags.h`.
- Nessuna esecuzione arbitraria di stringhe ricevute.
