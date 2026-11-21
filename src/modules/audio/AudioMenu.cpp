#include "AudioMenu.h"

#include "Display.h"
#include "ModuleFlags.h"
#include "Ui.h"

#if ENABLE_MODULE_AUDIO

namespace audio {

void runMenu() {
    ui::message("Audio", {"Microfono: spettro, registratore e voice memo.", "Modulo in integrazione."}, "Menu principale");
}

}  // namespace audio

#endif  // ENABLE_MODULE_AUDIO
