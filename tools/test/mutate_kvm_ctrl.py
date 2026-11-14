#!/usr/bin/env python3
"""kvm_ctrl.c に 1 か所ずつ変更(ミュータント)を入れて、再ビルドし、kvm_ctrl の全テストが落ちることを確認する。
結果は docs/KVM_CTRL_TESTS.md の表。ミュータントの定義は kvm_ctrl_mutants.py(同じディレクトリ)。
使い方:
  python3 tools/test/mutate_kvm_ctrl.py <repo> <workdir> tools/test/kvm_ctrl_mutants.py <out.json> [id ...]
repo の components/ と test/ を workdir にコピーして使う(repo は変更しない)。Unity の取得にネットワークが要る。
kvm_ctrl.c を変えたら、ミュータントの old 文字列(1 回だけ現れる文字列)も更新する(合わないと BAD EDIT で止まる)。"""
import json, os, re, shutil, subprocess, sys, importlib.util

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
target_file = os.path.join(src_tree, "components/kvm_ctrl/kvm_ctrl.c")
orig = open(target_file).read()
TESTS = ["test_kvm_ctrl_init", "test_kvm_ctrl_ops", "test_kvm_ctrl_fault", "test_kvm_ctrl_fuzz"]


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)


r = sh(f"cmake -S {src_tree}/test/host -B {build}")
if r.returncode:
    print(r.stdout, r.stderr)
    sys.exit(1)


def build_and_run():
    b = sh(f"cmake --build {build} -j8 --target {' '.join(TESTS)}")
    if b.returncode != 0:
        return "build_error", [], [(b.stdout[-800:] + b.stderr[-800:])]
    failed = []
    names = []
    for t in TESTS:
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


open(target_file, "w").write(orig)
st, failed, names = build_and_run()
print("baseline:", st, failed, names, flush=True)
if st != "survived":
    print("baseline must pass")
    sys.exit(1)

results = []
for m in MUTANTS:
    if only and m["id"] not in only:
        continue
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
    st, failed, names = build_and_run()
    results.append({"id": m["id"], "desc": m["desc"], "status": st, "failed_exes": failed, "failing_tests": names[:6]})
    print(f'{m["id"]:5} {st:11} {m["desc"][:72]}  -> {",".join(names[:2]) if st != "survived" else ""}', flush=True)
    open(target_file, "w").write(orig)

json.dump(results, open(outp, "w"), ensure_ascii=False, indent=1)
print("run:", len(results), "killed:", sum(r["status"] == "killed" for r in results),
      "survived:", sum(r["status"] == "survived" for r in results),
      "build_error:", sum(r["status"] == "build_error" for r in results))
