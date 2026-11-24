#!/usr/bin/env python3
"""iso_worker_logic (iso_worker_logic.c) に 1 か所ずつ変更(ミュータント)を入れて、再ビルドし、iso_worker のテストが落ちることを確認する。
結果は docs/ISO_WORKER_TESTS.md の表。ミュータントの定義は iso_worker_mutants.py(gen_iso_mutants.py がソースから作る)と、
iso_worker_mutants_extra.py(手書きの追加分)。同じディレクトリ。
使い方:
  python3 tools/test/mutate_iso_worker.py <repo> <workdir> tools/test/iso_worker_mutants.py <out.json> [-jN] [id ...]
  python3 tools/test/mutate_iso_worker.py <repo> <workdir> tools/test/iso_worker_mutants_extra.py <out_extra.json> [-jN] [id ...]
詳しい説明(repo は変更しない、UNITY_SRC、-jN、BAD EDIT)は mutate_iso_extent.py と同じ。"""
import os
import sys

sys.dont_write_bytecode = True  # mutate_iso_extent と、ミュータントの定義の読み込みで、tools/test/__pycache__ を作らない(repo を変更しない)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutate_iso_extent as common  # noqa: E402

# 未初期化のローカル変数と詰め物を、固定のパターン(0xFE)で埋める(GCC 12 以降。cmake の初回の設定が、環境変数 CFLAGS を取り込む)。
# スタックの残りがたまたま 0 のとき、未初期化の読み出し(memset / コピーの長さを削るミュータント)が隠れて、生き残るのを避ける。
# 正しいコードの挙動は変わらない(通常の ctest のビルドには入れない)。
os.environ["CFLAGS"] = (os.environ.get("CFLAGS", "") + " -ftrivial-auto-var-init=pattern").strip()

if __name__ == "__main__":
    common.main({
        "files": ["components/iso_worker/iso_worker_logic.c"],
        "tests": [
            "test_iso_worker_logic",
            "test_iso_worker_logic_mut",
        ],
    })
