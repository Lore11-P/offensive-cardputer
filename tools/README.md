# Strumenti copertina Cardputer

`cover_to_cardputer.ps1` converte una copertina album nel formato letto dal player
del firmware e la copia direttamente sulla microSD.

Requisiti: Windows PowerShell 5.1 e .NET `System.Drawing` (inclusi in Windows).
Nessuna dipendenza esterna.

## Uso

```powershell
.\tools\cover_to_cardputer.ps1 -Image "C:\covers\dark_side.jpg" -Album "The Dark Side of the Moon"
```

Scrive in `E:\data\Music\The Dark Side of the Moon\cover.cvr`, creando la cartella
album se non esiste.

| Parametro | Obbligatorio | Descrizione |
|-----------|--------------|-------------|
| `-Image`  | sì | Immagine sorgente: jpg, jpeg, png, bmp, gif. |
| `-Album`  | no | Nome della cartella album sotto `data\Music`. Se omesso, viene chiesto. |
| `-OutDir` | no | Cartella base alternativa. Default: `E:\data\Music`. |
| `-Force`  | no | Sovrascrive `cover.cvr` senza chiedere conferma. |

Esempi:

```powershell
# Album con spazi nel nome
.\tools\cover_to_cardputer.ps1 -Image ".\cover.png" -Album "The Dark Side of the Moon"

# Sovrascrittura senza prompt
.\tools\cover_to_cardputer.ps1 -Image ".\cover.jpg" -Album "The Dark Side of the Moon" -Force

# Prova su cartella locale, senza SD
.\tools\cover_to_cardputer.ps1 -Image ".\cover.jpg" -Album "Prova" -OutDir "C:\temp\music"
```

L'immagine viene ritagliata al centro (quadrato) e ridimensionata a 64x64 senza
distorsioni. Le immagini con trasparenza (png/gif) vengono composte su sfondo bianco.

## Formato `cover.cvr`

| Campo | Valore |
|-------|--------|
| Dimensioni | 64x64 pixel |
| Formato pixel | RGB565 |
| Byte per pixel | 2 |
| Endianness | little-endian (byte basso, poi alto) |
| Dimensione file | 8192 byte esatti (4096 px × 2) |
| Percorso | `E:\data\Music\<Album>\cover.cvr` |

Conversione: `r>>3`, `g>>2`, `b>>3`, con i 5 bit di rosso in alto, 6 di verde
al centro e 5 di blu in basso. Lo script stampa anche il primo pixel in hex e
ricontrolla la dimensione del file scritto.

## Se la SD non è montata

Con `-OutDir` assente lo script usa `E:\data\Music` e, se `E:` non esiste, si
ferma con un messaggio chiaro senza creare cartelle relative.

In quel caso:

1. inserisci la microSD e riprova, oppure
2. passa una cartella esplicita con `-OutDir`:
   ```powershell
   .\tools\cover_to_cardputer.ps1 -Image ".\cover.jpg" -Album "Prova" -OutDir "C:\temp\music"
   ```

## Avvertenza

La copertina deve stare **nella cartella dell'album**, non direttamente in
`data\Music`: il player del firmware cerca `cover.cvr` accanto ai brani
(`E:\data\Music\<Album>\cover.cvr`). Se i brani sono in un'altra cartella, sposta
la copertina lì oppure indica l'album corretto.

## Limiti

- GIF: viene usato solo il primo frame.
- Immagini più piccole di 64x64 sono rifiutate (niente upscaling).
- Il nome file è fisso: `cover.cvr`.
- Lo script non verifica la presenza del firmware né la versione: il contratto
  è il percorso e il formato descritti sopra.
