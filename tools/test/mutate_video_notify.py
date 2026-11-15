#!/usr/bin/env python3
"""映像の状態の通知(C-6b)のコードに 1 か所ずつ変更(ミュータント)を入れて、再ビルドし、テストが落ちることを確認する。
対象: video_status.c の video_status_format_json、ws_video_core.c の状態の通知(status_due ほか)。
結果は docs/VIDEO_NOTIFY_TESTS.md の表。ミュータントの定義は video_notify_mutants.py(同じディレクトリ)。
各ミュータントは "file"(video_status / ws_video_core)で対象のファイルを持つ。同値と判断したものは "equiv" に理由を持つ。
使い方:
  python3 tools/test/mutate_video_notify.py <repo> <workdir> tools/test/video_notify_mutants.py <out.json> [id ...]
repo の components/ と test/ を workdir にコピーして使う(repo は変更しない)。Unity の取得にネットワークが要る。
ソースを変えたら、ミュータントの old 文字列(1 回だけ現れる文字列)も更新する(合わないと BAD EDIT で止まる)。
実行するテスト: video_status のミュータントは test/host/video_status/ の全部、ws_video_core のミュータントは
test/host/ws_video_core/ の全部。1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。
新しいテストだけで殺せたか(killed_by_new)も記録する。手本は mutate_video_core.py。"""
import glob, json, os, re, shutil, subprocess, sys, importlib.util


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


repo, work, mfile, outp = sys.argv[1:5]
only = set(sys.argv[5:])
spec = importlib.util.spec_from_file_location("mutants", mfile)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
MUTANTS = mod.MUTANTS

work = os.path.realpath(work)
src_tree = os.path.join(work, "repo")
build = os.path.join(work, "build")
check_workdir(repo, src_tree, build)  # 何も消す前に確認する
if os.path.exists(src_tree):
    shutil.rmtree(src_tree)
if os.path.exists(build):
    # 前回のビルドの残り(最後のミュータントのオブジェクト)を使わない。コピーしたソースの更新時刻は、リポジトリのもの
    # (古い)なので、残したままだと、make が古いオブジェクトを「最新」と判断して、ミュータントのままリンクしてしまう
    shutil.rmtree(build)
os.makedirs(src_tree)
for d in ("components", "test"):
    shutil.copytree(os.path.join(repo, d), os.path.join(src_tree, d))

FILES = {
    "video_status": os.path.join(src_tree, "components/video_status/video_status.c"),
    "ws_video_core": os.path.join(src_tree, "components/ws_video_core/ws_video_core.c"),
}
ORIG = {k: open(v).read() for k, v in FILES.items()}
NEW_TEST = {"video_status": "test_video_status_format", "ws_video_core": "test_ws_video_core_notify"}


def tests_of(dirname):
    return sorted(os.path.splitext(os.path.basename(p))[0]
                  for p in glob.glob(os.path.join(src_tree, "test/host", dirname, "test_*.c")))


TESTS = {"video_status": tests_of("video_status"), "ws_video_core": tests_of("ws_video_core")}


r = sh(["cmake", "-S", os.path.join(src_tree, "test", "host"), "-B", build])
if r.returncode:
    print(r.stdout, r.stderr)
    sys.exit(1)


def build_and_run(group):
    tests = TESTS[group]
    b = sh(["cmake", "--build", build, "-j8", "--target", *tests])
    if b.returncode != 0:
        return "build_error", [], [(b.stdout[-800:] + b.stderr[-800:])]
    failed = []
    names = []
    for t in tests:
        try:
            p = sh([os.path.join(build, t)], timeout=120)
        except subprocess.TimeoutExpired:
            failed.append(t)
            names.append(t + ":timeout")
            continue
        bad = p.returncode != 0 or ":FAIL" in p.stdout or "runtime error" in p.stderr or "AddressSanitizer" in p.stderr
        if bad:
            failed.append(t)
            found = False
            for line in p.stdout.splitlines():
                m = re.match(r".*:(\w+):FAIL", line)
                if m:
                    names.append(m.group(1))
                    found = True
            if not found:
                names.append(t + ":crash/rc=%d" % p.returncode)
    return ("killed" if failed else "survived"), failed, names


for g in TESTS:
    st, failed, names = build_and_run(g)
    print("baseline", g, ":", st, failed, names, flush=True)
    if st != "survived":
        print("baseline must pass")
        sys.exit(1)

results = []
for m in MUTANTS:
    if only and m["id"] not in only:
        continue
    g = m["file"]
    target_file = FILES[g]
    orig = ORIG[g]
    text = orig
    for old, new in m["edits"]:
        if text.count(old) != 1:
            print("BAD EDIT", m["id"], text.count(old), repr(old[:80]))
            sys.exit(2)
        text = text.replace(old, new)
    if text == orig:
        print("NO-OP", m["id"])
        sys.exit(2)
    open(target_file, "w").write(text)
    st, failed, names = build_and_run(g)
    results.append({"id": m["id"], "file": g, "desc": m["desc"], "status": st, "failed_exes": failed,
                    "failing_tests": names[:6], "equiv": m.get("equiv"),
                    "killed_by_new": NEW_TEST[g] in failed})
    note = ""
    if st == "survived" and not m.get("equiv"):
        note = "  <-- UNEXPECTED SURVIVOR"
    if st != "survived" and m.get("equiv"):
        note = "  <-- marked equivalent but killed"
    print(f'{m["id"]:5} {st:11} {m["desc"][:72]}  -> {",".join(names[:2]) if st != "survived" else ""}{note}', flush=True)
    open(target_file, "w").write(orig)

json.dump(results, open(outp, "w"), ensure_ascii=False, indent=1)
print("run:", len(results), "killed:", sum(r["status"] == "killed" for r in results),
      "survived:", sum(r["status"] == "survived" for r in results),
      "build_error:", sum(r["status"] == "build_error" for r in results),
      "unexpected_survivors:", sum(r["status"] == "survived" and not r["equiv"] for r in results),
      "equiv_but_killed:", sum(r["status"] != "survived" and bool(r["equiv"]) for r in results),
      "killed_by_new_test_alone:", sum(r["killed_by_new"] for r in results))
