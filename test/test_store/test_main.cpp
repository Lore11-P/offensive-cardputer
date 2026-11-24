#include <unity.h>

#include <string>

#include "StoreLogic.h"

void setUp() {}
void tearDown() {}

void test_valid_id() {
    TEST_ASSERT_TRUE(store::validId("snake"));
    TEST_ASSERT_TRUE(store::validId("game-1_x"));
    TEST_ASSERT_TRUE(store::validId(std::string(24, 'a')));
}

void test_invalid_id() {
    TEST_ASSERT_FALSE(store::validId(""));
    TEST_ASSERT_FALSE(store::validId("Snake"));
    TEST_ASSERT_FALSE(store::validId("a/b"));
    TEST_ASSERT_FALSE(store::validId("../x"));
    TEST_ASSERT_FALSE(store::validId("a b"));
    TEST_ASSERT_FALSE(store::validId(std::string(25, 'a')));
}

void test_sha256_hex() {
    TEST_ASSERT_TRUE(store::isSha256Hex(std::string(64, 'a')));
    TEST_ASSERT_TRUE(
        store::isSha256Hex("94357260fe8ed7fc19a20c2474864ba69432b8cd86a7d82c4c724ba368211573"));

    TEST_ASSERT_FALSE(store::isSha256Hex(std::string(64, 'A')));
    TEST_ASSERT_FALSE(store::isSha256Hex(std::string(63, 'a')));
    TEST_ASSERT_FALSE(store::isSha256Hex(std::string(65, 'a')));
    TEST_ASSERT_FALSE(store::isSha256Hex(std::string(64, 'g')));
}

void test_game_path() {
    TEST_ASSERT_EQUAL_STRING("/games/snake.wren", store::gamePath("snake").c_str());
    TEST_ASSERT_EQUAL_STRING("", store::gamePath("../x").c_str());
}

void test_rgb565() {
    TEST_ASSERT_EQUAL_UINT16(0xF800, store::rgb565(255, 0, 0));
    TEST_ASSERT_EQUAL_UINT16(0x07E0, store::rgb565(0, 255, 0));
    TEST_ASSERT_EQUAL_UINT16(0x001F, store::rgb565(0, 0, 255));
    TEST_ASSERT_EQUAL_UINT16(0x0000, store::rgb565(0, 0, 0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_id);
    RUN_TEST(test_invalid_id);
    RUN_TEST(test_sha256_hex);
    RUN_TEST(test_game_path);
    RUN_TEST(test_rgb565);
    return UNITY_END();
}
