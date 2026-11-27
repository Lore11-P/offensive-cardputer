#pragma once

#include <cstddef>
#include <cstdint>

namespace orbit_hal {

struct KeyEvent {
    bool pressed = false;
    char key = 0;       // carattere base; '\n' Invio, '\b' Backspace, 0 = speciale
    uint8_t row = 0;    // riga logica 0..3
    uint8_t col = 0;    // colonna logica 0..13
};

bool keyboardBegin();
// Legge un evento dal TCA8418. Ritorna false se il buffer e' vuoto.
bool keyboardPoll(KeyEvent* out);
// Dump dei registri per la diagnostica (cfg, event count, matrice, int).
void keyboardDebug(char* out, size_t len);

// Bus I2C interno condiviso (SDA 8, SCL 9): usato da tastiera, codec audio
// ES8311, IMU e touch. Richiede keyboardBegin() gia' eseguito.
bool i2cPresent(uint8_t address);
bool i2cWrite(uint8_t address, const uint8_t* data, size_t len);
bool i2cWriteReg(uint8_t address, uint8_t reg, uint8_t value);
bool i2cReadReg(uint8_t address, uint8_t reg, uint8_t* value);

}  // namespace orbit_hal
