#include "StoreMenu.h"

#include "Display.h"
#include "Ui.h"
#include "wren.h"

// Spike temporaneo: verifica che Wren compili e si linki sul target.
// L'implementazione vera (catalogo, download, sandbox) arriva con gli agenti.
namespace store {

void runMenu() {
    WrenConfiguration config;
    wrenInitConfiguration(&config);
    WrenVM* vm = wrenNewVM(&config);
    if (vm == nullptr) {
        ui::message("Store", {"!Wren VM non disponibile."}, "Menu principale");
        return;
    }
    const WrenInterpretResult result =
        wrenInterpret(vm, "main", "System.print(\"Wren OK\")");
    wrenFreeVM(vm);
    ui::message("Store", {result == WREN_RESULT_SUCCESS ? "Wren OK" : "!Wren errore"},
                "Menu principale");
}

}  // namespace store
