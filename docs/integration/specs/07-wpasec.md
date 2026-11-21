# 07 — WPA-SEC upload

## Obiettivo
Caricare le catture `.hc22000` su WPA-SEC e scaricare i risultati.

## Requisiti
- Modulo `src/modules/wpasec/` con menu: Imposta chiave, Carica catture,
  Scarica potfile, Stato, Info.
- Chiave in `/wpasec/key.txt` (32 hex), mai in log; opzione cancellazione.
- Elenco `/captures/*.hc22000` e `.pcap` con dimensione e stato upload
  (`/wpasec/uploaded.txt`).
- Upload HTTPS a `https://wpa-sec.stanev.org/?submit&key=...` come
  multipart/form-data; gestire timeout, redirect e risposta.
- Download potfile e salvataggio `/wpasec/potfile.txt`; evidenziare i file
  che hanno una password nel potfile.
- TLS con `WiFiClientSecure`; se il cert bundle non entra, `setInsecure()`
  con avviso chiaro all'utente.
- Heap gate: richiedere ~40 KB liberi e blocco contiguo; messaggio se insufficiente.

## Accettazione
- Build OK.
- Con chiave valida, upload di un file di test e risposta HTTP gestita.
- Il potfile viene salvato e mostrato.
- `REPORT.md` con endpoint, formato richiesta, limiti TLS, dimensione firmware.

## Attenzione
- Non toccare `main.cpp`, `platformio.ini`, `ModuleFlags.h`.
- Non inviare dati senza conferma esplicita.
