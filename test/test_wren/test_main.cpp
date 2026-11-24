#include <unity.h>

#include "wren.h"

void setUp() {}
void tearDown() {}

// Prova che il Wren vendorizzato compili e linki anche su host.
void test_wren_print() {
    WrenVM* vm = wrenNewVM(nullptr);
    TEST_ASSERT_NOT_NULL(vm);
    const WrenInterpretResult result = wrenInterpret(vm, "main", "System.print(\"ok\")");
    wrenFreeVM(vm);
    TEST_ASSERT_EQUAL_INT(WREN_RESULT_SUCCESS, static_cast<int>(result));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_wren_print);
    return UNITY_END();
}
