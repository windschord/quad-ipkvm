#!/usr/bin/env python3
"""iso_core (iso_core.c) に 1 か所ずつ変更(ミュータント)を入れて、再ビルドし、iso_core のテストが落ちることを確認する。
結果は docs/ISO_CORE_TESTS.md の表。ミュータントの定義は iso_core_mutants.py(同じディレクトリ。gen_iso_mutants.py がソースから作る)。
使い方:
  python3 tools/test/mutate_iso_core.py <repo> <workdir> tools/test/iso_core_mutants.py <out.json> [-jN] [id ...]
詳しい説明(repo は変更しない、UNITY_SRC、-jN、BAD EDIT)は mutate_iso_extent.py と同じ。"""
import os
import sys

sys.dont_write_bytecode = True  # mutate_iso_extent と、ミュータントの定義の読み込みで、tools/test/__pycache__ を作らない(repo を変更しない)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutate_iso_extent as common  # noqa: E402

if __name__ == "__main__":
    common.main({
        "files": ["components/iso_core/iso_core.c"],
        "tests": [
            "test_core_queue",
            "test_core_jobs",
            "test_core_reinit",
            "test_core_tick",
            "test_core_fuzz",
        ],
    })
