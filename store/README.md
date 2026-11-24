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
3. Usa solo Wren core e il preludio `Game` (niente `import`). Il file deve
   restare sotto 32 KB. Wren 0.4 non supporta gli operatori composti
   (`+=`, `-=`, `*=`, `/=`): scrivi `x = x + 1`. Non ha dichiarazioni `fun`:
   le funzioni sono closure, ad esempio `var tick = Fn.new { ... }`,
   richiamate con `tick.call()`.
4. Rigenera il catalogo, firmalo e verifica che compaia nell'elenco:

   ```sh
   python tools/build_store.py
   python tools/sign_store.py store/games.json
   ```

## Catalogo

`tools/build_store.py` scansiona `store/games/*.wren`, calcola `size` e
`sha256` e riscrive `store/games.json` ordinato per nome, con indentazione 2
e newline finale. Non modificare `games.json` a mano.

Il device accetta al massimo 24 giochi e un catalogo da 32 KB: oltre quella
soglia le voci in eccesso vengono ignorate o il catalogo viene scartato.

## Firma e modello di fiducia

Il catalogo non si autentica con lo SHA-256: chi puo' scrivere su `main`
potrebbe cambiare gioco e hash insieme. Per questo `store/games.json` viene
firmato con la **stessa chiave ECDSA P-256 dell'OTA** (la privata resta fuori
dal repository) e il device verifica `store/games.sig` prima di leggere il
JSON. TLS con CA pinnata protegge il transito; la firma protegge il publisher.

Dopo ogni modifica a giochi o catalogo:

```sh
python tools/build_store.py
python tools/sign_store.py store/games.json
```

I file `.wren` e `games.json` devono restare **LF** (vedi `.gitattributes`):
il catalogo calcola gli hash sui byte LF, mentre GitHub raw pubblica il blob
cosi' com'e'. Se pubblichi in altro modo, forza LF o il device rifiutera' i
download.

## Pubblicare

1. Commit e push su `main` di `store/games.json`, `store/games.sig` e
   `store/games/*.wren`.
2. Verifica che gli URL raw rispondano, ad esempio:
   `https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games.json`.
3. Al primo avvio dello store il device scarica catalogo e firma; al download
   di un gioco verifica dimensione e **SHA-256** prima di scrivere il file.
   Se pubblichi un gioco modificato, rigenera e rifirma il catalogo: senza
   nuovo hash il device rifiuta il file.
