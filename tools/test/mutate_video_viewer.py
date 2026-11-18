#!/usr/bin/env python3
"""映像のビューア(Issue #82 ③ B-3)の C 側とアセットに、1 か所ずつ変更(ミュータント)を入れて、テストが落ちることを確認する。
対象:
  - http_guard.c の http_guard_format_status(video_port)       -> test/host/http_guard/ の全テスト
  - web/video_trust.html、web/index.html、web/app.css の追加分    -> test_web_pages(Unity)と tools/test/check_video_viewer.js(node)
app.js のミュータントは、check_video_viewer.js --mutations が自分で実行する(別の道具)。
使い方: python3 tools/test/mutate_video_viewer.py <repo> <workdir> <out.json> [id ...]  (id を指定すると、そのミュータントだけ実行する)
repo の components/ test/ tools/ を workdir にコピーして使う(repo は変更しない)。Unity の取得にネットワークが要る。
1 つでも失敗(アサーション、ASan/UBSan、終了コード非 0、タイムアウト)すれば「殺した」。同値のものは equivalent を付けて数える。"""
import glob, json, os, re, shutil, subprocess, sys


def check_workdir(repo, *paths):
    """作業先(paths)が repo と同じ、repo を内包する、または repo の内側なら、何も消さずにエラーで終了する
    (repo は変更しない)。シンボリックリンクは realpath で解決して比べる。rmtree / cmake の前に呼ぶこと。"""
    repo = os.path.realpath(repo)
    for p in paths:
        p = os.path.realpath(p)
        common = os.path.commonpath([p, repo])
        if common == p or common == repo:
            print(f"ERROR: work path {p} overlaps the repo {repo}; refusing to delete or write anything", file=sys.stderr)
            sys.exit(2)


def sh(cmd, **kw):
    """cmd は引数のリスト(シェルを通さない。空白を含むパスでも動く)。実行ファイルがなければ終了コード 127。"""
    try:
        return subprocess.run(cmd, capture_output=True, text=True, **kw)
    except FileNotFoundError as e:
        return subprocess.CompletedProcess(cmd, 127, "", str(e))


