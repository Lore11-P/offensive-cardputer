#pragma once

namespace orbit_hal {

// Tensione batteria in millivolt (GPIO10, partitore 1:2) e percentuale
// stimata 3,3 V = 0% .. 4,2 V = 100%. -1 se l'ADC non risponde.
int batteryMillivolts();
int batteryPercent();

}  // namespace orbit_hal
