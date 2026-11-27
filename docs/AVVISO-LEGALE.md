# OrbitOS 3.0 — avviso legale e uso delle funzioni offensive

OrbitOS 3.0 include, come la 2.x, funzioni di trasmissione radio e infrarossa
che possono interferire con reti e dispositivi altrui:

- deauthentication Wi-Fi (target e flood);
- beacon spam Wi-Fi;
- evil twin / captive portal;
- BLE spam (Apple, Samsung, Google, Windows);
- jammer IR.

**Queste funzioni sono pericolose e sono lecite solo se usate su reti e
dispositivi di tua proprietà, oppure in un ambiente RF isolato con
autorizzazione scritta.** Disturbare reti altrui, intercettare credenziali o
spacciarsi per un altro access point è reato in Italia e in molti altri Paesi.

Nel firmware ogni azione offensiva mostra **prima** una schermata di avviso
(`Avviso legale`, con ENTER = conferma e ESC = annulla). Se rifiuti, l'azione
non parte. Non esiste un modo per disattivare l'avviso dal menu.

Regole pratiche:

1. Usa solo apparecchi tuoi o reti di cui hai la disponibilità.
2. Non lasciare in onda deauth/beacon/jammer in ambienti condivisi.
3. Le credenziali del captive portal restano solo in RAM e non vengono
   scritte su SD/NVS; non usarle su persone terze.
4. Le funzioni passive (scanner Wi-Fi/BLE, analisi canali) non trasmettono e
   sono sempre disponibili.

Il progetto è fornito così com'è, senza garanzie. L'autore non si assume
responsabilità per usi impropri.
