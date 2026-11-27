#pragma once

// Console diagnostica su USB seriale (COM5). Attiva solo con
// -DORBIT_DIAG_CONSOLE=1: legge comandi riga per riga e risponde con [DIAG].
// Comandi: help, heap, fb [rel|res], games, run <nome>, stop, tasks, wifi, reset.
namespace diagconsole {

#ifndef ORBIT_DIAG_CONSOLE
#define ORBIT_DIAG_CONSOLE 0
#endif

#if ORBIT_DIAG_CONSOLE
void begin();
// Libera il task console (4 KB di stack) prima di un gioco: durante il gioco
// il gate deve vedere la stessa memoria del firmware di produzione.
void pause();
void resume();
#else
inline void begin() {}
inline void pause() {}
inline void resume() {}
#endif

}  // namespace diagconsole
