# 09 — WiFi+: Responder, Karma, Auto-Portal

## Obiettivo
Aggiungere le funzioni WiFi offensive di Bruce che non richiedono hardware.

## Requisiti
- Modulo `src/modules/wifiext/` con menu:
  - Responder: risponde a LLMNR (5355), NBT-NS (137), mDNS (5353) e cattura
    risposte NTLM/NTLMv2 in `/wifiext/ntlm.txt` (hashcat 5600).
  - Karma: AP mode + risposta ai probe request con SSID dalla lista
    `/wifiext/karma.txt` (o SSID noti), senza deauth.
  - Auto-Portal: AP con DNS captive e HTTP che mostra una pagina di login
    personalizzabile da `/wifiext/portal.html`; credenziali in
    `/wifiext/portal.csv` solo su SD, cancellabili.
- `ui::legalWarning` + `ENABLE_OFFENSIVE` per ogni trasmissione; nessun TX
  se flag 0. Avviso "solo reti proprie".
- Stop pulito: HTTP/DNS server, AP, sockets, WiFi mode.
- UI con stato client, richieste, hash catturati, spazio SD.

## Accettazione
- Build OK.
- Responder risponde a una query LLMNR di test e logga l'hash.
- Auto-Portal mostra la pagina e salva una credenziale fittizia.
- Karma avvia l'AP e risponde ai probe (test in lab).
- `REPORT.md` con protocolli, formati file, limiti, dimensione firmware.

## Attenzione
- Non toccare `main.cpp`, `platformio.ini`, `ModuleFlags.h`, `WifiMenu.cpp`.
- Non usare credenziali reali nei test.
