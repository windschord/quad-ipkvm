#include "unity.h"
#include "web_assets.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

void setUp(void) {}
void tearDown(void) {}

static const unsigned char dummy_data[] = {0x00};

static const web_asset_t assets[] = {
    {"/", "text/html", dummy_data, sizeof(dummy_data)},
    {"/app.js", "application/javascript", dummy_data, sizeof(dummy_data)},
    {"/app.css", "text/css", dummy_data, sizeof(dummy_data)}
};
static const size_t assets_count = sizeof(assets) / sizeof(assets[0]);

void test_web_assets_find_exact(void) {
    const web_asset_t *res;

    res = web_assets_find(assets, assets_count, "/");
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQUAL_STRING("/", res->path);

    res = web_assets_find(assets, assets_count, "/app.js");
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQUAL_STRING("/app.js", res->path);

    res = web_assets_find(assets, assets_count, "/app.css");
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQUAL_STRING("/app.css", res->path);
}

void test_web_assets_find_query_string(void) {
    const web_asset_t *res;

    res = web_assets_find(assets, assets_count, "/?v=123");
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQUAL_STRING("/", res->path);

    res = web_assets_find(assets, assets_count, "/app.js?cache=bust");
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_EQUAL_STRING("/app.js", res->path);
}

void test_web_assets_find_invalid(void) {
    // Should reject unknown paths
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, "/unknown"));

    // Should reject case variations
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, "/App.js"));

    // Should reject trailing slashes (except exactly "/")
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, "/app.js/"));

    // Traversal and dangerous characters
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, "/../app.js"));
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, "/app.js#fragment"));
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, "/app%2Ejs"));
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, "/app\\js"));
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, "//app.js"));

    // Very long URI
    char long_uri[300];
    memset(long_uri, 'a', sizeof(long_uri));
    long_uri[299] = '\0';
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, long_uri));

    // Edge cases
    TEST_ASSERT_NULL(web_assets_find(NULL, assets_count, "/"));
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, NULL));
    TEST_ASSERT_NULL(web_assets_find(assets, 0, "/"));
    TEST_ASSERT_NULL(web_assets_find(assets, assets_count, ""));
}

void test_web_assets_csp(void) {
    const char *csp_html = web_assets_csp("text/html");
    const char *csp_js = web_assets_csp("application/javascript");
    const char *csp_null = web_assets_csp(NULL);

    TEST_ASSERT_EQUAL_STRING("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' blob:; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'", csp_html);

    TEST_ASSERT_EQUAL_STRING("default-src 'none'; frame-ancestors 'none'", csp_js);
    TEST_ASSERT_EQUAL_STRING("default-src 'none'; frame-ancestors 'none'", csp_null);

    // Ensure dangerous keywords are not present
    TEST_ASSERT_NULL(strstr(csp_html, "unsafe-inline"));
    TEST_ASSERT_NULL(strstr(csp_html, "unsafe-eval"));
    TEST_ASSERT_NULL(strstr(csp_html, "*"));
    TEST_ASSERT_NULL(strstr(csp_html, "data:"));
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void test_web_assets_fuzz_uri(void) {
    uint32_t rng = 12345;
    char uri[20];

    for (int i = 0; i < 10000; i++) {
        int len = (xorshift32(&rng) % 18) + 1; // 1 to 18
        for(int j=0; j<len; j++) {
            // Generate mostly printable ASCII, occasionally dangerous ones
            uint8_t b = xorshift32(&rng) % 256;
            if (b < 128) {
                uri[j] = (char)b;
                if (uri[j] == 0) uri[j] = 1; // Avoid early truncation for now
            } else {
                char dangerous[] = {'/', '.', '#', '%', '\\', '?', 0};
                uri[j] = dangerous[xorshift32(&rng) % 7];
            }
        }
        uri[len] = '\0';

        // Let it run; we just ensure it doesn't crash or return an asset unless it happens to be valid.
        const web_asset_t *res = web_assets_find(assets, assets_count, uri);

        // If it did match, it must strictly be one of our valid cases
        if (res) {
            bool valid = false;
            if (strcmp(uri, "/") == 0) valid = true;
            if (strcmp(uri, "/app.js") == 0) valid = true;
            if (strcmp(uri, "/app.css") == 0) valid = true;
            if (strncmp(uri, "/?", 2) == 0) valid = true;
            if (strncmp(uri, "/app.js?", 8) == 0) valid = true;
            if (strncmp(uri, "/app.css?", 9) == 0) valid = true;

            if (!valid) {
                // If it claims to match but we didn't expect it...
                // ensure it hasn't matched a traversal or similar trick
                TEST_ASSERT_NULL(strstr(uri, ".."));
                TEST_ASSERT_NULL(strchr(uri, '\\'));
                TEST_ASSERT_NULL(strstr(uri, "//"));
            }
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_web_assets_find_exact);
    RUN_TEST(test_web_assets_find_query_string);
    RUN_TEST(test_web_assets_find_invalid);
    RUN_TEST(test_web_assets_csp);
    RUN_TEST(test_web_assets_fuzz_uri);
    return UNITY_END();
}
