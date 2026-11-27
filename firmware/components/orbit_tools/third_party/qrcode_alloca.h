#pragma once

// Wrapper locale per alloca: NON chiamarlo alloca.h, altrimenti con -I
// third_party shadowa l'header di sistema e rompe LovyanGFX.
#if __has_include(<alloca.h>)
#include <alloca.h>
#else
#include <stdlib.h>
#include <malloc.h>
#ifndef alloca
#define alloca _alloca
#endif
#endif
