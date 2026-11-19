#!/usr/bin/env python3
"""msc_bot.c に 1 か所ずつミュータントを入れて再ビルドし、test_msc_bot_* が全ミュータントを殺せることを確認する。

使い方:
  python3 tools/test/mutate_msc_bot.py <repo> <workdir> tools/test/msc_bot_mutants.py <out.json> [id ...]
  python3 tools/test/mutate_msc_bot.py --check-only <repo> <mutants.py>     # old 文字列が 1 回だけ現れるかの検査だけ

repo の components/ と test/ を workdir にコピーして使う（repo は変更しない）。
Unity の取得にネットワークが要る。取得済みの Unity を使うときは、環境変数 UNITY_SRC に、そのソースの
ディレクトリを指定する（FETCHCONTENT_SOURCE_DIR_UNITY として渡す）。
1 つのミュータントで、最初に失敗した実行ファイルで止める（速さのため。殺したテストの名前は、その実行ファイルの分）。"""
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys

DEFAULT_FILE = "components/msc_bot/msc_bot.c"
# 速く失敗するものから（表、CBW、13 のケース、ライフサイクル、データ、ファズ）
TESTS = [
    "test_msc_bot_table",
    "test_msc_bot_cbw",
    "test_msc_bot_cases",
    "test_msc_bot_lifecycle",
    "test_msc_bot_data",
    "test_msc_bot_fuzz",
]


def check_workdir(repo, *paths):
    repo = os.path.realpath(repo)
    for p in paths:
        p = os.path.realpath(p)
        common = os.path.commonpath([p, repo])
        if common == p or common == repo:
            print(f"ERROR: work path {p} overlaps the repo {repo}; refusing to delete or write anything", file=sys.stderr)
            sys.exit(2)


def sh(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, **kw)
    except FileNotFoundError as e:
        return subprocess.CompletedProcess(cmd, 127, "", str(e))


def load_mutants(mfile):
    spec = importlib.util.spec_from_file_location("mutants", mfile)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.MUTANTS


def apply_edits(m, text):
    for old, new in m["edits"]:
        cnt = text.count(old)
        if cnt != 1:
            print("BAD EDIT", m["id"], cnt, repr(old[:80]), file=sys.stderr)
            sys.exit(2)
        text = text.replace(old, new)
    return text


def check_only(repo, mfile):
    mutants = load_mutants(mfile)
    ids = set()
    bad = 0
    for m in mutants:
        if m["id"] in ids:
            print("DUPLICATE ID", m["id"], file=sys.stderr)
            bad += 1
        ids.add(m["id"])
        path = os.path.join(repo, m.get("file", DEFAULT_FILE))
        text = orig = open(path).read()
        for old, new in m["edits"]:
            cnt = text.count(old)
            if cnt != 1:
                print("BAD EDIT", m["id"], cnt, repr(old[:80]), file=sys.stderr)
                bad += 1
                break
            text = text.replace(old, new)
        else:
            if text == orig:
                print("NO-OP", m["id"], file=sys.stderr)
                bad += 1
    if bad:
        sys.exit(2)
    print(f"ok: {len(mutants)} mutants apply cleanly")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--check-only":
        check_only(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) < 5:
        print("usage: mutate_msc_bot.py <repo> <workdir> <mutants.py> <out.json> [id ...]", file=sys.stderr)
        sys.exit(2)

    repo, work, mfile, outp = sys.argv[1:5]
    only = set(sys.argv[5:])
    mutants = load_mutants(mfile)

    # 未知のミュータント ID は、作業領域を削除する前に、エラーで終了する（タイプミスで全て空振りにしない）。
    unknown = only - {m["id"] for m in mutants}
    if unknown:
        print("ERROR: unknown mutant id(s): " + ", ".join(sorted(unknown)), file=sys.stderr)
        sys.exit(2)

    work = os.path.realpath(work)
    src_tree = os.path.join(work, "repo")
    build = os.path.join(work, "build")
    check_workdir(repo, src_tree, build)
    if os.path.exists(src_tree):
        shutil.rmtree(src_tree)
    if os.path.exists(build):
        shutil.rmtree(build)
    os.makedirs(src_tree)
    for d in ("components", "test"):
        shutil.copytree(os.path.join(repo, d), os.path.join(src_tree, d))

    cfg = ["cmake", "-S", os.path.join(src_tree, "test", "host"), "-B", build]
    if os.environ.get("UNITY_SRC"):
        cfg.append("-DFETCHCONTENT_SOURCE_DIR_UNITY=" + os.environ["UNITY_SRC"])
    r = sh(cfg)
    if r.returncode:
        print(r.stdout, r.stderr, file=sys.stderr)
        sys.exit(1)

    def build_and_run():
        b = sh(["cmake", "--build", build, "-j8", "--target", *TESTS])
        if b.returncode != 0:
            return "build_error", [], [(b.stdout[-800:] + b.stderr[-800:])]
        for t in TESTS:
            try:
                p = sh([os.path.join(build, t)], timeout=300)
            except subprocess.TimeoutExpired:
                return "killed", [t], [t + ":timeout"]
            bad = (
                p.returncode != 0
                or ":FAIL" in p.stdout
                or "runtime error" in p.stderr
                or "AddressSanitizer" in p.stderr
            )
            if bad:
                names = []
                for line in p.stdout.splitlines():
                    m = re.match(r".*:(\w+):FAIL", line)
                    if m:
                        names.append(m.group(1))
                if not names:
                    names.append(f"{t}:crash/rc={p.returncode}")
                return "killed", [t], names
        return "survived", [], []

    st, failed, names = build_and_run()
    print("baseline:", st, failed, names, flush=True)
    if st != "survived":
        print("baseline must pass", file=sys.stderr)
        sys.exit(1)

    orig_cache = {}
    results = []
    for m in mutants:
        if only and m["id"] not in only:
            continue
        rel_file = m.get("file", DEFAULT_FILE)
        target_file = os.path.join(src_tree, rel_file)
        if target_file not in orig_cache:
            orig_cache[target_file] = open(target_file).read()
        orig = orig_cache[target_file]
        text = apply_edits(m, orig)
        if text == orig:
            print("NO-OP", m["id"], file=sys.stderr)
            sys.exit(2)
        open(target_file, "w").write(text)
        try:
            st, failed, names = build_and_run()
        finally:
            open(target_file, "w").write(orig)
        results.append({
            "id": m["id"],
            "file": rel_file,
            "desc": m["desc"],
            "status": st,
            "failed_exes": failed,
            "failing_tests": names[:6],
        })
        print(f'{m["id"]:5} {st:11} {m["desc"][:72]}  -> {",".join(names[:2]) if st != "survived" else ""}', flush=True)

    with open(outp, "w") as fp:
        json.dump(results, fp, ensure_ascii=False, indent=1)
    killed = sum(r["status"] == "killed" for r in results)
    survived = sum(r["status"] == "survived" for r in results)
    build_err = sum(r["status"] == "build_error" for r in results)
    print(f"run: {len(results)} killed: {killed} survived: {survived} build_error: {build_err}")
    if survived or build_err:
        sys.exit(1)


if __name__ == "__main__":
    main()
