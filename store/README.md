# Store giochi

Catalogo dei giochi Wren scaricabili da ORBIT OS. Il device legge
`store/games.json` dal ramo `main` e salva i giochi in `/games/<id>.wren`
sulla microSD.

## Aggiungere un gioco

1. Crea `store/games/<id>.wren`. L'`id` diventa il nome del file sul device:
   solo caratteri `[a-z0-9_-]`, massimo 24 caratteri.
2. Metti i metadati nelle prime righe, come commenti:

   ```wren
   // name: Snake
   // desc: Mangia e cresci senza morderti la coda.
   // version: 1.0
   ```

   `name` e `desc` compaiono nel menu dello store; `version` e' opzionale
   (default `1.0`).
3. Usa solo Wren core e il preludio `Game` (niente `import`, niente
   operatori bitwise). Il file deve restare sotto 64 KB. Wren non ha
   dichiarazioni `fun`: le funzioni sono closure, ad esempio
   `var tick = Fn.new { ... }`, richiamate con `tick.call()`.
4. Rigenera il catalogo e verifica che compaia nell'elenco:

   ```sh
   python tools/build_store.py
   ```

## Catalogo

`tools/build_store.py` scansiona `store/games/*.wren`, calcola `size` e
`sha256` e riscrive `store/games.json` ordinato per nome, con indentazione 2
e newline finale. Non modificare `games.json` a mano.

Il device accetta al massimo 24 giochi e un catalogo da 32 KB: oltre quella
soglia le voci in eccesso vengono ignorate o il catalogo viene scartato.

## Pubblicare

1. Commit e push su `main` di `store/games.json` e `store/games/*.wren`.
2. Verifica che l'URL raw risponda, ad esempio:
   `https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games.json`.
3. Al primo avvio dello store il device scarica il catalogo; al download di un
   gioco verifica la dimensione e lo **SHA-256** prima di scrivere il file.
   Se pubblichi un gioco modificato, rigenera il catalogo: senza nuovo hash il
   device rifiuta il file.
