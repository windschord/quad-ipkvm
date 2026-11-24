#!/usr/bin/env python3
"""iso_usb_msc_glue (iso_usb_msc_glue.c: MSC の BOT と mmc_scsi と iso_extent の橋) に 1 か所ずつ変更(ミュータント)を入れて、再ビルドし、
iso_usb_msc のテストが落ちることを確認する。結果は docs/ISO_PUT_MSC_TESTS.md の表。
ミュータントの定義は iso_msc_mutants.py(gen_iso_mutants.py がソースから作る)と、iso_msc_mutants_extra.py(手書きの追加分)。同じディレクトリ。
使い方:
  python3 tools/test/mutate_iso_msc.py <repo> <workdir> tools/test/iso_msc_mutants.py <out.json> [-jN] [id ...]
  python3 tools/test/mutate_iso_msc.py <repo> <workdir> tools/test/iso_msc_mutants_extra.py <out_extra.json> [-jN] [id ...]
詳しい説明(repo は変更しない、UNITY_SRC、-jN、BAD EDIT)は mutate_iso_extent.py と同じ。"""
import os
import sys

sys.dont_write_bytecode = True  # mutate_iso_extent と、ミュータントの定義の読み込みで、tools/test/__pycache__ を作らない(repo を変更しない)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutate_iso_extent as common  # noqa: E402

# 未初期化のローカル変数と詰め物を、固定のパターン(0xFE)で埋める(mutate_iso_worker.py と同じ理由。正しいコードの挙動は変わらない)。
os.environ["CFLAGS"] = (os.environ.get("CFLAGS", "") + " -ftrivial-auto-var-init=pattern").strip()

if __name__ == "__main__":
    common.main({
        "files": ["components/iso_usb_msc/iso_usb_msc_glue.c"],
        "tests": [
            "test_iso_usb_msc_glue_mut",  # 先に実行する(終わらないミュータントを、モックの上限で早く止めるため)。元のテストだけでの結果は、docs の 2 章
            "test_iso_usb_msc_glue",
        ],
    })
