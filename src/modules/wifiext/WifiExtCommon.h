#pragma once

#include <cstdint>
#include <string>

// Helper condivisi dai tre strumenti WiFi+ (Responder, Karma, Auto-Portal).
namespace wifiext {

std::string shorten(const std::string& text, size_t maxLen);
std::string trim(const std::string& text);
std::string hexBytes(const uint8_t* data, size_t len);

// Spazio libero sulla microSD, stringa breve per la UI ("SD assente" se non c'e').
std::string sdFreeText();

// Crea /wifiext sulla SD (no-op se esiste). False se la SD non e' montata.
bool ensureDir();

// Aggiunge testo in coda a un file sulla SD, creandolo se assente.
bool appendSd(const char* path, const std::string& text);

}  // namespace wifiext
