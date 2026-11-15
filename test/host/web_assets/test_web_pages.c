#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "web_assets.h"

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
        if (my_strcasestr(content, "ws://")) return "ws:// found (the viewer must use wss:// only)";

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


void test_app_js_websocket_rules(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/app.js", WEB_ASSET_DIR);
    char *js = read_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(js, path);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, "location.host"), "location.host must be used to construct WS URL");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(js, "wss:"), "wss: schema must be checked or used");
    free(js);
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


static char *read_file_for_hint(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/index.html", WEB_ASSET_DIR);
    return read_file(path);
}

// ---- 映像サーバー(GET /)の信頼ページ: video_trust.html (docs/VIDEO_TRANSPORT.md 5 章) ----
// 認証なしで返してよい代わりに、動的な値を一切含まない・外部を読み込まない・自己完結であること。

static char *read_trust_page(size_t *len_out) {
    char path[256];
    snprintf(path, sizeof(path), "%s/video_trust.html", WEB_ASSET_DIR);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_INT((int)n, (int)fread(buf, 1, (size_t)n, f));
    buf[n] = '\0';
    fclose(f);
    if (len_out) *len_out = (size_t)n;
    return buf;
}

void test_video_trust_html_passes_the_common_rules(void) {
    char *html = read_trust_page(NULL);
    const char *err = check_forbidden_patterns(html, 0);
    free(html);
    if (err) TEST_FAIL_MESSAGE(err);
}

void test_video_trust_html_is_static_and_self_contained(void) {
    size_t len = 0;
    char *html = read_trust_page(&len);
    // 小さい(上限は余裕を持たせた値。大きくなったら理由を確認する)
    TEST_ASSERT_TRUE_MESSAGE(len > 0 && len <= 1500, "video_trust.html size out of range");
    // 文書の形
    TEST_ASSERT_EQUAL_INT(0, strncmp(html, "<!DOCTYPE html>", 15));
    TEST_ASSERT_NOT_NULL(strstr(html, "<meta charset=\"UTF-8\">"));
    TEST_ASSERT_NOT_NULL(strstr(html, "</html>"));
    // NUL・制御文字(タブ・改行以外)を含まない
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)html[i];
        TEST_ASSERT_TRUE_MESSAGE(c >= 0x20 || c == '\n' || c == '\t' || c == '\r', "control character");
    }
    // 外部の読み込み、実行、入力、リンクを持たない(自己完結。CSP default-src 'none' でも壊れない)
    static const char *const forbidden[] = {
        "<script", "<link", "<img", "<iframe", "<frame", "<object", "<embed", "<form", "<input", "<button",
        "<a ", "<a>", "<base", "<svg", "<video", "<audio", "<source", "<meta http-equiv", "href", "src=",
        "url(", "@import", "srcset", "ping=", "action=", "data:", "blob:", "//",
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "video_trust.html must not contain \"%s\"", forbidden[i]);
        TEST_ASSERT_NULL_MESSAGE(my_strcasestr(html, forbidden[i]), msg);
    }
    // 動的な値を埋め込む印(テンプレート、printf の書式、サーバーが差し込む記号)を持たない
    static const char *const dynamic[] = {
        "%s", "%d", "%u", "%x", "%lu", "%%", "{{", "}}", "${", "<%", "$(", "@@", "{0}", "__",
        "cookie", "session", "token", "csrf", "nonce", "password", "set-cookie", "authenticated",
    };
    for (size_t i = 0; i < sizeof(dynamic) / sizeof(dynamic[0]); i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "video_trust.html must not contain \"%s\"", dynamic[i]);
        TEST_ASSERT_NULL_MESSAGE(my_strcasestr(html, dynamic[i]), msg);
    }
    free(html);
}

