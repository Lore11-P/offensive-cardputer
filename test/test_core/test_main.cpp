#include <unity.h>

#include "GridMath.h"
#include "MascotLogic.h"
#include "NavCore.h"

using navigation::Event;
using navigation::EventData;
using navigation::KeyState;
using navigation::PendingKeys;

namespace {

KeyState keys(const char* word, bool enter = false, bool back = false) {
    KeyState state;
    while (word[state.count] != '\0' && state.count < navigation::kMaxKeys) {
        state.word[state.count] = word[state.count];
        ++state.count;
    }
    state.enter = enter;
    state.back = back;
    return state;
}

int typeOf(const EventData& ev) {
    return static_cast<int>(ev.type);
}

}  // namespace

void setUp() {}
void tearDown() {}

// Un carattere che arriva nello stesso campione di ENTER non si perde: ENTER
// viene consegnato ora, il carattere al poll successivo.
void test_char_with_enter_is_not_lost() {
    PendingKeys pending;
    const KeyState cur = keys("f", true);
    EventData ev = navigation::stepKeys(KeyState{}, cur, pending);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::Select), typeOf(ev));
    TEST_ASSERT_EQUAL_INT(1, pending.count);
    TEST_ASSERT_EQUAL_CHAR('f', pending.chars[0]);

    ev = navigation::stepKeys(cur, cur, pending);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::Text), typeOf(ev));
    TEST_ASSERT_EQUAL_CHAR('f', ev.ch);
    TEST_ASSERT_EQUAL_INT(0, pending.count);
}

// Due frecce nello stesso campione: la seconda resta in coda.
void test_two_arrows_same_sample() {
    PendingKeys pending;
    const KeyState cur = keys(";.");
    EventData ev = navigation::stepKeys(KeyState{}, cur, pending);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::Up), typeOf(ev));

    ev = navigation::stepKeys(cur, cur, pending);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::Down), typeOf(ev));
}

// Ultima riga -> prima riga della pagina successiva, stessa colonna.
void test_grid_page_wrap_forward() {
    const int next = gridmath::move(4, 8, 3, 2, 0, 1, [](int) { return true; });
    TEST_ASSERT_EQUAL_INT(7, next);
}

// Pagina finale parziale: il target viene clampato sull'ultima voce.
void test_grid_partial_last_page() {
    const int next = gridmath::move(4, 7, 3, 2, 0, 1, [](int) { return true; });
    TEST_ASSERT_EQUAL_INT(6, next);
    const int clamped = gridmath::move(6, 7, 3, 2, 1, 0, [](int) { return true; });
    TEST_ASSERT_EQUAL_INT(6, clamped);
}

// Tutte le voci disabilitate: la selezione non si muove.
void test_grid_all_disabled_keeps_selection() {
    const int next = gridmath::move(3, 8, 3, 2, 1, 0, [](int) { return false; });
    TEST_ASSERT_EQUAL_INT(3, next);
}

// ESC non deve finire in coda come carattere stampabile.
void test_escape_is_not_queued_as_text() {
    PendingKeys pending;
    const KeyState cur = keys("`", false, true);
    EventData ev = navigation::stepKeys(KeyState{}, cur, pending);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::Back), typeOf(ev));
    TEST_ASSERT_EQUAL_INT(0, pending.count);

    ev = navigation::stepKeys(cur, cur, pending);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::None), typeOf(ev));
}

// I punti sono fissi per evento; Count non vale nulla.
void test_mascot_points_for() {
    using mascot::Event;
    TEST_ASSERT_EQUAL_UINT16(10, mascot::pointsFor(Event::WifiDeauth));
    TEST_ASSERT_EQUAL_UINT16(15, mascot::pointsFor(Event::WifiBeacon));
    TEST_ASSERT_EQUAL_UINT16(25, mascot::pointsFor(Event::WifiDeauthFlood));
    TEST_ASSERT_EQUAL_UINT16(20, mascot::pointsFor(Event::EvilTwin));
    TEST_ASSERT_EQUAL_UINT16(10, mascot::pointsFor(Event::BleSpam));
    TEST_ASSERT_EQUAL_UINT16(5, mascot::pointsFor(Event::IrJammer));
    TEST_ASSERT_EQUAL_UINT16(15, mascot::pointsFor(Event::AirTagSpoof));
    TEST_ASSERT_EQUAL_UINT16(0, mascot::pointsFor(Event::Count));
}

// Ogni evento valido ha un'etichetta mostrabile.
void test_mascot_labels() {
    for (int i = 0; i < static_cast<int>(mascot::Event::Count); ++i) {
        const char* label = mascot::labelFor(static_cast<mascot::Event>(i));
        TEST_ASSERT_NOT_NULL(label);
        TEST_ASSERT_TRUE(label[0] != '\0');
    }
}

void test_mascot_clamp_affection() {
    TEST_ASSERT_EQUAL_UINT8(0, mascot::clampAffection(-5));
    TEST_ASSERT_EQUAL_UINT8(0, mascot::clampAffection(0));
    TEST_ASSERT_EQUAL_UINT8(100, mascot::clampAffection(100));
    TEST_ASSERT_EQUAL_UINT8(100, mascot::clampAffection(140));
}

void test_mascot_clamp_score() {
    TEST_ASSERT_EQUAL_UINT32(mascot::kMaxScore, mascot::clampScore(mascot::kMaxScore));
    TEST_ASSERT_EQUAL_UINT32(mascot::kMaxScore, mascot::clampScore(mascot::kMaxScore + 1ULL));
}

void test_mascot_affection_after_pet() {
    TEST_ASSERT_EQUAL_UINT8(2, mascot::affectionAfterPet(0));
    TEST_ASSERT_EQUAL_UINT8(100, mascot::affectionAfterPet(99));
}

void test_mascot_can_earn_pet_points() {
    TEST_ASSERT_TRUE(mascot::canEarnPetPoints(99));
    TEST_ASSERT_FALSE(mascot::canEarnPetPoints(100));
}

// Cooldown con sottrazione unsigned: il wrap di millis() non blocca la coccola.
void test_mascot_pet_ready() {
    TEST_ASSERT_TRUE(mascot::petReady(0, 0));
    TEST_ASSERT_FALSE(mascot::petReady(12999, 1000));
    TEST_ASSERT_TRUE(mascot::petReady(13000, 1000));
    TEST_ASSERT_TRUE(mascot::petReady(0x1EE0, 0xFFFFF000));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_char_with_enter_is_not_lost);
    RUN_TEST(test_two_arrows_same_sample);
    RUN_TEST(test_grid_page_wrap_forward);
    RUN_TEST(test_grid_partial_last_page);
    RUN_TEST(test_grid_all_disabled_keeps_selection);
    RUN_TEST(test_escape_is_not_queued_as_text);
    RUN_TEST(test_mascot_points_for);
    RUN_TEST(test_mascot_labels);
    RUN_TEST(test_mascot_clamp_affection);
    RUN_TEST(test_mascot_clamp_score);
    RUN_TEST(test_mascot_affection_after_pet);
    RUN_TEST(test_mascot_can_earn_pet_points);
    RUN_TEST(test_mascot_pet_ready);
    return UNITY_END();
}
