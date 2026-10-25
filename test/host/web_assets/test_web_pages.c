#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buffer = malloc(length + 1);
    if (buffer) {
        fread(buffer, 1, length, f);
        buffer[length] = '\0';
    }
    fclose(f);
    return buffer;
}

static const char* check_forbidden_patterns(const char *content) {
    const char *script_tag = strstr(content, "<script>");
    if (!script_tag) script_tag = strstr(content, "<script type");
    if (script_tag) {
        if (!strstr(script_tag, "src=")) return "Inline script found";
    }
    if (strstr(content, "style=")) return "Inline style attribute found";
    if (strstr(content, "innerHTML")) return "innerHTML found";
    if (strstr(content, "eval(")) return "eval found";
    if (strstr(content, "document.write")) return "document.write found";
    if (strstr(content, "http://")) return "http:// external resource found";
    if (strstr(content, "https://")) return "https:// external resource found";
    const char *handlers[] = {"onclick=", "onload=", "onerror=", "onsubmit="};
    for (size_t i = 0; i < sizeof(handlers)/sizeof(handlers[0]); i++) {
        if (strstr(content, handlers[i])) return "Event handler found";
    }
    return NULL;
}

void test_index_html(void) {
    char *html = read_file("../components/web_server/web/index.html");
    TEST_ASSERT_NOT_NULL(html);
    const char *err = check_forbidden_patterns(html);

    int refs_ok = (strstr(html, "src=\"app.js\"") != NULL) && (strstr(html, "href=\"app.css\"") != NULL);
    free(html);

    if (err) TEST_FAIL_MESSAGE(err);
    TEST_ASSERT_TRUE_MESSAGE(refs_ok, "Missing references to app.js or app.css");
}

void test_app_js(void) {
    char *js = read_file("../components/web_server/web/app.js");
    TEST_ASSERT_NOT_NULL(js);
    const char *err = check_forbidden_patterns(js);
    free(js);
    if (err) TEST_FAIL_MESSAGE(err);
}

void test_app_css(void) {
    char *css = read_file("../components/web_server/web/app.css");
    TEST_ASSERT_NOT_NULL(css);
    const char *err = check_forbidden_patterns(css);
    free(css);
    if (err) TEST_FAIL_MESSAGE(err);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_index_html);
    RUN_TEST(test_app_js);
    RUN_TEST(test_app_css);
    return UNITY_END();
}
