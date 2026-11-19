#pragma once

namespace music {

// Lettore MP3/WAV/FLAC/OGG/M4A dagli album in /data/Music della SD
// (/data/Music/<Album>/<brano>). I file sciolti nella root compaiono come
// "Senza album"; la cover opzionale e' <Album>/cover.cvr (64x64 RGB565).
// Solo riproduzione di file locali: i brani vanno copiati sulla microSD dal PC.
void runMenu();

}  // namespace music
