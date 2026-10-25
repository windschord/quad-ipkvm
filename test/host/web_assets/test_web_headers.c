#include "unity.h"
#include "web_headers.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

void setUp(void) {}
void tearDown(void) {}

static bool has_duplicate_headers(const web_header_t *headers, size_t count) {
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            // Case-insensitive comparison
            bool match = true;
            if (strlen(headers[i].name) != strlen(headers[j].name)) continue;
            for (size_t k = 0; k < strlen(headers[i].name); k++) {
                if (tolower((unsigned char)headers[i].name[k]) != tolower((unsigned char)headers[j].name[k])) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
    }
    return false;
}

static int count_header(const web_header_t *headers, size_t count, const char *name) {
    int c = 0;
    for (size_t i = 0; i < count; i++) {
        bool match = true;
        if (strlen(headers[i].name) != strlen(name)) continue;
        for (size_t k = 0; k < strlen(name); k++) {
            if (tolower((unsigned char)headers[i].name[k]) != tolower((unsigned char)name[k])) {
                match = false;
                break;
            }
        }
        if (match) c++;
    }
    return c;
}

static const char* get_header_value(const web_header_t *headers, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        bool match = true;
        if (strlen(headers[i].name) != strlen(name)) continue;
        for (size_t k = 0; k < strlen(name); k++) {
            if (tolower((unsigned char)headers[i].name[k]) != tolower((unsigned char)name[k])) {
                match = false;
                break;
            }
        }
        if (match) return headers[i].value;
    }
    return NULL;
}

void test_web_headers_api(void) {
    web_header_t headers[8];
    size_t count = web_headers_get_api(headers, 8);

    TEST_ASSERT_EQUAL(4, count);
    TEST_ASSERT_FALSE(has_duplicate_headers(headers, count));

    TEST_ASSERT_EQUAL(1, count_header(headers, count, "Content-Security-Policy"));
    TEST_ASSERT_EQUAL(1, count_header(headers, count, "X-Content-Type-Options"));
    TEST_ASSERT_EQUAL(1, count_header(headers, count, "Cache-Control"));
    TEST_ASSERT_EQUAL(1, count_header(headers, count, "Referrer-Policy"));

    TEST_ASSERT_EQUAL_STRING("default-src 'none'; frame-ancestors 'none'", get_header_value(headers, count, "Content-Security-Policy"));
    TEST_ASSERT_EQUAL_STRING("nosniff", get_header_value(headers, count, "X-Content-Type-Options"));
    TEST_ASSERT_EQUAL_STRING("no-store", get_header_value(headers, count, "Cache-Control"));
    TEST_ASSERT_EQUAL_STRING("no-referrer", get_header_value(headers, count, "Referrer-Policy"));

    // Check static pointers
    web_header_t headers2[8];
    size_t count2 = web_headers_get_api(headers2, 8);
    TEST_ASSERT_EQUAL_PTR(get_header_value(headers, count, "Content-Security-Policy"), get_header_value(headers2, count2, "Content-Security-Policy"));
}

void test_web_headers_static(void) {
    const char *types[] = {"text/html", "application/javascript", "text/css", NULL};

    for (int i = 0; i < 4; i++) {
        web_header_t headers[8];
        size_t count = web_headers_get_static(types[i], headers, 8);

        TEST_ASSERT_EQUAL(4, count);
        TEST_ASSERT_FALSE(has_duplicate_headers(headers, count));

        TEST_ASSERT_EQUAL(1, count_header(headers, count, "Content-Security-Policy"));
        TEST_ASSERT_EQUAL(1, count_header(headers, count, "X-Content-Type-Options"));
        TEST_ASSERT_EQUAL(1, count_header(headers, count, "Cache-Control"));
        TEST_ASSERT_EQUAL(1, count_header(headers, count, "Referrer-Policy"));

        const char *csp = get_header_value(headers, count, "Content-Security-Policy");
        if (types[i] && strcmp(types[i], "text/html") == 0) {
            TEST_ASSERT_NOT_NULL(strstr(csp, "script-src 'self'"));
            TEST_ASSERT_NOT_NULL(strstr(csp, "style-src 'self'"));
            TEST_ASSERT_NOT_NULL(strstr(csp, "connect-src 'self'"));
            TEST_ASSERT_NULL(strstr(csp, "unsafe-inline"));
            TEST_ASSERT_NULL(strstr(csp, "unsafe-eval"));
        } else {
            TEST_ASSERT_EQUAL_STRING("default-src 'none'; frame-ancestors 'none'", csp);
        }

        TEST_ASSERT_EQUAL_STRING("nosniff", get_header_value(headers, count, "X-Content-Type-Options"));
        TEST_ASSERT_EQUAL_STRING("no-store", get_header_value(headers, count, "Cache-Control"));
        TEST_ASSERT_EQUAL_STRING("no-referrer", get_header_value(headers, count, "Referrer-Policy"));

        // Check static pointers
        web_header_t headers2[8];
        size_t count2 = web_headers_get_static(types[i], headers2, 8);
        TEST_ASSERT_EQUAL_PTR(get_header_value(headers, count, "Content-Security-Policy"), get_header_value(headers2, count2, "Content-Security-Policy"));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_web_headers_api);
    RUN_TEST(test_web_headers_static);
    return UNITY_END();
}
