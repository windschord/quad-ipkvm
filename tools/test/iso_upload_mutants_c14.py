# iso_finish.c の、SHA-256 の final の戻り値の扱い(C-14)の手書きのミュータント。tools/test/gen_iso_mutants.py は、代入の右辺の比較(== ISO_OK)と、
# 定数でないエラーの引数(ISO_ERR_IO)を変えないので、ここで足す。iso_upload_mutants.py(機械が作る)とは別に、同じスクリプトで実行する:
#   python3 tools/test/mutate_iso_upload.py <repo> <workdir> tools/test/iso_upload_mutants_c14.py <out.json> -j3
UP = "components/iso_upload/iso_upload.c"
FN = "components/iso_upload/iso_finish.c"
IX = "components/iso_upload/iso_index.c"
INC = '#include "iso_upload_priv.h"'
PRAGMAS = '\n#pragma GCC diagnostic ignored "-Wparentheses"\n#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n#pragma GCC diagnostic ignored "-Wunused-variable"\n#pragma GCC diagnostic ignored "-Wunused-parameter"\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"\n#pragma GCC diagnostic ignored "-Wuninitialized"\n#pragma GCC diagnostic ignored "-Warray-parameter"\n#pragma GCC diagnostic ignored "-Wstringop-overflow"\n#pragma GCC diagnostic ignored "-Wtype-limits"\n#pragma GCC diagnostic ignored "-Wsign-compare"\n#pragma GCC diagnostic ignored "-Wreturn-type"'


def M(id, desc, *edits, file=UP):
    return {"id": id, "desc": desc, "edits": list(edits), "file": file}


VER = '            job->have_digest = u->cfg.sha->final(u->cfg.sha_state, job->digest) == ISO_OK;'
VER_END = '        if (!job->have_digest) return end_ioerr(u, job, e, ISO_ERR_IO);'
HASH = '            if (sha->final(u->cfg.sha_state, job->digest) != ISO_OK) return hash_ioerr(job, ISO_ERR_IO); /* 出力は使わない */'

MUTANTS = [
    # FP_VERIFY(逐次の経路)
    M('C001', 'FP_VERIFY: final == ISO_OK -> != ISO_OK', (VER, VER.replace('== ISO_OK', '!= ISO_OK')), (INC, INC + PRAGMAS), file=FN),
    M('C002', 'FP_VERIFY: final == ISO_OK -> >= ISO_OK(正の戻り値を成功にする)', (VER, VER.replace('== ISO_OK', '>= ISO_OK')), (INC, INC + PRAGMAS), file=FN),
    M('C003', 'FP_VERIFY: final == ISO_OK -> != ISO_ERR_IO(ISO_ERR_IO だけを失敗にする)', (VER, VER.replace('== ISO_OK', '!= ISO_ERR_IO')), (INC, INC + PRAGMAS), file=FN),
    M('C004', 'FP_VERIFY: final の戻り値を捨てて、常に成功', (VER, '            (void)u->cfg.sha->final(u->cfg.sha_state, job->digest);\n            job->have_digest = true;'),
      (INC, INC + PRAGMAS), file=FN),
    M('C005', 'FP_VERIFY: 失敗の終わりのエラー ISO_ERR_IO -> ISO_OK', (VER_END, VER_END.replace('ISO_ERR_IO', 'ISO_OK')), (INC, INC + PRAGMAS), file=FN),
    M('C006', 'FP_VERIFY: 失敗の終わりが end_ioerr -> end_invalid(checksum_mismatch)',
      (VER_END, '        if (!job->have_digest) return end_invalid(u, job, e, ISO_INVALID_CHECKSUM_MISMATCH);'), (INC, INC + PRAGMAS), file=FN),
    # iso_upload_hash_step(読み直しの経路)
    M('C007', 'hash_step: final != ISO_OK -> < ISO_OK(正の戻り値を成功にする)', (HASH, HASH.replace('!= ISO_OK', '< ISO_OK')), (INC, INC + PRAGMAS), file=FN),
    M('C008', 'hash_step: final != ISO_OK -> == ISO_ERR_IO(ISO_ERR_IO だけを失敗にする)', (HASH, HASH.replace('!= ISO_OK', '== ISO_ERR_IO')), (INC, INC + PRAGMAS), file=FN),
    M('C009', 'hash_step: 失敗のエラー ISO_ERR_IO -> ISO_OK', (HASH, HASH.replace('hash_ioerr(job, ISO_ERR_IO)', 'hash_ioerr(job, ISO_OK)')), (INC, INC + PRAGMAS), file=FN),
    M('C010', 'hash_step: 失敗が I/O エラーでなく invalid(part_mismatch)',
      (HASH, HASH.replace('hash_ioerr(job, ISO_ERR_IO)', 'hash_invalid(job, ISO_INVALID_PART_MISMATCH)')), (INC, INC + PRAGMAS), file=FN),
    M('C011', 'hash_step: final の戻り値を捨てて、常に成功',
      (HASH, '            (void)sha->final(u->cfg.sha_state, job->digest);'), (INC, INC + PRAGMAS), file=FN),
]
