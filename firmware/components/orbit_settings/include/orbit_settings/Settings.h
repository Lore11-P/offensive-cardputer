#pragma once

#include <cstdint>
#include <string>

namespace orbit_settings {

void begin();
uint8_t brightness();
uint8_t theme();
uint8_t rotation();
std::string nickname();
void setBrightness(uint8_t value);
void setTheme(uint8_t id);
void setRotation(uint8_t index);
void setNickname(const std::string& value);

}  // namespace orbit_settings
