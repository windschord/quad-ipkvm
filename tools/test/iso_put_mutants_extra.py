# web_iso_put_logic.c の、手書きのミュータント(C-16)。tools/test/gen_iso_mutants.py は、次の形を作らないので、ここで足す:
# 引数の検査の項を 1 つずつ落とす、戻り値の取り違え(INVALID / IO / TIMEOUT / OK)、三項演算子の枝の入れ替えと落とし、コールバックの引数
# (ctx を NULL にする、buf の位置、recv の長さ、set_timeout のスライス)、`*last_bucket = bucket;`(行の先頭が `*` なので機械は作らない)、
# `return bucket * 5;`、`if (bucket <= 0 || ...)` の項を落とす、大きい合計の経路の防御(到達しないもの)。iso_put_mutants.py(機械が作る)とは別に、同じスクリプトで実行する:
#   python3 tools/test/mutate_iso_put.py <repo> <workdir> tools/test/iso_put_mutants_extra.py <out_extra.json> -j3
PL = "components/web_server/web_iso_put_logic.c"
INC = '#include "web_iso_put_logic.h"'
PRAGMAS = '\n#pragma GCC diagnostic ignored "-Wparentheses"\n#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n#pragma GCC diagnostic ignored "-Wunused-variable"\n#pragma GCC diagnostic ignored "-Wunused-parameter"\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"\n#pragma GCC diagnostic ignored "-Wuninitialized"\n#pragma GCC diagnostic ignored "-Warray-parameter"\n#pragma GCC diagnostic ignored "-Wstringop-overflow"\n#pragma GCC diagnostic ignored "-Wtype-limits"\n#pragma GCC diagnostic ignored "-Wsign-compare"\n#pragma GCC diagnostic ignored "-Wreturn-type"'

COND = "    if (!io || !io->now_us || !io->recv || !buf || want_len == 0u || want_len > cap || timeout_us == 0ULL) {"


def M(id, desc, old, new, file=PL):
    return {"id": id, "desc": desc, "edits": [(old, new), (INC, INC + PRAGMAS)], "file": file}


