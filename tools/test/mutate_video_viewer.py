#!/usr/bin/env python3
"""映像のビューア(Issue #82 ③ B-3)の C 側とアセットに、1 か所ずつ変更(ミュータント)を入れて、テストが落ちることを確認する。
対象:
  - http_guard.c の http_guard_format_status(video_port)       -> test/host/http_guard/ の全テスト
  - web/video_trust.html、web/index.html、web/app.css の追加分    -> test_web_pages(Unity)と tools/test/check_video_viewer.js(node)
app.js のミュータントは、check_video_viewer.js --mutations が自分で実行する(別の道具)。
使い方: python3 tools/test/mutate_video_viewer.py <repo> <workdir> <out.json>
repo の components/ test/ tools/ を workdir にコピーして使う(repo は変更しない)。Unity の取得にネットワークが要る。
1 つでも失敗(アサーション、ASan/UBSan、終了コード非 0、タイムアウト)すれば「殺した」。同値のものは equivalent を付けて数える。"""
import glob, json, os, re, shutil, subprocess, sys

repo, work, outp = sys.argv[1:4]
src = os.path.join(work, "repo")
build = os.path.join(work, "build")
for d in (src, build):  # build も消す(copytree は更新時刻を保つので、古い成果物が新しいと見なされることがある)
    if os.path.exists(d):
        shutil.rmtree(d)
os.makedirs(src)
for d in ("components", "test", "tools"):
    shutil.copytree(os.path.join(repo, d), os.path.join(src, d))

FILES = {
    "http_guard.c": os.path.join(src, "components/http_guard/http_guard.c"),
    "video_trust.html": os.path.join(src, "components/web_server/web/video_trust.html"),
    "index.html": os.path.join(src, "components/web_server/web/index.html"),
    "app.css": os.path.join(src, "components/web_server/web/app.css"),
}
GROUP = {"http_guard.c": "http_guard", "video_trust.html": "web", "index.html": "web", "app.css": "web"}
ORIG = {k: open(v, encoding="utf-8").read() for k, v in FILES.items()}


def M(id, file, desc, *edits, equivalent=None):
    return {"id": id, "file": file, "desc": desc, "edits": list(edits), "equivalent": equivalent}


