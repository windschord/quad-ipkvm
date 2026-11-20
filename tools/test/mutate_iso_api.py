#!/usr/bin/env python3
"""iso_api (iso_api.c / iso_api_json.c / iso_api_fmt.c) に 1 か所ずつ変更(ミュータント)を入れて、再ビルドし、
iso_api のテストが落ちることを確認する。結果は docs/ISO_API_TESTS.md の表。
ミュータントの定義は iso_api_mutants.py(同じディレクトリ。gen_iso_mutants.py がソースから作る。ミュータントごとに file で対象のファイルを指す)。
使い方:
  python3 tools/test/mutate_iso_api.py <repo> <workdir> tools/test/iso_api_mutants.py <out.json> [-jN] [id ...]
詳しい説明(repo は変更しない、UNITY_SRC、-jN、BAD EDIT)は mutate_iso_extent.py と同じ。"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutate_iso_extent as common  # noqa: E402

if __name__ == "__main__":
    common.main({
        "files": [
            "components/iso_api/iso_api.c",
            "components/iso_api/iso_api_json.c",
            "components/iso_api/iso_api_fmt.c",
        ],
        "tests": [
            "test_api_json",
            "test_api_route",
            "test_api_list",
            "test_api_order",
            "test_api_ops",
            "test_api_fuzz",
        ],
    })
