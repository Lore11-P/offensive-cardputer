#include <unity.h>

#include <string>

#include "UsbLogic.h"

void setUp() {}
void tearDown() {}

void test_crc32_known_vector() {
    const std::string data = "123456789";
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u,
                             usb::crc32(reinterpret_cast<const uint8_t*>(data.data()),
                                        data.size()));
}

void test_crc32_chained() {
    const std::string a = "1234";
    const std::string b = "56789";
    const uint32_t first =
        usb::crc32(reinterpret_cast<const uint8_t*>(a.data()), a.size());
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u,
                             usb::crc32(reinterpret_cast<const uint8_t*>(b.data()),
                                        b.size(), first));
}

void test_sanitize_path_valid() {
    std::string out;
    TEST_ASSERT_TRUE(usb::sanitizePath("ir/tv/a.ir", out));
    TEST_ASSERT_EQUAL_STRING("ir/tv/a.ir", out.c_str());
}

void test_sanitize_path_leading_slash() {
    std::string out;
    TEST_ASSERT_TRUE(usb::sanitizePath("/ir/a.ir", out));
    TEST_ASSERT_EQUAL_STRING("ir/a.ir", out.c_str());
}

void test_sanitize_path_invalid() {
    std::string out;
    TEST_ASSERT_FALSE(usb::sanitizePath("..\\x", out));
    TEST_ASSERT_TRUE(out.empty());
    TEST_ASSERT_FALSE(usb::sanitizePath("a//b", out));
    TEST_ASSERT_FALSE(usb::sanitizePath("", out));
    TEST_ASSERT_FALSE(usb::sanitizePath("C:\\x", out));
    TEST_ASSERT_FALSE(usb::sanitizePath(std::string(161, 'a'), out));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_known_vector);
    RUN_TEST(test_crc32_chained);
    RUN_TEST(test_sanitize_path_valid);
    RUN_TEST(test_sanitize_path_leading_slash);
    RUN_TEST(test_sanitize_path_invalid);
    return UNITY_END();
}