FS_HEAD = "int http_guard_format_status(auth_state_e state, bool authenticated, uint16_t video_port, char *out, size_t out_len) {\n"
FS_TAIL = "state_str, auth_str, (unsigned)video_port);\n    }\n    if (ret < 0 || (size_t)ret >= out_len) return -1;"
HG = "http_guard.c"
T = "video_trust.html"
MUTANTS = [
    M("H01", HG, "format_status: zero check == 0 -> == 1", ("if (video_port == 0) {", "if (video_port == 1) {")),
    M("H02", HG, "format_status: zero check inverted", ("if (video_port == 0) {", "if (video_port != 0) {")),
    M("H03", HG, "format_status: video_port always printed (the 0 branch removed)",
      ("if (video_port == 0) {", "if (0) {")),
    M("H04", HG, "format_status: key name misspelled", ('\\"video_port\\":%u}', '\\"videoport\\":%u}')),
    M("H05", HG, "format_status: port truncated to 8 bits", ("(unsigned)video_port);\n    }", "(unsigned)(uint8_t)video_port);\n    }")),
    M("H06", HG, "format_status: port printed with a sign extension from 16 bits",
      ("(unsigned)video_port);\n    }", "(unsigned)(int16_t)video_port);\n    }")),
    M("H07", HG, "format_status: closing brace missing", ('\\"video_port\\":%u}"', '\\"video_port\\":%u"')),
    M("H08", HG, "format_status: comma before video_port missing", ('%s,\\"video_port\\"', '%s\\"video_port\\"')),
    M("H09", HG, "format_status: buffer check off by one (too lenient)", (FS_TAIL, FS_TAIL.replace(">= out_len", "> out_len"))),
    M("H10", HG, "format_status: buffer check off by one (too strict)", (FS_TAIL, FS_TAIL.replace(">= out_len", ">= out_len - 1"))),
    M("H11", HG, "format_status: buffer check removed (port branch)", (FS_TAIL, "state_str, auth_str, (unsigned)video_port);\n    }\n    if (ret < 0) return -1;")),
    M("H12", HG, "format_status: out NULL check removed", (FS_HEAD + "    if (!out || out_len == 0) return -1;\n", FS_HEAD + "    if (out_len == 0) return -1;\n")),
    M("H13", HG, "format_status: out_len 0 check removed", (FS_HEAD + "    if (!out || out_len == 0) return -1;\n", FS_HEAD + "    if (!out) return -1;\n"),
      equivalent="out_len == 0 のとき snprintf は何も書かず、戻り値(必要な長さ >= 1)が out_len 以上になるので、後ろの検査が -1 を返す(二重の防御)"),
    M("H14", HG, "format_status: needs_setup/ready swapped", ('(state == AUTH_STATE_NEEDS_SETUP) ? "needs_setup" : "ready";\n    const char *auth_str = authenticated ? "true" : "false";\n    int ret;',
                                                          '(state == AUTH_STATE_NEEDS_SETUP) ? "ready" : "needs_setup";\n    const char *auth_str = authenticated ? "true" : "false";\n    int ret;')),
    M("H15", HG, "format_status: authenticated true/false swapped", ('const char *auth_str = authenticated ? "true" : "false";\n    int ret;', 'const char *auth_str = authenticated ? "false" : "true";\n    int ret;')),
    M("H16", HG, "format_status: legacy (port 0) form gets a trailing comma", ('\\"authenticated\\":%s}", state_str, auth_str);', '\\"authenticated\\":%s,}", state_str, auth_str);')),
    M("H17", HG, "format_status: error return replaced by success on a too-small buffer", (FS_TAIL, FS_TAIL.replace("return -1;", "return 0;"))),
    # ---- video_trust.html ----
    M("T01", T, "trust page: external script added", ("</body>", '<script src="app.js"></script>\n</body>')),
    M("T02", T, "trust page: inline script added", ("</body>", "<script>1</script>\n</body>")),
    M("T03", T, "trust page: a link added", ("</body>", '<a href="x">x</a>\n</body>')),
    M("T04", T, "trust page: stylesheet added", ("</head>", '<link rel="stylesheet" href="app.css">\n</head>')),
    M("T05", T, "trust page: printf placeholder added", ("<p>このページに映像や設定はありません。</p>", "<p>%s</p>")),
    M("T06", T, "trust page: cookie word added", ("<p>このページに映像や設定はありません。</p>", "<p>cookie: x</p>")),
    M("T07", T, "trust page: image added", ("</body>", '<img src="a.png">\n</body>')),
    M("T08", T, "trust page: https URL added", ("</body>", "<p>https://example.com/</p>\n</body>")),
    M("T09", T, "trust page: main headline removed", ("この接続を信頼しました", "完了しました")),
    M("T10", T, "trust page: the by-hand address advice removed", ("元のアドレス", "ブックマーク")),
    M("T11", T, "trust page: reload advice removed", ("再読み込み", "確認")),
    M("T12", T, "trust page: grows beyond 1500 bytes", ("</body>", "<!-- " + "x" * 1000 + " -->\n</body>")),
    M("T13", T, "trust page: style tag added", ("</head>", "<style>p{}</style>\n</head>")),
    M("T14", T, "trust page: inline event handler added", ("<body>", '<body onload="x()">')),
    M("T15", T, "trust page: form added", ("</body>", '<form></form>\n</body>')),
    M("T16", T, "trust page: protocol-relative URL text added", ("</body>", "<p>//example.com</p>\n</body>")),
    M("T17", T, "trust page: closing html tag removed", ("</html>", "")),
    M("T18", T, "trust page: charset removed", ('<meta charset="UTF-8">\n', "")),
    M("T19", T, "trust page: doctype removed", ("<!DOCTYPE html>\n", "")),
    M("T20", T, "trust page: session word added", ("<p>このページに映像や設定はありません。</p>", "<p>session</p>")),
    # ---- index.html / app.css ----
    M("I01", "index.html", "index: hint no longer starts hidden", ('class="trust-hint trust-hint-hidden"', 'class="trust-hint"')),
    M("I02", "index.html", "index: hint id renamed", ('id="video-trust-hint"', 'id="video-hint"')),
    M("I03", "index.html", "index: hint link id renamed", ('id="video-trust-link"', 'id="video-link"')),
    M("I04", "index.html", "index: link opens in the same tab", ('target="_blank" ', "")),
    M("I05", "index.html", "index: link loses rel noopener", ('rel="noopener noreferrer"', 'rel="noreferrer"')),
    M("I06", "index.html", "index: link gets a static href", ('<a id="video-trust-link"', '<a href="x" id="video-trust-link"')),
    M("I07", "index.html", "index: hint sentence changed", ("まだ受け入れていない可能性があります。", "まだ受け入れていません。")),
    M("I08", "index.html", "index: reload instruction removed", ("このページを再読み込みしてください", "しばらくお待ちください")),
    M("C01", "app.css", "css: hidden rule no longer hides", (".trust-hint-hidden {\n    display: none;\n}", ".trust-hint-hidden {\n    display: block;\n}")),
    M("C02", "app.css", "css: hidden rule removed", (".trust-hint-hidden {\n    display: none;\n}\n", "")),
]


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)


