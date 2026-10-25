#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef WEB_ASSET_DIR
#define WEB_ASSET_DIR "../../components/web_server/web"
#endif

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

// Case-insensitive substring search
static const char *my_strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (const char *p = haystack; *p; p++) {
        if (toupper((unsigned char)*p) == toupper((unsigned char)*needle)) {
            const char *h = p, *n = needle;
            while (*h && *n && toupper((unsigned char)*h) == toupper((unsigned char)*n)) {
                h++; n++;
            }
            if (!*n) return p;
        }
    }
    return NULL;
}

static const char* check_forbidden_patterns(const char *content, int is_js) {
    // 1. Check <script> tags
    const char *p = content;
    while ((p = my_strcasestr(p, "<script")) != NULL) {
        const char *end_tag = strchr(p, '>');
        if (!end_tag) return "Unclosed <script tag";

        // Find src= within the tag
        const char *src = my_strcasestr(p, "src=");
        if (!src || src > end_tag) {
            return "Inline script found";
        }

        // Ensure the src value doesn't start with //, http://, or https://
        // Look for quote
        const char *quote = strchr(src, '"');
        if (!quote) quote = strchr(src, '\'');
        if (quote && quote < end_tag) {
            quote++;
            if (strncmp(quote, "//", 2) == 0) return "Protocol-relative URL in script src";
            if (my_strcasestr(quote, "http://") == quote) return "http:// in script src";
            if (my_strcasestr(quote, "https://") == quote) return "https:// in script src";
        }

        p = end_tag + 1;
    }

    // 2. Disallow <style tags
    if (my_strcasestr(content, "<style")) return "<style tag found";

    // 3. Disallow style= attribute
    // Need to handle optional spaces like style =
    p = content;
    while ((p = my_strcasestr(p, "style")) != NULL) {
        const char *tmp = p + 5;
        while (*tmp == ' ' || *tmp == '\t') tmp++;
        if (*tmp == '=') return "style= attribute found";
        p = tmp;
    }

    // 4. Disallow on* attributes
    p = content;
    while ((p = my_strcasestr(p, "on")) != NULL) {
        // Must be boundary-like to avoid matching inside strings, but for simplicity
        // we check if it's followed by alphas and then '='
        if (p == content || p[-1] == ' ' || p[-1] == '"' || p[-1] == '\'' || p[-1] == '<' || p[-1] == '\t' || p[-1] == '\n') {
            const char *tmp = p + 2;
            int has_alpha = 0;
            while (isalpha((unsigned char)*tmp)) { tmp++; has_alpha = 1; }
            if (has_alpha) {
                while (*tmp == ' ' || *tmp == '\t') tmp++;
                if (*tmp == '=') return "on* event handler found";
            }
        }
        p += 2;
    }

    // 5. Disallow javascript:
    if (my_strcasestr(content, "javascript:")) return "javascript: found";

    // 6. Disallow protocol relative URLs (="//, '//, (//, url(//)
    if (strstr(content, "=\"//")) return "=\"// found";
    if (strstr(content, "='//")) return "='// found";
    if (strstr(content, "('//")) return "('// found";
    if (strstr(content, "(//")) return "(// found"; // covers url(// and Function(//

    // 7. Disallow @import
    if (my_strcasestr(content, "@import")) return "@import found";

    // 8. Disallow http:// and https://
    if (my_strcasestr(content, "http://")) return "http:// found";
    if (my_strcasestr(content, "https://")) return "https:// found";

    if (is_js) {
        // JS specific checks
        if (my_strcasestr(content, "innerHTML")) return "innerHTML found";
        if (my_strcasestr(content, "outerHTML")) return "outerHTML found";
        if (my_strcasestr(content, "insertAdjacentHTML")) return "insertAdjacentHTML found";

        p = content;
        while ((p = my_strcasestr(p, "eval")) != NULL) {
            const char *tmp = p + 4;
            while (*tmp == ' ' || *tmp == '\t') tmp++;
            if (*tmp == '(') return "eval found";
            p = tmp;
        }

        if (my_strcasestr(content, "Function(")) return "Function( found";

        p = content;
        while ((p = my_strcasestr(p, "setTimeout")) != NULL) {
            const char *tmp = p + 10;
            while (*tmp == ' ' || *tmp == '\t') tmp++;
            if (*tmp == '(') {
                tmp++;
                while (*tmp == ' ' || *tmp == '\t') tmp++;
                if (*tmp == '"' || *tmp == '\'') return "setTimeout with string found";
            }
            p = tmp;
        }

        p = content;
        while ((p = my_strcasestr(p, "setInterval")) != NULL) {
            const char *tmp = p + 11;
            while (*tmp == ' ' || *tmp == '\t') tmp++;
            if (*tmp == '(') {
                tmp++;
                while (*tmp == ' ' || *tmp == '\t') tmp++;
                if (*tmp == '"' || *tmp == '\'') return "setInterval with string found";
            }
            p = tmp;
        }

        if (my_strcasestr(content, "document.write")) return "document.write found";
        if (my_strcasestr(content, "console.")) return "console. found";

        // fetch strict check - allow fetch(url) where url is variable since we use it in wrapper
        p = content;
        while ((p = my_strcasestr(p, "fetch(")) != NULL) {
            const char *tmp = p + 6;
            while (*tmp == ' ' || *tmp == '\t') tmp++;
            if (*tmp == '"' || *tmp == '\'') {
                tmp++;
                if (strncmp(tmp, "/api/v1/auth/", 13) != 0) {
                    return "fetch URL does not start with /api/v1/auth/";
                }
            } else if (strncmp(tmp, "url", 3) != 0) {
                return "fetch called without string literal or 'url' variable";
            }
            p = tmp;
        }
    }

    return NULL;
}

