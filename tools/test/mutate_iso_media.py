#!/usr/bin/env python3
"""iso_media.c に 1 か所ずつミュータントを入れて再ビルドし、
test_iso_media と test_mmc_scsi_* が全ミュータントを殺せることを確認する（ミュータントの定義は iso_media_mutants.py）。

使い方:
  python3 tools/test/mutate_iso_media.py <repo> <workdir> tools/test/iso_media_mutants.py <out.json> [id ...]

repo の components/ と test/ を workdir にコピーして使う（repo は変更しない）。
Unity の取得にネットワークが要る（すでに取得済みなら再構成はキャッシュ可）。"""
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys


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


def main():
    if len(sys.argv) < 5:
        print("usage: mutate_iso_media.py <repo> <workdir> <mutants.py> <out.json> [id ...]", file=sys.stderr)
        sys.exit(2)

    repo, work, mfile, outp = sys.argv[1:5]
    only = set(sys.argv[5:])

    spec = importlib.util.spec_from_file_location("mutants", mfile)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mutants = mod.MUTANTS

    # 指定された ID が MUTANTS にないときは、何も作らずに（ビルドの前に）終了コード 2 で終わる。
    # 有効な ID と未知の ID が混ざっていても拒否する（打ち間違いで、黙って一部だけ実行しない）。引数なしは全数。
    known = [m["id"] for m in mutants]
    if len(set(known)) != len(known):
        print("ERROR: duplicate mutant ids in", mfile, file=sys.stderr)
        sys.exit(2)
    unknown = sorted(only - set(known))
    if unknown:
        print("ERROR: unknown mutant id(s): " + " ".join(unknown), file=sys.stderr)
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

    tests = ["test_iso_media", "test_mmc_scsi_cmds", "test_mmc_scsi_data", "test_mmc_scsi_fuzz"]
    r = sh(["cmake", "-S", os.path.join(src_tree, "test", "host"), "-B", build])
    if r.returncode:
        print(r.stdout, r.stderr, file=sys.stderr)
        sys.exit(1)

    def build_and_run():
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
            bad = (
                p.returncode != 0
                or ":FAIL" in p.stdout
                or "runtime error" in p.stderr
                or "AddressSanitizer" in p.stderr
            )
            if bad:
                failed.append(t)
                found = False
                for line in p.stdout.splitlines():
                    m = re.match(r".*:(\w+):FAIL", line)
                    if m:
                        names.append(m.group(1))
                        found = True
                if not found:
                    names.append(f"{t}:crash/rc={p.returncode}")
                break  # 1 つ落ちれば殺した（残りは走らせない）
        return ("killed" if failed else "survived"), failed, names

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
        rel_file = m.get("file", "components/iso_media/iso_media.c")
        target_file = os.path.join(src_tree, rel_file)
        if target_file not in orig_cache:
            orig_cache[target_file] = open(target_file).read()
        orig = orig_cache[target_file]
        text = orig
        for old, new in m["edits"]:
            cnt = text.count(old)
            if cnt != 1:
                print("BAD EDIT", m["id"], rel_file, cnt, repr(old[:80]), file=sys.stderr)
                sys.exit(2)
            text = text.replace(old, new)
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