def tests_of(dirname):
    return sorted(os.path.splitext(os.path.basename(p))[0]
                  for p in glob.glob(os.path.join(src, "test/host", dirname, "test_*.c")))


r = sh(f"cmake -S {src}/test/host -B {build}")
if r.returncode:
    print(r.stdout, r.stderr)
    sys.exit(1)

C_TESTS = {"http_guard": tests_of("http_guard"), "web": ["test_web_pages"]}


def run_group(group):
    exes = C_TESTS[group]
    b = sh(f"cmake --build {build} -j8 --target {' '.join(exes)}")
    if b.returncode != 0:
        return "build_error", [(b.stdout[-600:] + b.stderr[-600:])]
    failed = []
    for t in exes:
        try:
            p = sh(f"ASAN_OPTIONS=detect_leaks=0 {build}/{t}", timeout=120)
        except subprocess.TimeoutExpired:
            failed.append(t + ":timeout")
            continue
        if p.returncode != 0 or ":FAIL" in p.stdout or "runtime error" in p.stderr or "AddressSanitizer" in p.stderr:
            names = [m.group(1) for m in (re.match(r".*:(\w+):FAIL", l) for l in p.stdout.splitlines()) if m]
            failed.append(",".join(names) if names else t + ":crash")
    if group == "web":
        try:
            p = sh(f"node {src}/tools/test/check_video_viewer.js", timeout=120)
            if p.returncode != 0:
                failed.append("check_video_viewer.js")
        except subprocess.TimeoutExpired:
            failed.append("check_video_viewer.js:timeout")
    return ("killed" if failed else "survived"), failed


for g in C_TESTS:
    st, failed = run_group(g)
    print("baseline", g, st, failed, flush=True)
    if st != "survived":
        print("baseline must pass")
        sys.exit(1)

results = []
for m in MUTANTS:
    f = m["file"]
    orig = ORIG[f]
    text = orig
    for old, new in m["edits"]:
        if text.count(old) != 1:
            print("BAD EDIT", m["id"], text.count(old), repr(old[:80]))
            sys.exit(2)
        text = text.replace(old, new)
    if text == orig:
        print("NO-OP", m["id"])
        sys.exit(2)
    open(FILES[f], "w", encoding="utf-8").write(text)
    st, failed = run_group(GROUP[f])
    if st == "survived" and m["equivalent"]:
        st = "equivalent"
    results.append({"id": m["id"], "file": f, "desc": m["desc"], "status": st, "failed": failed[:4], "equivalent": m["equivalent"]})
    print(f'{m["id"]:4} {st:11} {m["desc"][:70]}  -> {"; ".join(failed[:2])}', flush=True)
    open(FILES[f], "w", encoding="utf-8").write(orig)

json.dump(results, open(outp, "w"), ensure_ascii=False, indent=1)
print("run:", len(results), "killed:", sum(r["status"] == "killed" for r in results),
      "equivalent:", sum(r["status"] == "equivalent" for r in results),
      "survived:", sum(r["status"] == "survived" for r in results),
      "build_error:", sum(r["status"] == "build_error" for r in results))