repo, work, outp = sys.argv[1:4]
only = set(sys.argv[4:])
work = os.path.realpath(work)
src = os.path.join(work, "repo")
build = os.path.join(work, "build")
check_workdir(repo, src, build)  # 何も消す前に確認する
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
    # ---- 映像の状態の重ねる表示(C-6b) ----
    M("I09", "index.html", "index: overlay no longer starts hidden", ('class="video-overlay video-overlay-hidden"', 'class="video-overlay"')),
    M("I10", "index.html", "index: overlay loses role=status", ('role="status" aria-live', 'aria-live')),
    M("I11", "index.html", "index: overlay aria-live polite -> assertive", ('aria-live="polite"', 'aria-live="assertive"')),
    M("I12", "index.html", "index: overlay id renamed", ('id="video-overlay"', 'id="video-status-overlay"')),
    M("I13", "index.html", "index: overlay placed before the canvas",
      ('            <div id="video-overlay" class="video-overlay video-overlay-hidden" role="status" aria-live="polite"></div>\n', ""),
      ('            <canvas id="video"', '            <div id="video-overlay" class="video-overlay video-overlay-hidden" role="status" aria-live="polite"></div>\n            <canvas id="video"')),
    M("I14", "index.html", "index: video wrapper class renamed", ('class="video-wrap"', 'class="video-box"')),
    M("I15", "index.html", "index: overlay starts with text", ('aria-live="polite"></div>', 'aria-live="polite">x</div>')),
    M("I16", "index.html", "index: overlay moved outside the wrapper",
      ('            <div id="video-overlay" class="video-overlay video-overlay-hidden" role="status" aria-live="polite"></div>\n        </div>\n', "        </div>\n"
       '        <div id="video-overlay" class="video-overlay video-overlay-hidden" role="status" aria-live="polite"></div>\n')),
    M("C03", "app.css", "css: overlay hidden rule shows it", (".video-overlay-hidden {\n    display: none;", ".video-overlay-hidden {\n    display: block;")),
    M("C04", "app.css", "css: overlay hidden rule removed", (".video-overlay-hidden {\n    display: none;\n}\n", "")),
    M("C05", "app.css", "css: wrapper is not the positioning context", (".video-wrap {\n    position: relative;", ".video-wrap {\n    position: static;")),
    M("C06", "app.css", "css: overlay not absolutely positioned", ("    position: absolute;\n    top: 50%;", "    position: fixed;\n    top: 50%;")),
    M("C07", "app.css", "css: overlay takes the clicks of the canvas", ("    pointer-events: none;\n", "")),
    M("C08", "app.css", "css: dark mode rule removed", ("@media (prefers-color-scheme: dark)", "@media print")),
    M("C09", "app.css", "css: overlay positioning rule renamed", (".video-overlay {\n    position: absolute;", ".video-overlay-box {\n    position: absolute;")),
    # ---- 表示モードとフルスクリーン(ダッシュボードを全幅に。check_video_viewer.js の (E) と test_web_pages が見る) ----
    M("I17", "index.html", "index: a mode button is not type=button", ('<button type="button" id="btn-view-width"', '<button id="btn-view-width"')),
    M("I18", "index.html", "index: the full screen button is not type=button", ('<button type="button" id="btn-fullscreen"', '<button id="btn-fullscreen"')),
    M("I19", "index.html", "index: the mode group loses role=group", ('role="group" aria-label="表示サイズ"', 'aria-label="表示サイズ"')),
    M("I20", "index.html", "index: the mode group label changed", ('aria-label="表示サイズ"', 'aria-label="モード"')),
    M("I21", "index.html", "index: fit does not start pressed", ('id="btn-view-fit" aria-pressed="true"', 'id="btn-view-fit" aria-pressed="false"')),
    M("I22", "index.html", "index: width starts pressed", ('id="btn-view-width" aria-pressed="false"', 'id="btn-view-width" aria-pressed="true"')),
    M("I23", "index.html", "index: a mode button id renamed", ('id="btn-view-actual"', 'id="btn-view-real"')),
    M("I24", "index.html", "index: a mode button label shortened", (">幅に合わせる<", ">幅<")),
    M("I25", "index.html", "index: the text under the video moves into the video wrap",
      ('        </div>\n        <div class="video-info">\n', '        <div class="video-info">\n'),
      ("</p>\n        </div>\n    </div>\n    <script", "</p>\n        </div>\n        </div>\n    </div>\n    <script")),
    M("I26", "index.html", "index: the dashboard moves into the narrow card",
      ('    </div>\n\n    <div id="view-dashboard"', '\n    <div id="view-dashboard"'),
      ('    </div>\n    <script src="app.js">', '    </div></div>\n    <script src="app.js">')),
    M("I27", "index.html", "index: the error message moves into the card",
      ('    <div id="error-message" class="error-hidden"></div>\n', ""),
      ('        <h1>Quad IP-KVM</h1>\n', '        <h1>Quad IP-KVM</h1>\n        <div id="error-message" class="error-hidden"></div>\n')),
    M("I28", "index.html", "index: the dashboard is a narrow .container", ('class="dashboard mode-fit view-hidden"', 'class="dashboard container mode-fit view-hidden"')),
    M("I29", "index.html", "index: an inline style on the hint", ('<p class="hint video-hint">', '<p class="hint video-hint" style="margin: -10px 0 0">')),
    M("I30", "index.html", "index: the logout button leaves the bar",
      ('            <button type="button" id="btn-logout">ログアウト</button>\n        </div>\n', "        </div>\n"),
      ('        </div>\n    </div>\n    <script', '            <button type="button" id="btn-logout">ログアウト</button>\n        </div>\n    </div>\n    <script')),
    M("I31", "index.html", "index: the bar title is removed", ('            <span class="bar-title">Quad IP-KVM</span>\n', "")),
    M("I32", "index.html", "index: the full screen button leaves the keyboard", ('<button type="button" id="btn-fullscreen">', '<button type="button" tabindex="-1" id="btn-fullscreen">')),
    M("I33", "index.html", "index: the mode buttons in another order",
      ('                <button type="button" id="btn-view-fit" aria-pressed="true">ウィンドウに合わせる</button>\n                <button type="button" id="btn-view-width" aria-pressed="false">幅に合わせる</button>\n',
       '                <button type="button" id="btn-view-width" aria-pressed="false">幅に合わせる</button>\n                <button type="button" id="btn-view-fit" aria-pressed="true">ウィンドウに合わせる</button>\n')),
    M("I34", "index.html", "index: the logout button is not type=button", ('<button type="button" id="btn-logout">', '<button id="btn-logout">')),
    M("I35", "index.html", "index: the input toggle is not type=button", ('<button type="button" id="btn-input-toggle">', '<button id="btn-input-toggle">')),
    M("I36", "index.html", "index: the hint is not in the text block", ('<p class="hint video-hint">', '<p class="hint">')),
    M("I37", "index.html", "index: an inline script is added", ("</body>", "<script>1</script>\n</body>")),
    M("I38", "index.html", "index: the dashboard starts visible", ('class="dashboard mode-fit view-hidden"', 'class="dashboard mode-fit"')),
    M("C10", "app.css", "css: the hint overlaps the line above again", (".hint {\n    margin: 0;", ".hint {\n    margin: -10px 0 15px;")),
    M("C11", "app.css", "css: the form hint loses its negative margin", ("form .hint {\n    margin: -10px 0 15px;", "form .hint {\n    margin: 0;")),
    M("C12", "app.css", "css: the video is limited by max-width", ("#video {\n    display: block;", "#video {\n    max-width: 100%;\n    display: block;")),
    M("C13", "app.css", "css: the video is fitted by object-fit", ("    flex: none;\n    margin: auto;", "    flex: none;\n    object-fit: contain;\n    margin: auto;")),
    M("C14", "app.css", "css: the video is inline again", ("#video {\n    display: block;", "#video {\n    display: inline;")),
    M("C15", "app.css", "css: the video may shrink in the flex row", ("    flex: none;\n    margin: auto;", "    margin: auto;")),
    M("C16", "app.css", "css: the bar does not wrap", ("    flex-wrap: wrap;\n    align-items: center;\n    gap: 6px 8px;", "    flex-wrap: nowrap;\n    align-items: center;\n    gap: 6px 8px;")),
    M("C17", "app.css", "css: the mode buttons are not pushed to the right", ("    margin-left: auto;\n", "")),
    M("C18", "app.css", "css: the pressed mode button has no look", ('.bar-modes button[aria-pressed="true"] {', '.bar-modes button[aria-pressed="x"] {')),
    M("C19", "app.css", "css: the dashboard is flex even when hidden", ("#view-dashboard:not(.view-hidden) {\n    display: flex;", "#view-dashboard {\n    display: flex;")),
    M("C20", "app.css", "css: the bar stays in full screen", ("#view-dashboard.is-fullscreen .bar,\n#view-dashboard.is-fullscreen .video-info {\n    display: none;", "#view-dashboard.is-fullscreen .bar,\n#view-dashboard.is-fullscreen .video-info {\n    display: block;")),
    M("C21", "app.css", "css: the full screen wrap is not the window wide", ("    width: 100%;\n    height: 100%;\n    overflow: hidden;\n    background-color: #000;", "    height: 100%;\n    overflow: hidden;\n    background-color: #000;")),
    M("C22", "app.css", "css: the full screen background is white", ("    overflow: hidden;\n    background-color: #000;", "    overflow: hidden;\n    background-color: #fff;")),
    M("C23", "app.css", "css: fit lets the video area scroll", ("#view-dashboard.mode-fit .video-wrap {\n    overflow: hidden;", "#view-dashboard.mode-fit .video-wrap {\n    overflow: visible;")),
    M("C24", "app.css", "css: actual cannot scroll", ("#view-dashboard.mode-actual .video-wrap {\n    overflow: auto;", "#view-dashboard.mode-actual .video-wrap {\n    overflow: hidden;")),
    M("C25", "app.css", "css: width mode fixes the page height", ("#view-dashboard.mode-width {\n    min-height: 100vh;", "#view-dashboard.mode-width {\n    height: 100vh;")),
    M("C26", "app.css", "css: width mode limits the video area height", ("#view-dashboard.mode-width .video-wrap {\n    flex: none;", "#view-dashboard.mode-width .video-wrap {\n    flex: 1 1 0;")),
    M("C27", "app.css", "css: fit and actual lose the page height", ("    height: 100vh;\n    height: 100dvh;\n}", "    height: auto;\n}")),
    M("C28", "app.css", "css: the video area does not take the rest of the height", ("    flex: 1 1 0;\n    min-height: 0;\n}", "    flex: none;\n    min-height: 0;\n}")),
    M("C29", "app.css", "css: the video area may not shrink below its content", ("    flex: 1 1 0;\n    min-height: 0;\n}", "    flex: 1 1 0;\n}")),
    M("C30", "app.css", "css: the video wrap is not a flex box", ("    position: relative;\n    display: flex;\n}", "    position: relative;\n}")),
    M("C31", "app.css", "css: the video is not centered", ("    flex: none;\n    margin: auto;", "    flex: none;\n    margin: 0;")),
    M("C32", "app.css", "css: the button focus ring is removed", ("button:focus-visible {\n    outline: 2px solid #ff9800;", "button:focus-visible {\n    outline: none;\n    outline: 2px solid #ff9800;")),
    M("C33", "app.css", "css: buttons lose the focus outline", ("button:hover {", "button {\n    outline: none;\n}\nbutton:hover {")),
    M("C34", "app.css", "css: the disabled button looks enabled", ("button:disabled {\n    background-color: #8aa4c0;", "button:disabled-x {\n    background-color: #8aa4c0;")),
    M("C35", "app.css", "css: the input frame colour is lost", ("#video.input-active {\n    cursor: crosshair;\n    border-color: #007bff;", "#video.input-active {\n    cursor: crosshair;")),
    M("C36", "app.css", "css: the body has a 20px padding again", ("    margin: 0;\n    padding: 0;\n    background-color: #f0f0f0;", "    margin: 0;\n    padding: 20px;\n    background-color: #f0f0f0;")),
    M("C37", "app.css", "css: the dashboard has a wide padding", ("    gap: 6px;\n    padding: 6px 8px;", "    gap: 6px;\n    padding: 20px;")),
    M("C38", "app.css", "css: width mode does not keep the scroll bar space", ("    scrollbar-gutter: stable;", "    scrollbar-gutter: auto;")),
    M("C39", "app.css", "css: the full screen wrap has no border width reset", ("#view-dashboard .video-wrap:fullscreen #video {\n    border-width: 0;", "#view-dashboard .video-wrap:fullscreen #video {\n    border-width: 1px;")),
    M("C01", "app.css", "css: hidden rule no longer hides", (".trust-hint-hidden {\n    display: none;\n}", ".trust-hint-hidden {\n    display: block;\n}")),
    M("C02", "app.css", "css: hidden rule removed", (".trust-hint-hidden {\n    display: none;\n}\n", "")),
]


