#!/usr/bin/env python3
"""ミュータントの定義(tools/test/iso_upload_mutants.py、iso_core_mutants.py)を、ソースから機械的に作る。演算子の置換(関係、論理、算術)、
if 条件の否定、文の削除、定数の ±1、真偽値の反転。old の文字列がソースの中で 1 回だけ現れるように、前後の行を足す。
コメントの行は変えない。ミュータントごとに、先頭の #include の後ろに、-Werror の警告(括弧の省略、未使用の変数など)を無効にする #pragma を足す
(ミュータントは、わざとコードを壊すので、コンパイラの警告でビルドできなくなるものを、ビルドエラーとして数えない)。
使い方(リポジトリのルートで):
  python3 tools/test/gen_iso_mutants.py upload . > tools/test/iso_upload_mutants.py
  python3 tools/test/gen_iso_mutants.py core . > tools/test/iso_core_mutants.py
  python3 tools/test/gen_iso_mutants.py api . > tools/test/iso_api_mutants.py"""
import re
import sys


def mask_strings(code):
    out = []
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c in "\"'":
            q = c
            j = i + 1
            while j < n and code[j] != q:
                if code[j] == "\\":
                    j += 1
                j += 1
            out.append(q + "_" * (j - i - 1) + (q if j < n else ""))
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def split_comment(line):
    """コメントの前までをコードとして返す(文字列の中の // は見ない)"""
    masked = mask_strings(line)
    cut = len(line)
    m = re.search(r"/\*|//", masked)
    if m:
        cut = m.start()
    return line[:cut], line[cut:]


DECL = re.compile(r"^\s*(const\s+)?(static\s+)?(u?int\d*_t|size_t|bool|int|char|unsigned|iso_\w+|up_\w+|enum)\b[\s\*]+\w+(\[[^\]]*\])?\s*(=|;)")

REL = [("==", "!="), ("!=", "=="), ("<=", "<"), (">=", ">"), ("<", "<="), (">", ">=")]


def find_cond(code, start):
    """code[start] が '(' の位置。対応する ')' の位置を返す"""
    depth = 0
    for k in range(start, len(code)):
        if code[k] == "(":
            depth += 1
        elif code[k] == ")":
            depth -= 1
            if depth == 0:
                return k
    return -1


def line_variants(line):
    code, comment = split_comment(line)
    masked = mask_strings(code)
    stripped = masked.strip()
    if not stripped or stripped.startswith(("#", "*", "/*", "//")) or stripped in ("{", "}", "};"):
        return []
    if re.match(r"^\s*(static\s+)?(inline\s+)?(const\s+)?[\w\*\s]+\s+\*?\w+\(.*\)\s*\{?\s*$", masked) and not stripped.startswith(("if", "while", "for", "return", "else", "switch")):
        return []  # 関数の定義の行
    if stripped.startswith(("case ", "default:", "enum", "typedef")):
        return []
    res = []  # (desc, newline)

    def add(desc, new_code):
        nl = new_code + comment
        if nl != line:
            res.append((desc, nl))

    # 関係演算子(条件の行だけ)
    cond_line = re.search(r"\b(if|while|for|return)\b|\?", masked) is not None
    if cond_line:
        for op, rep in REL:
            for m in re.finditer(re.escape(op), masked):
                s, e = m.span()
                before = masked[s - 1] if s > 0 else " "
                after = masked[e] if e < len(masked) else " "
                if op in ("<", ">") and (before in "<>-=!" or after in "<>=" or (op == ">" and before == "-")):
                    continue
                if op == "<" and re.search(r"#include\s*<", masked):
                    continue
                if op in ("==", "!=", "<=", ">=") and (before in "<>=!" or after == "="):
                    continue
                add("%s -> %s" % (op, rep), code[:s] + rep + code[e:])
        for m in re.finditer(r"&&", masked):
            add("&& -> ||", code[: m.start()] + "||" + code[m.end():])
        for m in re.finditer(r"\|\|", masked):
            add("|| -> &&", code[: m.start()] + "&&" + code[m.end():])
    # if (cond) の否定
    for m in re.finditer(r"\bif \(", masked):
        p = m.end() - 1
        q = find_cond(masked, p)
        if q > 0:
            add("negate if condition", code[: p + 1] + "!(" + code[p + 1: q] + ")" + code[q:])
    # 算術
    for m in re.finditer(r" (\+|-|\*|/) ", masked):
        s = m.start(1)
        rep = {"+": "-", "-": "+", "*": "/", "/": "*"}[m.group(1)]
        add("%s -> %s" % (m.group(1), rep), code[:s] + rep + code[s + 1:])
    for m in re.finditer(r"(\+=|-=)", masked):
        rep = "-=" if m.group(1) == "+=" else "+="
        add("%s -> %s" % (m.group(1), rep), code[: m.start()] + rep + code[m.end():])
    # 定数
    for m in re.finditer(r"(?<![\w\.])(\d+)(u|ull|ULL)?(?![\w\.])", masked):
        n = int(m.group(1))
        suf = m.group(2) or ""
        s, e = m.span()
        for d, nv in (("+1", n + 1), ("-1", n - 1)):
            if nv < 0 or (n == 0 and d == "-1"):
                continue
            if re.match(r"^\s*(static\s+)?const", masked) or "[" in masked[max(0, s - 2): s]:
                continue
            add("const %d%s -> %d" % (n, d, nv), code[:s] + str(nv) + suf + code[e:])
    # 真偽値
    for m in re.finditer(r"\b(true|false)\b", masked):
        rep = "false" if m.group(1) == "true" else "true"
        add("%s -> %s" % (m.group(1), rep), code[: m.start()] + rep + code[m.end():])
    # 文の削除
    if masked.rstrip().endswith(";") and not DECL.match(masked) and not stripped.startswith(("return", "break", "continue", "goto", "for", "typedef")):
        if re.match(r"^\s*(\(void\)\s*)?[\w\->\.\[\]\*\(\)]+\s*(=|\+=|-=|\(|\+\+|--)", masked) or re.match(r"^\s*\w+\+\+;", masked):
            indent = re.match(r"^\s*", code).group(0)
            add("delete statement", indent + ";")
    return res