void test_index_html(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/index.html", WEB_ASSET_DIR);
    char *html = read_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(html, path);
    const char *err = check_forbidden_patterns(html, 0);

    int refs_ok = (strstr(html, "src=\"app.js\"") != NULL) && (strstr(html, "href=\"app.css\"") != NULL);
    free(html);

    if (err) TEST_FAIL_MESSAGE(err);
    TEST_ASSERT_TRUE_MESSAGE(refs_ok, "Missing references to app.js or app.css");
}

void test_app_js(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/app.js", WEB_ASSET_DIR);
    char *js = read_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(js, path);
    const char *err = check_forbidden_patterns(js, 1);
    free(js);
    if (err) TEST_FAIL_MESSAGE(err);
}

void test_app_css(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/app.css", WEB_ASSET_DIR);
    char *css = read_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(css, path);
    const char *err = check_forbidden_patterns(css, 0);
    free(css);
    if (err) TEST_FAIL_MESSAGE(err);
}

// Error codes the server returns for a rejected setup password (web_status_map_setup_error_code) and the
// message the page must show for each of them.
static const struct { const char *code; const char *message; } k_setup_errors[] = {
    {"password_too_short",     "パスワードは 10 文字以上にしてください"},
    {"password_too_long",      "パスワードは 128 バイト以下にしてください"},
    {"password_contains_admin", "パスワードに「admin」を含めないでください"},
    {"password_all_same",      "同じ文字だけの繰り返しは使えません"},
    {"password_sequential",    "連番（0123456789 や abcdefghij など）は使えません"},
    {"password_common",        "よく使われるパスワードは使えません"},
    {"bad_request",            "リクエストが不正です"},
};

void test_app_js_shows_a_message_for_every_setup_error_code(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/app.js", WEB_ASSET_DIR);
    char *js = read_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(js, path);
    for (size_t i = 0; i < sizeof(k_setup_errors) / sizeof(k_setup_errors[0]); i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "app.js lacks the code %s", k_setup_errors[i].code);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, k_setup_errors[i].code), msg);
        snprintf(msg, sizeof(msg), "app.js lacks the message for %s", k_setup_errors[i].code);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, k_setup_errors[i].message), msg);
    }
    free(js);
}

void test_app_js_pairs_each_code_with_its_own_message(void) {
    // Guards against swapped messages: the message must follow its code on the same line.
    char path[256];
    snprintf(path, sizeof(path), "%s/app.js", WEB_ASSET_DIR);
    char *js = read_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(js, path);
    for (size_t i = 0; i < sizeof(k_setup_errors) / sizeof(k_setup_errors[0]); i++) {
        const char *code = strstr(js, k_setup_errors[i].code);
        TEST_ASSERT_NOT_NULL(code);
        const char *eol = strchr(code, '\n');
        const char *msg = strstr(code, k_setup_errors[i].message);
        char text[96];
        snprintf(text, sizeof(text), "message for %s is not on the same line as its code", k_setup_errors[i].code);
        TEST_ASSERT_TRUE_MESSAGE(msg != NULL && (eol == NULL || msg < eol), text);
    }
    free(js);
}

void test_app_js_checks_the_password_length_in_bytes_before_sending(void) {
    // The server rejects bodies whose password is longer than 128 bytes with a generic bad request, so the
    // page reports the real reason itself (the limit is in bytes, not characters).
    char path[256];
    snprintf(path, sizeof(path), "%s/app.js", WEB_ASSET_DIR);
    char *js = read_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(js, path);
    TEST_ASSERT_NOT_NULL(strstr(js, "TextEncoder"));
    TEST_ASSERT_NOT_NULL(strstr(js, "128"));
    free(js);
}

void test_index_html_explains_the_password_rules(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/index.html", WEB_ASSET_DIR);
    char *html = read_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(html, path);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(html,
        "10 文字以上、128 バイト以下。連番、同じ文字の繰り返し、よく使われるパスワード、「admin」を含むものは使えません。"),
        "setup form lacks the password rules");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(html, "id=\"setup-hint\""), "rules element id");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(html, "aria-describedby=\"setup-hint\""), "input is not linked to the rules");
    free(html);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_index_html);
    RUN_TEST(test_app_js);
    RUN_TEST(test_app_css);
    RUN_TEST(test_app_js_shows_a_message_for_every_setup_error_code);
    RUN_TEST(test_app_js_pairs_each_code_with_its_own_message);
    RUN_TEST(test_app_js_checks_the_password_length_in_bytes_before_sending);
    RUN_TEST(test_index_html_explains_the_password_rules);
    return UNITY_END();
}
