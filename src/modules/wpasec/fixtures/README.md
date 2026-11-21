# Fixture WPA-SEC

Dati inventati, nessuna credenziale o cattura reale.

- `potfile_colon.txt`: formato restituito dal servizio reale,
  `bssid:station_mac:ssid:password`. Il secondo record ha due punti nell'SSID
  per verificare che la password venga presa come ultimo campo.
- `potfile_wpa.txt`: stessa rete in formato `WPA*...:password`, accettato per
  compatibilita'.
- `capture_sample.hc22000`: hash PMKID con BSSID `020000000001`, lo stesso dei
  due potfile. Non e' una cattura reale e non va caricata sul servizio.

## Test locale

1. Copiare `capture_sample.hc22000` in `/captures/`.
2. Copiare `potfile_colon.txt` in `/wpasec/potfile.txt`.
3. Aprire `WPA-SEC > Elenco catture`: il file deve mostrare `PW` e il dettaglio
   deve mostrare `test-password`.
4. Ripetere con `potfile_wpa.txt`: stesso risultato.
5. Con un potfile di soli commenti o vuoto, nessun file deve risultare `PW`.
