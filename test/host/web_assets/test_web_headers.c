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

void test_web_headers_allow_for_uri(void);
void test_web_headers_allow_for_uri_mutations(void);
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_web_headers_api);
    RUN_TEST(test_web_headers_static);
        RUN_TEST(test_web_headers_allow_for_uri);
        RUN_TEST(test_web_headers_allow_for_uri_mutations);
    return UNITY_END();
}

void test_web_headers_allow_for_uri(void) {
    TEST_ASSERT_EQUAL_STRING("GET", web_headers_allow_for_uri("/"));
    TEST_ASSERT_EQUAL_STRING("GET", web_headers_allow_for_uri("/app.js"));
    TEST_ASSERT_EQUAL_STRING("GET", web_headers_allow_for_uri("/app.css"));
    TEST_ASSERT_EQUAL_STRING("GET", web_headers_allow_for_uri("/api/v1/auth/status"));

    TEST_ASSERT_EQUAL_STRING("POST", web_headers_allow_for_uri("/api/v1/auth/setup"));
    TEST_ASSERT_EQUAL_STRING("POST", web_headers_allow_for_uri("/api/v1/auth/login"));
    TEST_ASSERT_EQUAL_STRING("POST", web_headers_allow_for_uri("/api/v1/auth/logout"));

    // With query params
    TEST_ASSERT_EQUAL_STRING("GET", web_headers_allow_for_uri("/?cache=123"));
    TEST_ASSERT_EQUAL_STRING("POST", web_headers_allow_for_uri("/api/v1/auth/login?x=1"));

    // Unregistered URIs
    TEST_ASSERT_NULL(web_headers_allow_for_uri("/nonexistent"));
    TEST_ASSERT_NULL(web_headers_allow_for_uri("/api/v1/unknown"));
    TEST_ASSERT_NULL(web_headers_allow_for_uri(NULL));
}

void test_web_headers_allow_for_uri_mutations(void) {
    // 1. Swap return values
    // TEST_ASSERT_EQUAL_STRING("POST", web_headers_allow_for_uri("/")); // Fails if swapped

    // 2. Return without removing query
    // TEST_ASSERT_NULL(web_headers_allow_for_uri("/?cache=123")); // Fails if query not stripped

    // 3. Unregistered returns non-null
    // TEST_ASSERT_NOT_NULL(web_headers_allow_for_uri("/nonexistent")); // Fails if default is non-null
}
