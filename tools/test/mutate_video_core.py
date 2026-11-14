#!/usr/bin/env python3
"""ws_video_core.c と http_guard.c(http_guard_origin_ok_for_port)に 1 か所ずつ変更(ミュータント)を入れて、
再ビルドし、対応するテストが落ちることを確認する。結果は docs/VIDEO_CORE_TESTS.md の表。
ミュータントの定義は video_core_mutants.py(同じディレクトリ)。各ミュータントは "file" で対象のファイルを持つ。
使い方:
  python3 tools/test/mutate_video_core.py <repo> <workdir> tools/test/video_core_mutants.py <out.json> [id ...]
repo の components/ と test/ を workdir にコピーして使う(repo は変更しない)。Unity の取得にネットワークが要る。
ソースを変えたら、ミュータントの old 文字列(1 回だけ現れる文字列)も更新する(合わないと BAD EDIT で止まる)。
実行するテスト: ws_video_core.c のミュータントは test/host/ws_video_core/ の全部、http_guard.c のミュータントは
test/host/http_guard/ の全部。1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。"""
import glob, json, os, re, shutil, subprocess, sys, importlib.util

repo, work, mfile, outp = sys.argv[1:5]
only = set(sys.argv[5:])
spec = importlib.util.spec_from_file_location("mutants", mfile)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
MUTANTS = mod.MUTANTS

src_tree = os.path.join(work, "repo")
build = os.path.join(work, "build")
if os.path.exists(src_tree):
    shutil.rmtree(src_tree)
os.makedirs(src_tree)
for d in ("components", "test"):
    shutil.copytree(os.path.join(repo, d), os.path.join(src_tree, d))

FILES = {
    "ws_video_core": os.path.join(src_tree, "components/ws_video_core/ws_video_core.c"),
    "http_guard": os.path.join(src_tree, "components/http_guard/http_guard.c"),
}
ORIG = {k: open(v).read() for k, v in FILES.items()}


def tests_of(dirname):
    return sorted(os.path.splitext(os.path.basename(p))[0]
                  for p in glob.glob(os.path.join(src_tree, "test/host", dirname, "test_*.c")))


TESTS = {"ws_video_core": tests_of("ws_video_core"), "http_guard": tests_of("http_guard")}


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)


r = sh(f"cmake -S {src_tree}/test/host -B {build}")
if r.returncode:
    print(r.stdout, r.stderr)
    sys.exit(1)


def build_and_run(group):
    tests = TESTS[group]
    b = sh(f"cmake --build {build} -j8 --target {' '.join(tests)}")
    if b.returncode != 0:
        return "build_error", [], [(b.stdout[-800:] + b.stderr[-800:])]
    failed = []
    names = []
    for t in tests:
        try:
            p = sh(f"{build}/{t}", timeout=120)
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
    results.append({"id": m["id"], "file": g, "desc": m["desc"], "status": st, "failed_exes": failed, "failing_tests": names[:6]})
    print(f'{m["id"]:5} {st:11} {m["desc"][:72]}  -> {",".join(names[:2]) if st != "survived" else ""}', flush=True)
    open(target_file, "w").write(orig)

json.dump(results, open(outp, "w"), ensure_ascii=False, indent=1)
print("run:", len(results), "killed:", sum(r["status"] == "killed" for r in results),
      "survived:", sum(r["status"] == "survived" for r in results),
      "build_error:", sum(r["status"] == "build_error" for r in results))
