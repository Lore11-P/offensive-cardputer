// Test host dei pezzi portabili del componente:
//   g++ -std=c++17 -I src tests/host_test.cpp -o host_test
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Base32.h"
#include "CalcExpr.h"

namespace {

void expectCalc(const char* expression, double want) {
    double value = 0.0;
    bool divZero = false;
    const bool ok = orbit_tools::calc::evaluate(expression, value, &divZero);
    if (!ok || divZero || std::fabs(value - want) > 1e-9) {
        std::printf("calc fail: %s -> %g (ok=%d div0=%d)\n", expression, value, ok, divZero);
        std::abort();
    }
}

void expectDivZero(const char* expression) {
    double value = 0.0;
    bool divZero = false;
    const bool ok = orbit_tools::calc::evaluate(expression, value, &divZero);
    if (ok || !divZero || !std::isfinite(value)) {
        std::printf("calc div0 fail: %s (ok=%d div0=%d)\n", expression, ok, divZero);
        std::abort();
    }
}

void expectInvalid(const char* expression) {
    double value = 0.0;
    bool divZero = false;
    if (orbit_tools::calc::evaluate(expression, value, &divZero)) {
        std::printf("calc should fail: %s -> %g\n", expression, value);
        std::abort();
    }
}

}  // namespace

int main() {
    expectCalc("(1+2)*3", 9);
    expectCalc("10/4", 2.5);
    expectCalc("2+3*4", 14);
    expectCalc("50%7", 1);
    expectCalc("(2+3)*(4-1)", 15);
    expectCalc("-3*-4", 12);
    expectCalc("100/10/2", 5);
    expectCalc("2*(3+4)-5", 9);
    expectCalc("1.5+2.25", 3.75);
    expectCalc("((2+3)*2)/(4-1)", 10.0 / 3.0);
    expectInvalid("");
    expectInvalid("1+");
    expectInvalid("(1+2");
    expectInvalid("2^3");
    expectInvalid("abc");
    expectInvalid("1..2");
    expectDivZero("1/0");
    expectDivZero("5%0");
    expectDivZero("1/(2-2)");

    uint8_t key[64] = {};
    size_t length = 0;
    const char* rfc = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    assert(orbit_tools::base32Decode(rfc, key, sizeof(key), length));
    assert(length == 20);
    assert(std::memcmp(key, "12345678901234567890", 20) == 0);

    std::memset(key, 0, sizeof(key));
    assert(orbit_tools::base32Decode("gezd gnbv-gy3t qojq gezd gnbv gy3t qojq", key,
                                     sizeof(key), length));
    assert(length == 20 && std::memcmp(key, "12345678901234567890", 20) == 0);

    std::memset(key, 0, sizeof(key));
    assert(orbit_tools::base32Decode("MY======", key, sizeof(key), length));
    assert(length == 1 && key[0] == 'f');

    assert(!orbit_tools::base32Decode("", key, sizeof(key), length));
    assert(!orbit_tools::base32Decode("GEZD1", key, sizeof(key), length));
    assert(!orbit_tools::base32Decode("MY==AA", key, sizeof(key), length));
    assert(!orbit_tools::base32Decode("A", key, sizeof(key), length));
    assert(!orbit_tools::base32Decode("GEZDGNBV", key, 1, length));
    assert(orbit_tools::base32Valid(rfc));

    std::puts("host_test ok");
    return 0;
}