def gen(path, prefix, file_const, inc):
    src = open(path).read()
    lines = src.split("\n")
    out = []
    seen = set()
    nid = 1
    for i, line in enumerate(lines):
        for desc, newline in line_variants(line):
            old, new = line, newline
            # 一意にする
            lo, hi = i, i
            while src.count("\n".join(lines[lo: hi + 1])) != 1:
                if lo > 0 and (hi - lo) % 2 == 0:
                    lo -= 1
                elif hi + 1 < len(lines):
                    hi += 1
                else:
                    break
                if hi - lo > 6:
                    break
            old = "\n".join(lines[lo: hi + 1])
            if src.count(old) != 1:
                continue
            new_block = "\n".join(lines[lo:i] + [newline] + lines[i + 1: hi + 1])
            key = (old, new_block)
            if key in seen:
                continue
            seen.add(key)
            out.append((("%s%03d" % (prefix, nid)), "L%d: %s" % (i + 1, desc), old, new_block, file_const, inc))
            nid += 1
    return out


PRAGMAS = '''
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Warray-parameter"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wreturn-type"'''


def render(items):
    """ミュータントの行。-Werror の警告を無効にする #pragma(PRAGMAS)と、それを足す先の #include(INC)は、出力の先頭に 1 回だけ書く(各行では参照する)。"""
    lines = []
    for id_, desc, old, new, f, inc in items:
        lines.append("    M(%r, %r, (%r, %r), (INC, INC + PRAGMAS), file=%s)," % (id_, desc, old, new, f))
    return "\n".join(lines)


def pragma_header(inc):
    return "INC = %r\nPRAGMAS = %r\n" % (inc, PRAGMAS)


USAGE = "usage: gen_iso_mutants.py {upload|core|api} <repo-root>   (標準出力に、ミュータントの Python ソースを書く)\n"


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("upload", "core", "api"):
        sys.stderr.write(USAGE)
        sys.exit(2)
    mode = sys.argv[1]
    root = sys.argv[2]
    if mode == "upload":
        files = [("components/iso_upload/iso_upload.c", "U", "UP", '#include "iso_upload_priv.h"'), ("components/iso_upload/iso_finish.c", "F", "FN", '#include "iso_upload_priv.h"'),
                 ("components/iso_upload/iso_index.c", "X", "IX", '#include "iso_upload_priv.h"')]
        inc_line = '#include "iso_upload_priv.h"'
        header = '''# iso_upload.c / iso_finish.c / iso_index.c のミュータント。tools/test/gen_iso_mutants.py(演算子の置換、if 条件の否定、文の削除、定数 ±1、真偽値の反転)が
# ソースから機械的に作ったもの。old がソースの中で 1 回だけ現れるよう、前後の行を足してある。ソースを変えたら、gen_iso_mutants.py で作り直す。
UP = "components/iso_upload/iso_upload.c"
FN = "components/iso_upload/iso_finish.c"
IX = "components/iso_upload/iso_index.c"


def M(id, desc, *edits, file=UP):
    return {"id": id, "desc": desc, "edits": list(edits), "file": file}


MUTANTS = [
'''
    elif mode == "api":
        files = [("components/iso_api/iso_api.c", "A", "AP", '#include "iso_api_priv.h"'), ("components/iso_api/iso_api_json.c", "J", "JS", '#include "iso_api_priv.h"'),
                 ("components/iso_api/iso_api_fmt.c", "M", "FM", '#include "iso_api_priv.h"')]
        inc_line = '#include "iso_api_priv.h"'
        header = '''# iso_api.c / iso_api_json.c / iso_api_fmt.c のミュータント。tools/test/gen_iso_mutants.py が、ソースから機械的に作ったもの(iso_upload_mutants.py と同じ方法)。
AP = "components/iso_api/iso_api.c"
JS = "components/iso_api/iso_api_json.c"
FM = "components/iso_api/iso_api_fmt.c"


def M(id, desc, *edits, file=AP):
    return {"id": id, "desc": desc, "edits": list(edits), "file": file}


MUTANTS = [
'''
    else:
        files = [("components/iso_core/iso_core.c", "K", "CO", '#include "iso_core.h"')]
        inc_line = '#include "iso_core.h"'
        header = '''# iso_core.c のミュータント。tools/test/gen_iso_mutants.py が、ソースから機械的に作ったもの(iso_upload_mutants.py と同じ方法)。
CO = "components/iso_core/iso_core.c"


def M(id, desc, *edits, file=CO):
    return {"id": id, "desc": desc, "edits": list(edits), "file": file}


MUTANTS = [
'''
    items = []
    for path, prefix, const, inc in files:
        items += gen(root + "/" + path, prefix, const, inc)
    assert all(it[5] == inc_line for it in items)  # #include は、モードごとに 1 つ
    i = header.index("\n\ndef M(")
    sys.stdout.write(header[:i] + "\n" + pragma_header(inc_line) + header[i:] + render(items) + "\n]\n")
    sys.stderr.write("%d mutants\n" % len(items))
