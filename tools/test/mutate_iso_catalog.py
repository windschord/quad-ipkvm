#!/usr/bin/env python3
"""iso_catalog (iso_catalog.c / iso_meta.c / iso_name.c) に 1 か所ずつ変更(ミュータント)を入れて、再ビルドし、
iso_catalog のテストが落ちることを確認する。結果は docs/ISO_CATALOG_TESTS.md の表。
ミュータントの定義は iso_catalog_mutants.py(同じディレクトリ。ミュータントごとに file で対象のファイルを指す)。
使い方:
  python3 tools/test/mutate_iso_catalog.py <repo> <workdir> tools/test/iso_catalog_mutants.py <out.json> [-jN] [id ...]
詳しい説明(repo は変更しない、UNITY_SRC、-jN、BAD EDIT)は mutate_iso_extent.py と同じ。"""
import os
import sys

sys.dont_write_bytecode = True  # mutate_iso_extent と、ミュータントの定義の読み込みで、tools/test/__pycache__ を作らない(repo を変更しない)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutate_iso_extent as common  # noqa: E402

if __name__ == "__main__":
    common.main({
        "files": [
            "components/iso_catalog/iso_catalog.c",
            "components/iso_catalog/iso_meta.c",
            "components/iso_catalog/iso_name.c",
        ],
        "tests": [
            "test_iso_meta",
            "test_iso_name",
            "test_iso_slots",
            "test_iso_transitions",
            "test_iso_begin_apply",
            "test_iso_scan",
            "test_iso_delete",
            "test_iso_import",
            "test_iso_powercut",
            "test_iso_fuzz",
        ],
    })