void test_video_trust_html_text(void) {
    char *html = read_trust_page(NULL);
    TEST_ASSERT_NOT_NULL(strstr(html, "<title>"));
    TEST_ASSERT_NOT_NULL(strstr(html, "この接続を信頼しました"));
    TEST_ASSERT_NOT_NULL(strstr(html, "元のページ"));
    TEST_ASSERT_NOT_NULL(strstr(html, "再読み込み"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(html, "元のアドレス"), "must tell the person to open the original address by hand");
    free(html);
}

void test_video_trust_html_served_as_the_root_of_the_video_server(void) {
    // 映像サーバーの表は "/" だけ。web_server.c の表(IDF 側)と同じ web_asset_t で引く。
    size_t len = 0;
    char *html = read_trust_page(&len);
    const web_asset_t table[] = {
        {"/", "text/html", (const unsigned char *)html, len},
    };
    const web_asset_t *a = web_assets_find(table, 1, "/");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("text/html", a->content_type);
    TEST_ASSERT_EQUAL_size_t(len, a->len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(a->data, html, len));
    // 相対パス、ファイル名、クエリ付きの探索: ファイル名では引けない。クエリは無視して "/" に当たる(既存の規則)。
    TEST_ASSERT_NULL(web_assets_find(table, 1, "/video_trust.html"));
    TEST_ASSERT_NULL(web_assets_find(table, 1, "/app.js"));
    TEST_ASSERT_NULL(web_assets_find(table, 1, "/video"));
    TEST_ASSERT_NULL(web_assets_find(table, 1, "//"));
    TEST_ASSERT_NOT_NULL(web_assets_find(table, 1, "/?x=1"));
    // CSP は HTML 用(自己完結なので、script/style/img などの許可は使わないが、既存の規則に従う)
    TEST_ASSERT_EQUAL_STRING(web_assets_csp("text/html"), web_assets_csp(a->content_type));
    TEST_ASSERT_NOT_NULL(strstr(web_assets_csp(a->content_type), "frame-ancestors 'none'"));
    TEST_ASSERT_NOT_NULL(strstr(web_assets_csp(a->content_type), "default-src 'none'"));
    free(html);
}

void test_index_html_has_the_hidden_certificate_hint(void) {
    char *html = read_file_for_hint();
    TEST_ASSERT_NOT_NULL(html);
    const char *hint = strstr(html, "id=\"video-trust-hint\"");
    TEST_ASSERT_NOT_NULL_MESSAGE(hint, "index.html lacks #video-trust-hint");
    const char *tag_end = strchr(hint, '>');
    TEST_ASSERT_NOT_NULL(tag_end);
    char tag[256];
    size_t n = (size_t)(tag_end - hint);
    TEST_ASSERT_TRUE(n < sizeof(tag));
    memcpy(tag, hint, n);
    tag[n] = '\0';
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(tag, "trust-hint-hidden"), "the hint must start hidden");
    TEST_ASSERT_NOT_NULL(strstr(html, "映像用の接続の証明書を、まだ受け入れていない可能性があります。"));
    TEST_ASSERT_NOT_NULL(strstr(html, "証明書を受け入れてから、このページを再読み込みしてください"));
    free(html);

    char path[256];
    snprintf(path, sizeof(path), "%s/app.css", WEB_ASSET_DIR);
    char *css = read_file(path);
    TEST_ASSERT_NOT_NULL(css);
    const char *rule = strstr(css, ".trust-hint-hidden");
    TEST_ASSERT_NOT_NULL_MESSAGE(rule, "app.css lacks .trust-hint-hidden");
    const char *rule_end = strchr(rule, '}');
    const char *none = strstr(rule, "display: none");
    TEST_ASSERT_TRUE_MESSAGE(rule_end != NULL && none != NULL && none < rule_end, ".trust-hint-hidden needs display: none");
    free(css);
}

void test_app_js_builds_the_video_url_only_through_the_pure_functions(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/app.js", WEB_ASSET_DIR);
    char *js = read_file(path);
    TEST_ASSERT_NOT_NULL(js);
    TEST_ASSERT_NOT_NULL(strstr(js, "const VideoTarget = {"));
    TEST_ASSERT_NOT_NULL(strstr(js, "VideoTarget.resolve(location, data.video_port)"));
    TEST_ASSERT_NOT_NULL(strstr(js, "new WebSocket(target.url)"));
    // 映像の WebSocket の URL(".../video'" の文字列)は、VideoTarget の中にだけある
    const char *vt = strstr(js, "const VideoTarget = {");
    const char *end = strstr(js, "function showTrustHint");
    TEST_ASSERT_NOT_NULL(vt);
    TEST_ASSERT_NOT_NULL(end);
    int count = 0;
    for (const char *p = js; (p = strstr(p, "/video'")) != NULL; p++) {
        TEST_ASSERT_TRUE_MESSAGE(p > vt && p < end, "the /video URL must come from VideoTarget.resolve only");
        count++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count, "expected the legacy and the separate-port /video URLs");
    free(js);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_index_html);
    RUN_TEST(test_app_js);
    RUN_TEST(test_app_js_websocket_rules);
    RUN_TEST(test_app_css);
    RUN_TEST(test_app_js_shows_a_message_for_every_setup_error_code);
    RUN_TEST(test_app_js_pairs_each_code_with_its_own_message);
    RUN_TEST(test_app_js_checks_the_password_length_in_bytes_before_sending);
    RUN_TEST(test_index_html_explains_the_password_rules);
    RUN_TEST(test_video_trust_html_passes_the_common_rules);
    RUN_TEST(test_video_trust_html_is_static_and_self_contained);
    RUN_TEST(test_video_trust_html_text);
    RUN_TEST(test_video_trust_html_served_as_the_root_of_the_video_server);
    RUN_TEST(test_index_html_has_the_hidden_certificate_hint);
    RUN_TEST(test_app_js_builds_the_video_url_only_through_the_pure_functions);
    return UNITY_END();
}
