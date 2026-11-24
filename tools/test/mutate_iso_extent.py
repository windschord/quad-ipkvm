#!/usr/bin/env python3
"""iso_extent.c に 1 か所ずつ変更(ミュータント)を入れて、再ビルドし、iso_extent のテストが落ちることを確認する。
結果は docs/ISO_EXTENT_TESTS.md の表。ミュータントの定義は iso_extent_mutants.py(同じディレクトリ)。
使い方:
  python3 tools/test/mutate_iso_extent.py <repo> <workdir> tools/test/iso_extent_mutants.py <out.json> [-jN] [id ...]
repo の components/ と test/ を workdir にコピーして使う(repo は変更しない)。Unity の取得にネットワークが要る
(環境変数 UNITY_SRC に取得済みの Unity のソースのパスがあれば、それを使う)。
ミュータントの old 文字列(1 回だけ現れる文字列)が対象のファイルと合わないと BAD EDIT で止まる(ソースを変えたら更新する)。
-jN: N 個のワーカー(それぞれ別のコピーとビルドの場所)で並列に実行する(既定 1)。

iso_catalog の mutate_iso_catalog.py も、この main() を使う(対象のファイル・テストの実行ファイル・ビルドの対象が違うだけ)。"""
import concurrent.futures
import types
import json
import os
import re
import shutil
import subprocess
import sys


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


class Worker:
    def __init__(self, repo, work, tests, files):
        self.tests = tests
        self.src_tree = os.path.join(work, "repo")
        self.build = os.path.join(work, "build")
        check_workdir(repo, self.src_tree, self.build)  # 何も消す前に確認する
        if os.path.exists(self.src_tree):
            shutil.rmtree(self.src_tree)
        if os.path.exists(self.build):
            shutil.rmtree(self.build)
        os.makedirs(self.src_tree)
        for d in ("components", "test"):
            shutil.copytree(os.path.join(repo, d), os.path.join(self.src_tree, d))
        self.path = {f: os.path.join(self.src_tree, f) for f in files}
        self.orig = {f: open(p).read() for f, p in self.path.items()}
        cmd = ["cmake", "-S", os.path.join(self.src_tree, "test", "host"), "-B", self.build]
        if os.environ.get("UNITY_SRC"):
            cmd.append("-DFETCHCONTENT_SOURCE_DIR_UNITY=" + os.environ["UNITY_SRC"])
        r = sh(cmd)
        if r.returncode:
            print(r.stdout, r.stderr)
            sys.exit(1)

    def restore(self):
        for f, p in self.path.items():
            open(p, "w").write(self.orig[f])

    def build_and_run(self):
        b = sh(["cmake", "--build", self.build, "-j4", "--target", *self.tests])
        if b.returncode != 0:
            return "build_error", [], [(b.stdout[-800:] + b.stderr[-800:])]
        failed = []
        names = []
        for t in self.tests:
            try:
                p = sh([os.path.join(self.build, t)], timeout=180)
            except subprocess.TimeoutExpired:
                failed.append(t)
                names.append(t + ":timeout")
                break
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
                break  # 1 つ落ちれば殺した(残りは実行しない)
        return ("killed" if failed else "survived"), failed, names

    def run_mutant(self, m, default_file):
        f = m.get("file", default_file)
        text = self.orig[f]
        for old, new in m["edits"]:
            if text.count(old) != 1:
                print("BAD EDIT", m["id"], f, text.count(old), repr(old[:80]))
                sys.exit(2)
            text = text.replace(old, new)
        if text == self.orig[f]:
            print("NO-OP", m["id"])
            sys.exit(2)
        open(self.path[f], "w").write(text)
        try:
            st, failed, names = self.build_and_run()
        finally:
            self.restore()
        return {"id": m["id"], "desc": m["desc"], "file": f, "status": st, "failed_exes": failed, "failing_tests": names[:6]}


def main(config):
    argv = [a for a in sys.argv[1:]]
    jobs = 1
    rest = []
    for a in argv:
        if re.fullmatch(r"-j\d+", a):
            jobs = int(a[2:])
        else:
            rest.append(a)
    if len(rest) < 4:
        print("usage: %s <repo> <workdir> <mutants.py> <out.json> [-jN] [id ...]" % sys.argv[0], file=sys.stderr)
        sys.exit(2)
    repo, work, mfile, outp = rest[:4]
    only = set(rest[4:])
    check_workdir(repo, outp)  # 結果の JSON を、リポジトリ(作業ツリー)の中に書かない(既存のファイルを上書きしない)
    # ミュータントの定義は、ソースをそのまま読む(import の仕組みを通さない: 既存の __pycache__ の .pyc を使わず、新しい .pyc も作らない)
    mod = types.ModuleType("mutants")
    with open(mfile, "rb") as fh:
        exec(compile(fh.read(), mfile, "exec"), mod.__dict__)
    unknown = sorted(only - {m["id"] for m in mod.MUTANTS})
    if unknown:  # ビルドの前に、指定した ID を照合する(打ち間違いで、何も実行せずに成功しない)
        print("ERROR: unknown mutant id(s): " + " ".join(unknown), file=sys.stderr)
        sys.exit(2)
    mutants = [m for m in mod.MUTANTS if not only or m["id"] in only]
    files = config["files"]
    tests = config["tests"]
    work = os.path.realpath(work)

    workers = [Worker(repo, os.path.join(work, "w%d" % i), tests, files) for i in range(jobs)]
    # ベースライン(ミュータントなし)は、全テストが成功すること
    st, failed, names = workers[0].build_and_run()
    print("baseline:", st, failed, names, flush=True)
    if st != "survived":
        print("baseline must pass")
        sys.exit(1)

    results = {}

    def work_fn(i):
        w = workers[i]
        for k, m in enumerate(mutants):
            if k % jobs != i:
                continue
            r = w.run_mutant(m, files[0])
            results[m["id"]] = r
            print(f'{r["id"]:5} {r["status"]:11} {r["desc"][:72]}  -> {",".join(r["failing_tests"][:2]) if r["status"] != "survived" else ""}', flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for f in [ex.submit(work_fn, i) for i in range(jobs)]:
            f.result()
    out = [results[m["id"]] for m in mutants]
    json.dump(out, open(outp, "w"), ensure_ascii=False, indent=1)
    print("run:", len(out), "killed:", sum(r["status"] == "killed" for r in out),
          "survived:", sum(r["status"] == "survived" for r in out),
          "build_error:", sum(r["status"] == "build_error" for r in out))


if __name__ == "__main__":
    main({
        "files": ["components/iso_extent/iso_extent.c"],
        "tests": ["test_iso_extent_build", "test_iso_extent_map", "test_iso_extent_fuzz"],
    })