def tests_of(dirname):
    return sorted(os.path.splitext(os.path.basename(p))[0]
                  for p in glob.glob(os.path.join(src, "test/host", dirname, "test_*.c")))


r = sh(["cmake", "-S", os.path.join(src, "test", "host"), "-B", build])
if r.returncode:
    print(r.stdout, r.stderr)
    sys.exit(1)

C_TESTS = {"http_guard": tests_of("http_guard"), "web": ["test_web_pages"]}


def run_group(group):
    exes = C_TESTS[group]
    b = sh(["cmake", "--build", build, "-j8", "--target", *exes])
    if b.returncode != 0:
        return "build_error", [(b.stdout[-600:] + b.stderr[-600:])]
    failed = []
    for t in exes:
        try:
            p = sh([os.path.join(build, t)], env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"), timeout=120)
        except subprocess.TimeoutExpired:
            failed.append(t + ":timeout")
            continue
        if p.returncode != 0 or ":FAIL" in p.stdout or "runtime error" in p.stderr or "AddressSanitizer" in p.stderr:
            names = [m.group(1) for m in (re.match(r".*:(\w+):FAIL", l) for l in p.stdout.splitlines()) if m]
            failed.append(",".join(names) if names else t + ":crash")
    if group == "web":
        try:
            p = sh(["node", os.path.join(src, "tools", "test", "check_video_viewer.js")], timeout=120)
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
    if only and m["id"] not in only:
        continue
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