MUTANTS = [
    # ---- recv_all: 引数の検査の項を 1 つずつ落とす ----
    M("Q01", "args: now_us term dropped", COND, COND.replace("!io->now_us || ", "")),
    M("Q02", "args: recv term dropped", COND, COND.replace("!io->recv || ", "")),
    M("Q03", "args: buf term dropped", COND, COND.replace("!buf || ", "")),
    M("Q04", "args: want_len == 0 term dropped", COND, COND.replace("want_len == 0u || ", "")),
    M("Q05", "args: want_len > cap term dropped", COND, COND.replace("want_len > cap || ", "")),
    M("Q06", "args: timeout_us == 0 term dropped", COND, COND.replace(" || timeout_us == 0ULL", "")),
    M("Q07", "args: INVALID returns IO", "        return WEB_ISO_PUT_ERR_INVALID;", "        return WEB_ISO_PUT_ERR_IO;"),
    M("Q08", "args: want_len > cap becomes want_len + 1 > cap", "want_len > cap ||", "want_len + 1u > cap ||"),
    # ---- recv_all: 時計 ----
    M("Q09", "clock: start_us fixed at 0", "    const uint64_t start_us = io->now_us(io->ctx);", "    const uint64_t start_us = 0ULL;"),
    M("Q10", "clock: start read gets a NULL ctx", "    const uint64_t start_us = io->now_us(io->ctx);", "    const uint64_t start_us = io->now_us(NULL);"),
    M("Q11", "clock: loop read gets a NULL ctx", "        const uint64_t now = io->now_us(io->ctx);", "        const uint64_t now = io->now_us(NULL);"),
    M("Q12", "clock: post-timeout read gets a NULL ctx", "            const uint64_t now2 = io->now_us(io->ctx);", "            const uint64_t now2 = io->now_us(NULL);"),
    M("Q13", "clock: backwards clock treated as zero elapsed", "(now >= start_us) ? (now - start_us) : timeout_us;", "(now >= start_us) ? (now - start_us) : 0ULL;"),
    M("Q14", "clock: backwards guard dropped (wraps)", "(now >= start_us) ? (now - start_us) : timeout_us;", "(now - start_us);"),
    M("Q15", "clock: loop-top timeout returns IO", "        if (elapsed >= timeout_us) {\n            return WEB_ISO_PUT_ERR_TIMEOUT;", "        if (elapsed >= timeout_us) {\n            return WEB_ISO_PUT_ERR_IO;"),
    M("Q16", "clock: post-recv timeout returns IO", "(now2 - start_us) >= timeout_us) {\n                return WEB_ISO_PUT_ERR_TIMEOUT;", "(now2 - start_us) >= timeout_us) {\n                return WEB_ISO_PUT_ERR_IO;"),
    M("Q17", "clock: post-recv backwards term dropped", "            if (now2 < start_us || (now2 - start_us) >= timeout_us) {", "            if ((now2 - start_us) >= timeout_us) {"),
    M("Q18", "clock: post-recv elapsed term dropped", "            if (now2 < start_us || (now2 - start_us) >= timeout_us) {", "            if (now2 < start_us) {"),
    M("Q19", "clock: post-recv check never ends the loop", "            if (now2 < start_us || (now2 - start_us) >= timeout_us) {", "            if (0) {"),
    # ---- recv_all: スライス ----
    M("Q20", "slice: no minimum clamp", "(rem_us < WEB_ISO_PUT_MIN_SLICE_US) ? WEB_ISO_PUT_MIN_SLICE_US : rem_us;", "rem_us;"),
    M("Q21", "slice: always the minimum", "(rem_us < WEB_ISO_PUT_MIN_SLICE_US) ? WEB_ISO_PUT_MIN_SLICE_US : rem_us;", "WEB_ISO_PUT_MIN_SLICE_US;"),
    M("Q22", "slice: ternary branches swapped", "(rem_us < WEB_ISO_PUT_MIN_SLICE_US) ? WEB_ISO_PUT_MIN_SLICE_US : rem_us;", "(rem_us < WEB_ISO_PUT_MIN_SLICE_US) ? rem_us : WEB_ISO_PUT_MIN_SLICE_US;"),
    M("Q23", "slice: set_timeout gets a NULL ctx", "        io->set_timeout_us(io->ctx, slice_us);", "        io->set_timeout_us(NULL, slice_us);"),
    M("Q24", "slice: set_timeout gets the unclamped remainder", "        io->set_timeout_us(io->ctx, slice_us);", "        io->set_timeout_us(io->ctx, rem_us);"),
    M("Q25", "slice: set_timeout gets the whole timeout", "        io->set_timeout_us(io->ctx, slice_us);", "        io->set_timeout_us(io->ctx, timeout_us);"),
    M("Q26", "slice: remainder uses want_len", "const uint64_t rem_us = timeout_us - elapsed;", "const uint64_t rem_us = timeout_us - (uint64_t)want_len;"),
    # ---- recv_all: recv ----
    M("Q27", "recv: buf offset dropped", "io->recv(io->ctx, buf + got, rem_bytes);", "io->recv(io->ctx, buf, rem_bytes);"),
    M("Q28", "recv: asks for the whole want_len", "io->recv(io->ctx, buf + got, rem_bytes);", "io->recv(io->ctx, buf + got, want_len);"),
    M("Q29", "recv: NULL ctx", "io->recv(io->ctx, buf + got, rem_bytes);", "io->recv(NULL, buf + got, rem_bytes);"),
    M("Q30", "recv: over-read returns TIMEOUT", "            if ((size_t)r > rem_bytes) {\n                return WEB_ISO_PUT_ERR_IO;", "            if ((size_t)r > rem_bytes) {\n                return WEB_ISO_PUT_ERR_TIMEOUT;"),
    M("Q31", "recv: over-read accepted and clamped", "            if ((size_t)r > rem_bytes) {\n                return WEB_ISO_PUT_ERR_IO;\n            }\n            got += (size_t)r;", "            got += ((size_t)r > rem_bytes) ? rem_bytes : (size_t)r;"),
    M("Q32", "recv: counts the whole remainder", "            got += (size_t)r;", "            got += rem_bytes;"),
    M("Q33", "recv: every negative result is a timeout", "        } else if (r == WEB_ISO_PUT_IO_TIMEOUT) {", "        } else if (r < 0) {"),
    M("Q34", "recv: every non-positive result is a timeout", "        } else if (r == WEB_ISO_PUT_IO_TIMEOUT) {", "        } else if (r <= 0) {"),
    M("Q35", "recv: IO error returns TIMEOUT", "        } else {\n            return WEB_ISO_PUT_ERR_IO;", "        } else {\n            return WEB_ISO_PUT_ERR_TIMEOUT;"),
    M("Q36", "recv: IO error is retried", "        } else {\n            return WEB_ISO_PUT_ERR_IO;", "        } else {\n            continue;"),
    M("Q37", "recv: success returns the byte count", "    return WEB_ISO_PUT_OK;\n}", "    return (int)got;\n}"),
    M("Q38", "recv: success returns IO", "    return WEB_ISO_PUT_OK;\n}", "    return WEB_ISO_PUT_ERR_IO;\n}"),
    # ---- progress ----
    M("Q39", "progress: invalid returns 0", "    if (total == 0ULL || !last_bucket) {\n        return -1;", "    if (total == 0ULL || !last_bucket) {\n        return 0;"),
    M("Q40", "progress: suppressed returns 0", "        return -1;\n    }\n    *last_bucket = bucket;", "        return 0;\n    }\n    *last_bucket = bucket;"),
    M("Q41", "progress: last_bucket not updated", "    *last_bucket = bucket;", "    ;"),
    M("Q42", "progress: last_bucket gets the percentage", "    *last_bucket = bucket;", "    *last_bucket = pct;"),
    M("Q43", "progress: last_bucket gets the milestone", "    *last_bucket = bucket;", "    *last_bucket = bucket * 5;"),
    M("Q44", "progress: returns the percentage, not the milestone", "    return bucket * 5;", "    return pct;"),
    M("Q45", "progress: returns the bucket", "    return bucket * 5;", "    return bucket;"),
    M("Q46", "progress: bucket 0 not suppressed", "    if (bucket <= 0 || bucket <= *last_bucket) {", "    if (bucket <= *last_bucket) {"),
    M("Q47", "progress: repeated bucket not suppressed", "    if (bucket <= 0 || bucket <= *last_bucket) {", "    if (bucket <= 0) {"),
    M("Q48", "progress: bucket width 10", "    const int bucket = pct / 5;", "    const int bucket = pct / 10;"),
    M("Q49", "progress: bucket width 4", "    const int bucket = pct / 5;", "    const int bucket = pct / 4;"),
    M("Q50", "progress: total + 1 in the exact path", "        pct = (int)((received * 100ULL) / total);", "        pct = (int)((received * 100ULL) / (total + 1ULL));"),
    M("Q51", "progress: pct from the wrong operand", "        pct = (int)((received * 100ULL) / total);", "        pct = (int)((received * 100ULL) / received);"),
    M("Q52", "progress: large path divides by total", "        const uint64_t denom = total / 100ULL;", "        const uint64_t denom = total;"),
    M("Q53", "progress: large path rounds the divisor up", "        const uint64_t denom = total / 100ULL;", "        const uint64_t denom = total / 100ULL + 1ULL;"),
    M("Q54", "progress: large path drops the zero-divisor guard (unreachable: received < total and received > UINT64_MAX / 100 mean total > UINT64_MAX / 100)", "(denom > 0ULL) ? (received / denom) : 100ULL;", "(received / denom);"),
    M("Q55", "progress: large path drops the 100 clamp (unreachable: raw <= 100 because received < total)", "        pct = (raw > 100ULL) ? 100 : (int)raw;", "        pct = (int)raw;"),
]
