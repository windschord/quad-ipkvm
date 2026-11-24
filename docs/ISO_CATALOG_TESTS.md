# iso_catalog のテストとミューテーションテストの結果

作成: 2026-11-19(Issue #139、C-10 の前半)。`components/iso_catalog`(メタデータ 256 バイトの符号化・復号、2 スロット、名前の規則、`<id>` の生成、起動時のスキャン、状態の遷移表、`begin`、削除のステップ、`import`。純 C)のホスト単体テストの構成と、`iso_catalog.c` / `iso_meta.c` / `iso_name.c` に対するミューテーションテストの結果。契約は `docs/ISO_MOUNT_DESIGN.md` §3(特に 3.2、3.3、3.4)、§7、§9、`docs/WEB_API.md` §8。

## 1. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/iso_catalog/test_iso_meta.c` | CRC-32 の既知の値、符号化のバイト列(手書きの期待値 3 通り)、往復、復号の拒否(長さ 255 / 257、マジック、版、CRC、全ビット反転、範囲外の各フィールド、名前の長さ 63 / 64、名前の規則)、予約領域、失敗時に出力を変えない | 19 |
| `test/host/iso_catalog/test_iso_name.c` | 名前の規則(UTF-8 の境界、C0 / DEL / C1、`/` と `\`、前後の空白、63 / 64 バイト)、同名判定、`<id>` の形式 | 6 |
| `test/host/iso_catalog/test_iso_slots.c` | 2 スロットの選択(世代の巻き戻り UINT32_MAX → 0、同世代、差が INT32_MIN)、スロットの読み込み(長さ 0 / 255 / 256 / 257 / 258 / 512、壊れた CRC、ディレクトリ、I/O エラー) | 14 |
| `test/host/iso_catalog/test_iso_transitions.c` | 状態の遷移表(§3.4)の全 98 マス(7 状態 × 14 イベント)、フラグで変わる行、状態の保存の対応 | 9 |
| `test/host/iso_catalog/test_iso_scan.c` | 起動時のスキャン(id でないディレクトリ、状態ごとの扱い、`uploading` → `incomplete`、`meta_corrupt`、deleting の印、16 個の上限と hidden)、`received` の計算、**予約 ID `00000000` を採用せず `hidden` に数える(C-14。枠を使わない、ディレクトリを読まない、似た名前は従来どおり、再スキャンで数え直す)** | 27 |
| `test/host/iso_catalog/test_iso_begin_apply.c` | `begin`(書き込みの順序、検査の順序、同名、上限、`<id>` の引き直し、失敗の片づけ、**予約 ID `00000000` の引き直し(C-14。乱数が 0 のとき mkdir せずに引き直す、16 回に数える、衝突と合わせて 16 回、0 に近い値は採用)**)、イベントの適用(meta の書き込みと世代、スロットの交互、世代の巻き戻り) | 37 |
| `test/host/iso_catalog/test_iso_delete.c` | 削除のステップ(順序、1 回 1 つの書き込み、再起動後の続き、エラーの再試行、`ready` が復活しないこと) | 25 |
| `test/host/iso_catalog/test_iso_import.c` | `import`(拡張子、名前、大きさ、署名の位置、同名、上限、クロスリンク、`cursor` / `next_cursor` と 4 個ずつの処理、I/O エラー、書き込み失敗の片づけ、**取り込みの `<id>` も 0 を引き直す(C-14)**) | 31 |
| `test/host/iso_catalog/test_iso_powercut.c` | 電源断の網羅(下記) | 6 |
| `test/host/iso_catalog/test_iso_fuzz.c` | 固定シードのファズ(下記) | 6 |
| `test/host/iso_catalog/iso_mock_fs.c` / `.h` | モックの `iso_fs_t`(メモリ上。電源断の注入、書き込みの失敗の注入、読み出しの失敗の注入、書き込みの記録、疎なファイル、クロスリンクの再現)と `iso_random_t` | - |
| `test/host/iso_catalog/ic_common.h` | 共通の部品(フィクスチャ、meta / スロット / パーツ / import ファイルを置く補助) | - |

合計 180 件(`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルは 10 個増える。`iso_extent` の 3 個と合わせて 13 個。全体は 75 → 88)。

**期待値はすべて手書き**。符号化のバイト列は、レイアウト表(設計 3.2)から手で書き、CRC-32 の値は Python の `zlib.crc32` で別に求めた定数(`0x258DD8B1`、`0x3D5027B1`、`0x8B1AE264`)。遷移表は、7 × 14 の表を手書きで固定した(実装の関数から作らない)。

### 電源断の網羅(`test_iso_powercut.c`)

モックの `iso_fs_t` は、書き込み(`mkdir` / `remove` / `rename` / `overwrite`)に通し番号を付け、n 番目の書き込みで電源を落とす(それ以降の読み書きはすべて IO エラー。`mock_fs_power_on` で戻す)。落とし方は 3 通り: `NONE`(その書き込みは何も起きない)、`HALF`(`overwrite` が、データの前半だけを書いたファイルを残す = 途中のセクタだけ書けた)、`FULL`(書き込みは完了していて、戻り値だけ IO エラー)。

- **シナリオ 1(全 12 回の書き込み × 3 通り = 36 通り)**: `begin` → idle → resume → idle → finish → finalize ok → index fail(part_mismatch)→ delete(手順 (1) の `meta` の書き込み、`p000`、`p001`、`meta.a`、`meta.b`、`rmdir`)。書き込みの列と、それぞれが書く meta(世代、状態、理由)は手書きで固定した(L1〜L6)。再起動(スキャン)して、**電源断の位置に応じて、ディスクに残る meta がどれか**を確かめる: 電源断より前に完了した最後の meta、`FULL` で meta の書き込みが完了していればその次。`mkdir` だけ、または `meta.a` が途中までなら `meta_corrupt`(削除できる)。削除の手順 (1) の後は、`deleting` を続ける、または `meta_corrupt` で、`ready` / `incomplete` は復活しない。どの場合も、削除のステップを最後まで進めると、`/iso` には `import` だけが残る。
- **二重の電源断(6 × 3 × 6 × 3 = 324 通り)**: 削除の最中に電源断 → 再起動 → 続きの削除の最中にまた電源断 → 再起動 → 削除を完了。
- **import(3 回の書き込み × 3 通り)**: `mkdir` / `meta` / `rename` のそれぞれで電源断。ファイルが失われない(`rename` が完了していなければ `import/` に残る)。残ったディレクトリは `meta_corrupt` か、meta だけの `ready`。再び `rescan` すれば取り込める(meta だけが残った同名の `ready` があるときは、同名として `skipped`。**設計への提案を参照**)。
- **begin(2 回の書き込み × 3 通り)**: 何も残らない、`meta_corrupt`、または `incomplete`(`meta.a` が完了していた場合)で復帰する。

### ファズ(`test_iso_fuzz.c`。固定シード、ASan / UBSan)

1. **meta の復号**: (a) 任意のバイト列、任意の長さ(**ちょうどの大きさのヒープ領域**なので、範囲外の読み出しを ASan が見つける)10 万回。先頭・版・各フィールドをほぼ正しくして CRC を合わせたものを混ぜ、フィールドの検査まで届かせる(受理 5781 回)。(b) 有効な meta をランダムに作り、1〜4 バイトを壊して(CRC を直す / 直さない)復号する 10 万回(受理 34405 回、拒否 65595 回)。受理したものは、検査を通り、符号化し直せて、復号し直すと同じになる。(c) 範囲外を含むランダムな構造体で、`iso_meta_validate` と `iso_meta_encode` の結果が一致する 20 万回(符号化できたもの 15095 回)。
2. **名前の規則**: 実装とは独立な基準(コードポイントを計算して判定)と、**長さ 1〜3 のすべてのバイト列(約 1684 万通り。規則に合うもの 120 万通り)**を総当たりで比べる。長さ 60〜67 を含むランダムな列 100 万回(規則に合うもの 11 万回)。
3. **ランダムな操作列 + 電源断**: 150 シード × 100 操作(`begin`、任意のイベント、削除のステップ、import するファイルを置く、`rescan`、再起動、電源断の予約)。**電源が入っている間は毎回**、別のカタログでスキャンして、**ディスク上のスロットのバイト列を実装を使わずに読んだ基準**(有効 = 長さ 256 + 復号できる、世代の新しい方、同世代は a)と、カタログのエントリ(採用スロット、世代、状態、`meta_corrupt`)が一致すること、`<id>` のディレクトリの数 = `count + hidden` を確かめる。最後に、すべて削除でき、`/iso` に `import` だけが残る。実測(固定シードなので毎回同じ): 操作 15000 回、電源断 877 回、import 640 件、削除の完了 1620 件。

## 2. ミューテーションテスト

方法: `tools/test/mutate_iso_catalog.py` が、`components/` と `test/` をスクラッチにコピーし、`iso_catalog.c` / `iso_meta.c` / `iso_name.c` の 1 か所を置換 → 再ビルド → 10 個のテストの実行ファイルを順に実行、を繰り返す(ミュータントの定義は `tools/test/iso_catalog_mutants.py`。ミュータントごとに対象のファイルを指す)。1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。最初の失敗で、そのミュータントの実行を止める。

```
python3 tools/test/mutate_iso_catalog.py <repo> <スクラッチ> tools/test/iso_catalog_mutants.py <結果.json> -j4
```

**C-14(Issue #162。予約 ID `00000000`)の後の再実行**: `iso_id_generate`(乱数が 0 のとき `continue` で引き直す。`ISO_ID_TRIES` に数える)と `iso_catalog_scan`(`00000000` を採用せず `hidden` に数える)を変えたので、ミュータント P01〜P15(15 個。生成の 6 個、スキャンの 9 個)を足し、**全数(593 個)を最初から実行**した。
生存は、前と同じ 21 個(同じ ID、同じ理由)。新しい P は 15 個とも殺した。`C62`(`hidden` を 2 回数える)は、old の文字列が、新しい `hidden++` と重なって 1 回だけ現れなくなった(BAD EDIT)ので、前後の行を足して書き直した(意味は同じ)。
`P05`(0 を引いても `tries` を進めない)は、乱数が 0 しか返さないとき終わらなくなる変更。`test_id_generate_zero_draws_count_as_tries_and_never_create_the_reserved_directory`(0 を 16 回引いた後で 17 個目の値を採用してしまう)が殺す(0 だけを返す乱数は、先に別のテストが落ちるので、ハングは起きない)。
`test_iso_import.c` と `test_iso_delete.c` の件数は、以前の表が 1 ずつ少なかった(実数に直した)。

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー |
|---|---|---|---|
| 593 | 572 | 21 | 0 |

ミュータントの内訳(PR #150 の対応で、Q の 30 個を足した。下の 3.): `iso_name.c` 63 個(N)、`iso_meta.c` 88 個(T)、遷移表と状態の対応 56 個(S)、2 スロット 31 個(L)、カタログ・received・スキャン 66 個(C)、`begin` と `<id>` 50 個(B)、`write_meta` と `apply` 49 個(W)、削除 52 個(D)、import 69 個(I)、`rescan` 24 個(R)、クロスリンクの照合と、削除の拒否 30 個(Q)、**予約 ID 15 個(P。C-14)**。

経緯:

1. 最初の実行(548 個): 殺した 477、生き残り 49、ビルドエラー 22(`-Werror` の未使用変数・関数・引数。ミュータントの書き方を直した。ミュータントの意味は変えていない)。
2. 生き残ったものを 1 つずつ調べ、**同値でないものには、殺すテストを足した**(下表)。足した後、最初の実行で殺せなかった 71 個(生き残り + ビルドエラー)を実行し直して、47 個を殺し、24 個が生き残った(うち 1 個は、下の `iso_meta_encode` の変更で殺せるようになった)。
3. `T52`(符号化の一時バッファを 0 で埋めない)は、スタックを汚すテストでも殺せなかった(スタックの場所が重ならない)。**実装を変えた**: `iso_meta_encode` は、検査が済んだ後は失敗しないので、一時バッファを使わず、`out` に直接書く(`out` を 0 で埋めてから書く)。これで、`out` を 0xAA で汚したテストが殺す。変更の後、`iso_meta.c` のミュータント 88 個を**最初から実行し直して**、85 個を殺し、3 個が同値(T21、T23、T90)。その他のミュータント(`iso_catalog.c`、`iso_name.c`)は、`iso_meta.c` の変更に影響されず、テストは足しただけなので、最初の実行 / 2 回目の実行の結果を使った。

4. **C-10b での変更(Issue #139 の後半)**: `iso_seams.h` に `iso_fs_t.read_at` を足したので、import の署名の検査は、順次読み 65 回から `read_at` の 1 回に変わった(上の 6.)。それに関わるミュータント I12〜I20 を書き直して、全数を再実行した。**Q11**(クロスリンクの照合で、一致した後に `break` しない)は、一致の次の `dir_next` が `ISO_ERR_IO` を返すと、`crosslinked` ではなく I/O エラーになる(同値ではない)ので、`test_delete_precheck_stops_at_the_first_match_and_does_not_read_further`(モックに `mock_fs_fail_dir_next_after` を足した)で殺した。全数の再実行: 578 個、殺した 557、生き残り 21(すべて同値。下の 2.1)、ビルドエラー 0。

| 最初の実行で生き残ったミュータント | 足したテスト |
|---|---|
| B12(`iso_id_generate` が mkdir のエラーで出力を空にしない) | `test_id_generate_clears_the_output_on_a_mkdir_error` |
| C04、C53(`/iso` の作成の失敗を無視) | `test_ensure_dirs_reports_each_failure`(`mkdir /iso` だけを失敗させる) |
| C07、C08、C10(`find` の NULL、前方一致、先頭より小さい id) | `test_find_returns_only_exact_ids` |
| C15(`remove` が先頭だけを見る) | `test_remove_entry_in_the_middle_and_at_the_end` |
| C22(`received` が meta のパーツ数を超えて数える) | `test_received_follows_part_count_of_the_meta` |
| C26(`received` がディレクトリを数える) | `test_received_part_that_is_a_directory_stops`(大きさが満杯のディレクトリ) |
| D16(削除の続きで、残す方のスロットを覚えていない) | `test_delete_resume_when_slot_a_holds_the_deleting_meta`(採用されているのが `meta.a` のとき) |
| D40、D41(スロットの削除のエラーを無視) | `test_delete_error_at_every_write_is_reported_and_retried`(6 回の書き込みのそれぞれで失敗させる) |
| I06(`.iso` の検査が、4 バイトより短い名前で名前の手前を読む) | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored` に `a`、`ab`、`abc` を追加(ASan) |
| I12〜I20(署名の読み出し: `open` の失敗、`read_at` の位置・長さ・I/O エラー、閉じ忘れ)。C-10b で `read_at` に変えたので、書き直した | `test_rescan_read_errors_while_checking_the_signature`(`read_at` の 1 回目を失敗させる)、`test_import_signature_is_one_read_at_at_sector_16`、署名のテスト |
| I30(クロスリンクの検査が先頭のイメージだけ) | `test_import_crosslink_is_checked_against_every_image_and_ignores_directories` |
| I33(クロスリンクの検査がディレクトリも数える) | 同上(`p000` が、先頭クラスタが同じディレクトリのとき) |
| I54(import の名前に拡張子が入る。ミュータントの書き方が間違っていて、同じ内容になっていた。書き直した) | (既存のテストが殺す) |
| L13、L17、L18(スロットの読み出し: ディレクトリ、open の失敗、read の失敗) | `test_slots_read_open_and_read_errors_are_not_treated_as_invalid`、`test_slots_read_directory_with_the_size_of_a_slot_is_invalid` |
| L30(`write_slot` が符号化のエラーを無視) | `test_write_meta_rejects_an_invalid_meta_without_writing` |
| T26(`get_le64` が上位 1 バイトを読まない) | `test_decode_rejects_out_of_range_fields_with_valid_crc` に、`size_bytes` の最上位バイトが非 0 のケースを追加 |
| T52(符号化の予約領域が 0 でない) | `test_encode_exact_bytes_case_a` で `out` を 0xAA で汚す(実装を直接書きに変更。上の 3.) |
| T63(符号化が名前の枠の全体をコピーする) | `test_encode_name_padding_is_zero_even_if_the_struct_has_garbage` |
| W36、W37(SHA-256 の更新が、`FINALIZE_OK` 以外、または `set_sha` なしでも起きる) | `test_apply_sha_update_only_for_finalize_ok_with_set_sha` |
| W48、W49(`DELETE` で、理由と「続ける」印が消えない) | `test_apply_delete_clears_reason_and_resume_mark` |

### 2.1 同値のミュータント(21 個)

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| N16 | 名前: 2 バイトの先頭バイトの下限を 0xC2 → 0xC1 | `C1 xx` は、0x40〜0x7F にデコードされ、`cp <= 0x9F` の検査(C1 制御文字)が拒否する。結果は同じ |
| N22 | 名前: 4 バイトの先頭バイトの上限を 0xF4 → 0xF5 | `F5 xx xx xx` は 0x10FFFF を超え、範囲の検査が拒否する |
| N30 | 名前: 継続バイトの下位の mask を 0x3F → 0x7F | 継続バイトは `10xxxxxx` であることを、直前に検査している。ビット 6 は 0 なので、0x7F でも 0x3F でも同じ |
| T21 | `put_le16` が上位バイトを書かない | 版(1)とパーツ数(16 以下)は、1 バイトに収まる |
| T23 | `put_le64` が 7 バイトだけ書く | `size_bytes` は 16 GiB 以下(5 バイト)。上位のバイトは 0 |
| T90 | 復号が名前を NUL で終端しない | 構造体は、先に 0 で埋めている |
| S42 | 遷移の拒否のとき、`*next` に `n` を入れる | 拒否のとき `n` は、まだ `cur` のまま |
| L19 | `got != 256` の検査を外す | 残りのバッファは不定で、CRC が合わず、復号が拒否する |
| C13 | `remove` が、最後のエントリを 0 で埋めない | `count` より後ろのエントリは、読まれない(次の追加で `memset` する) |
| C51 | スキャンの先頭で `images` を 0 で埋めない | `load_entry` が各エントリを `memset` する。`count` 以降は読まれない |
| C66 | スキャンで、読み込みに失敗したエントリを 0 で埋めない | `count` に数えていないので、読まれない |
| B16 | `begin` が `name == NULL` を拒否しない | `iso_name_valid(NULL, ...)` が拒否する |
| W13 | `apply` の `received_complete` が `meta_valid` を見ない | `meta_valid` が偽のエントリは `invalid` で、遷移表が先に拒否する |
| W27 | `apply` が `meta_valid` の偽のエントリへの書き込みを拒否しない | 同上(`invalid` では、meta を書くイベントが受理されない) |
| W32 | `apply` が理由 5(`meta_corrupt`)を受け付ける | `iso_meta_encode` が理由 5 を拒否し、何も書かない(エラーは `ISO_ERR_INVALID` で同じ) |
| D05 | 削除ジョブの初期化で `keep` を -1 にしない | 直前の `memset` で 0 になるが、`PH_INIT` の最初のステップが `keep` を設定する(それまで読まれない) |
| D35 | 削除で、パスの長さの検査を外す | 名前は 255 バイト以下で、パスは最大 `/iso/xxxxxxxx/` + 255 + NUL = 270 < 288 |
| I17 | 署名の `read_at` の `got == 6` の検査を外す | 大きさ 34816 以上は、直前に検査している。短い読み出しは起きない |
| I69 | import のパスの長さの検査を外す | `/iso/import/` + 255 バイト + NUL = 268 < 288 |
| Q41 | `apply`: INVALID に入るときも、直前の理由を引き継ぐ | INVALID に入るイベント(`FINALIZE_FAIL` / `INDEX_FAIL`)は、引数の理由で上書きする |
| Q42 | `apply`: INVALID を離れるときも、理由を引き継ぐ | 離れるとき(`DELETE`)は、その後で理由を 0 にする |

### 2.2 殺したミュータントの一覧(572 個)

「殺したテスト」は、失敗したテストのうち先頭の 2 つ(最初に落ちたテストの実行ファイルで止める)。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| N01 | name: length limit >= 63 | `test_encode_exact_bytes_maximum_values`, `test_decode_roundtrip` |
| N02 | name: length limit removed | `test_encode_rejects_out_of_range_values` |
| N03 | name: empty name accepted | `test_encode_rejects_out_of_range_values` |
| N04 | name: 1-byte name rejected | `test_encode_exact_bytes_minimum_values`, `test_decode_roundtrip` |
| N05 | name: NULL accepted (len check only) | `test_iso_name:crash/rc=1` |
| N06 | name: leading space accepted | `test_decode_applies_the_name_rule` |
| N07 | name: trailing space accepted | `test_decode_applies_the_name_rule` |
| N08 | name: trailing check looks at the byte after | `test_decode_applies_the_name_rule` |
| N09 | name: C0 boundary <= 0x20 (space rejected) | `test_name_rule_table`, `test_name_valid_every_ascii_byte_in_the_middle` |
| N10 | name: C0 boundary < 0x1F | `test_name_rule_table`, `test_name_valid_every_ascii_byte_in_the_middle` |
| N11 | name: C0 check removed | `test_decode_applies_the_name_rule` |
| N12 | name: DEL accepted | `test_name_rule_table`, `test_name_valid_every_ascii_byte_in_the_middle` |
| N13 | name: slash accepted | `test_decode_applies_the_name_rule`, `test_encode_rejects_out_of_range_values` |
| N14 | name: backslash accepted | `test_decode_applies_the_name_rule` |
| N15 | name: ASCII loop does not advance | `test_iso_meta:timeout` |
| N17 | name: 2-byte lead from 0xC3 | `test_name_rule_table` |
| N18 | name: 2-byte lead to 0xE0 (E0 taken as 2-byte) | `test_name_rule_table` |
| N19 | name: 2-byte lead to 0xDE | `test_name_rule_table` |
| N20 | name: 3-byte lead mask wrong | `test_name_rule_table` |
| N21 | name: 4-byte lead from 0xF1 | `test_name_rule_table` |
| N23 | name: 4-byte lead to 0xF3 | `test_name_rule_table` |
| N24 | name: invalid lead accepted as 1 byte | `test_decode_applies_the_name_rule` |
| N25 | name: truncated sequence accepted (>=) | `test_iso_fuzz:crash/rc=1` |
| N26 | name: truncated sequence check removed | `test_iso_fuzz:crash/rc=1` |
| N27 | name: sequence ending exactly at len rejected | `test_decode_applies_the_name_rule` |
| N28 | name: continuation byte mask 0x80 | `test_name_rule_table` |
| N29 | name: continuation check removed | `test_name_rule_table` |
| N31 | name: C1 boundary < 0x9F | `test_name_rule_table` |
| N32 | name: C1 boundary <= 0xA0 | `test_name_rule_table` |
| N33 | name: C1 check removed | `test_name_rule_table` |
| N34 | name: overlong 3-byte boundary <= 0x800 | `test_name_rule_table` |
| N35 | name: overlong 3-byte boundary < 0x7FF | `test_name_rule_table` |
| N36 | name: overlong 3-byte check removed | `test_name_rule_table` |
| N37 | name: surrogate lower bound 0xD801 | `test_name_rule_table` |
| N38 | name: surrogate upper bound 0xDFFE | `test_name_rule_table` |
| N39 | name: surrogate lower bound 0xD7FF | `test_name_rule_table` |
| N40 | name: surrogate upper bound 0xE000 | `test_name_rule_table` |
| N41 | name: surrogate check removed | `test_name_rule_table` |
| N42 | name: overlong 4-byte boundary <= 0x10000 | `test_name_rule_table` |
| N43 | name: overlong 4-byte boundary < 0xFFFF | `test_name_rule_table` |
| N44 | name: max code point >= 0x10FFFF | `test_name_rule_table` |
| N45 | name: max code point 0x110000 allowed | `test_name_rule_table` |
| N46 | name: 4-byte range check removed | `test_name_rule_table` |
| N47 | name: sequence length not skipped | `test_decode_applies_the_name_rule` |
| N48 | name: equal: length compare removed | `test_name_equal_is_exact_bytes` |
| N49 | name: equal: empty names differ | `test_name_equal_is_exact_bytes` |
| N50 | name: equal: memcmp inverted | `test_name_equal_is_exact_bytes` |
| N51 | name: equal: compares one byte less | `test_name_equal_is_exact_bytes` |
| N52 | name: id: '0' lower bound 1 | `test_id_format` |
| N53 | name: id: '9' upper bound 8 | `test_id_format` |
| N54 | name: id: ':' accepted | `test_id_format` |
| N55 | name: id: '/' accepted | `test_id_format` |
| N56 | name: id: 'a' lower bound 'b' | `test_id_format` |
| N57 | name: id: 'f' upper bound 'e' | `test_id_format` |
| N58 | name: id: 'g' accepted | `test_id_format` |
| N59 | name: id: '`' accepted | `test_id_format` |
| N60 | name: id: uppercase accepted | `test_id_format` |
| N61 | name: id: NUL terminator not required | `test_id_format` |
| N62 | name: id: NULL accepted | `test_iso_name:crash/rc=1` |
| N63 | name: id: checks only 7 digits | `test_id_format` |
| T01 | crc: polynomial | `test_crc32_known_vectors`, `test_encode_exact_bytes_case_a` |
| T02 | crc: initial value | `test_crc32_known_vectors`, `test_encode_exact_bytes_case_a` |
| T03 | crc: no final xor | `test_crc32_known_vectors`, `test_encode_exact_bytes_case_a` |
| T04 | crc: 7 bit rounds | `test_crc32_known_vectors`, `test_encode_exact_bytes_case_a` |
| T05 | crc: last byte skipped | `test_crc32_known_vectors`, `test_encode_exact_bytes_case_a` |
| T06 | crc: shift direction | `test_crc32_known_vectors`, `test_encode_exact_bytes_case_a` |
| T07 | layout: state offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_maximum_values` |
| T08 | layout: reason offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_maximum_values` |
| T09 | layout: size offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T10 | layout: parts offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T11 | layout: part_blocks offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T12 | layout: sha offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_maximum_values` |
| T13 | layout: sha flag offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_maximum_values` |
| T14 | layout: name_len offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T15 | layout: name offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T16 | layout: generation offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_maximum_values` |
| T17 | layout: crc offset (covers 248 bytes) | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T18 | layout: version offset | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T19 | magic: wrong byte | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T20 | version constant is not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T22 | le32: bytes in big-endian order | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T24 | get_le16: high byte dropped | `test_decode_rejects_bad_version`, `test_decode_rejects_out_of_range_fields_with_valid_crc` |
| T25 | get_le32: order reversed | `test_decode_roundtrip`, `test_decode_expected_bytes_case_a` |
| T26 | get_le64: only 7 bytes | `test_decode_rejects_out_of_range_fields_with_valid_crc` |
| T30 | validate: NULL accepted | `test_iso_meta:crash/rc=1` |
| T31 | validate: state 5 accepted | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_encode_rejects_out_of_range_values` |
| T32 | validate: state 4 rejected | `test_encode_exact_bytes_maximum_values`, `test_decode_roundtrip` |
| T33 | validate: reason 5 accepted | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_encode_rejects_out_of_range_values` |
| T34 | validate: reason 4 rejected | `test_encode_exact_bytes_maximum_values`, `test_decode_roundtrip` |
| T35 | validate: size 0 accepted | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_encode_rejects_out_of_range_values` |
| T36 | validate: size limit >= | `test_encode_exact_bytes_maximum_values`, `test_decode_roundtrip` |
| T37 | validate: size limit removed | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_encode_rejects_out_of_range_values` |
| T38 | validate: size 1 rejected | `test_encode_exact_bytes_minimum_values`, `test_decode_roundtrip` |
| T39 | validate: parts 0 accepted | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_encode_rejects_out_of_range_values` |
| T40 | validate: parts 17 accepted | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_encode_rejects_out_of_range_values` |
| T41 | validate: parts 16 rejected | `test_encode_exact_bytes_maximum_values`, `test_decode_roundtrip` |
| T42 | validate: parts 1 rejected | `test_encode_exact_bytes_minimum_values`, `test_decode_roundtrip` |
| T43 | validate: part_blocks 0 accepted | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_encode_rejects_out_of_range_values` |
| T44 | validate: part_blocks limit >= | `test_encode_exact_bytes_maximum_values`, `test_decode_roundtrip` |
| T45 | validate: part_blocks limit removed | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_encode_rejects_out_of_range_values` |
| T46 | validate: part_blocks 1 rejected | `test_encode_exact_bytes_minimum_values`, `test_decode_roundtrip` |
| T47 | validate: sha flag 3 accepted | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_encode_rejects_out_of_range_values` |
| T48 | validate: sha flag 2 rejected | `test_encode_exact_bytes_case_a`, `test_decode_roundtrip` |
| T49 | validate: name not checked | `test_decode_applies_the_name_rule`, `test_encode_rejects_out_of_range_values` |
| T50 | encode: NULL out accepted | `test_iso_meta:crash/rc=1` |
| T51 | encode: validation removed | `test_encode_rejects_out_of_range_values` |
| T52 | encode: buffer not cleared | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T53 | encode: magic not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T54 | encode: state not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_maximum_values` |
| T55 | encode: reason not written | `test_encode_exact_bytes_maximum_values`, `test_decode_roundtrip` |
| T56 | encode: size not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T57 | encode: parts not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T58 | encode: part_blocks not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T59 | encode: sha not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_maximum_values` |
| T60 | encode: sha flag not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_maximum_values` |
| T61 | encode: name_len not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T62 | encode: name not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T63 | encode: name copies the whole 64-byte field | `test_encode_name_padding_is_zero_even_if_the_struct_has_garbage` |
| T64 | encode: generation not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_maximum_values` |
| T65 | encode: crc not written | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T66 | encode: crc covers one byte more | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T67 | encode: crc is zero | `test_encode_exact_bytes_case_a`, `test_encode_exact_bytes_minimum_values` |
| T70 | decode: NULL buf accepted | `test_iso_meta:crash/rc=1` |
| T71 | decode: NULL out accepted | `test_iso_meta:crash/rc=1` |
| T72 | decode: length check >= | `test_decode_rejects_wrong_lengths` |
| T73 | decode: length check <= | `test_decode_rejects_wrong_lengths` |
| T74 | decode: magic not checked | `test_decode_rejects_bad_magic_each_byte` |
| T75 | decode: only 3 magic bytes checked | `test_decode_rejects_bad_magic_each_byte` |
| T76 | decode: version not checked | `test_decode_rejects_bad_version` |
| T77 | decode: version >= 1 accepted | `test_decode_rejects_bad_version` |
| T78 | decode: crc not checked | `test_decode_rejects_each_crc_byte`, `test_decode_rejects_any_single_bit_flip_in_the_body` |
| T79 | decode: state not read | `test_decode_roundtrip`, `test_decode_rejects_out_of_range_fields_with_valid_crc` |
| T80 | decode: reason not read | `test_decode_roundtrip`, `test_decode_rejects_out_of_range_fields_with_valid_crc` |
| T81 | decode: size not read | `test_decode_roundtrip`, `test_decode_expected_bytes_case_a` |
| T82 | decode: parts not read | `test_decode_roundtrip`, `test_decode_expected_bytes_case_a` |
| T83 | decode: part_blocks not read | `test_decode_roundtrip`, `test_decode_expected_bytes_case_a` |
| T84 | decode: sha not read | `test_decode_roundtrip` |
| T85 | decode: sha flag not read | `test_decode_roundtrip`, `test_decode_rejects_out_of_range_fields_with_valid_crc` |
| T86 | decode: name_len check removed (equivalent: validate rejects 0 and > 63; memcpy of > 63 is the hazard) | `test_iso_meta:crash/rc=1` |
| T87 | decode: name_len upper bound 64 | `test_iso_meta:crash/rc=0` |
| T88 | decode: name_len lower bound 2 | `test_decode_roundtrip`, `test_decode_rejects_bad_name_length` |
| T89 | decode: name not read | `test_decode_roundtrip`, `test_decode_expected_bytes_case_a` |
| T91 | decode: generation not read | `test_decode_roundtrip`, `test_decode_expected_bytes_case_a` |
| T92 | decode: validation removed | `test_decode_rejects_out_of_range_fields_with_valid_crc`, `test_decode_applies_the_name_rule` |
| T93 | decode: out not written | `test_decode_roundtrip`, `test_decode_expected_bytes_case_a` |
| S01 | trans: BEGIN from any state | `test_transition_table_every_cell`, `test_transition_next_may_be_null` |
| S02 | trans: BEGIN goes to incomplete | `test_transition_table_every_cell` |
| S03 | trans: PUT rejected in uploading | `test_transition_table_every_cell`, `test_put_rows_by_flags` |
| S04 | trans: PUT ignores the slot | `test_put_rows_by_flags`, `test_null_ctx_means_all_flags_false` |
| S05 | trans: PUT resume stays incomplete | `test_transition_table_every_cell`, `test_put_rows_by_flags` |
| S06 | trans: PUT busy slot is a state error | `test_put_rows_by_flags`, `test_null_ctx_means_all_flags_false` |
| S07 | trans: PUT accepted when ready | `test_transition_table_every_cell`, `test_transition_rejection_leaves_next_equal_to_current` |
| S08 | trans: idle timeout from incomplete too | `test_transition_table_every_cell` |
| S09 | trans: idle timeout goes to invalid | `test_transition_table_every_cell` |
| S10 | trans: reboot keeps uploading | `test_transition_table_every_cell` |
| S11 | trans: reboot keeps finalizing | `test_transition_table_every_cell` |
| S12 | trans: reboot of deleting rejected | `test_transition_table_every_cell` |
| S13 | trans: reboot of invalid rejected | `test_transition_table_every_cell` |
| S14 | trans: reboot of ready rejected | `test_transition_table_every_cell` |
| S15 | trans: reboot of incomplete rejected | `test_transition_table_every_cell` |
| S16 | trans: finish size mismatch not detected | `test_transition_rejection_leaves_next_equal_to_current`, `test_finish_rows_by_flags` |
| S17 | trans: finish size mismatch reported as state | `test_transition_rejection_leaves_next_equal_to_current`, `test_finish_rows_by_flags` |
| S18 | trans: finish busy slot ignored | `test_finish_rows_by_flags` |
| S19 | trans: finish busy slot also for uploading | `test_finish_rows_by_flags` |
| S20 | trans: finish busy slot is a state error | `test_finish_rows_by_flags` |
| S21 | trans: finish goes to ready | `test_transition_table_every_cell`, `test_finish_rows_by_flags` |
| S22 | trans: finish while finalizing rejected | `test_transition_table_every_cell`, `test_finish_rows_by_flags` |
| S23 | trans: finish of ready rejected | `test_transition_table_every_cell`, `test_finish_rows_by_flags` |
| S24 | trans: finish of invalid is a state error | `test_transition_table_every_cell`, `test_finish_rows_by_flags` |
| S25 | trans: finalize ok from any state | `test_transition_table_every_cell` |
| S26 | trans: finalize ok goes to invalid | `test_transition_table_every_cell` |
| S27 | trans: finalize fail goes to incomplete | `test_transition_table_every_cell` |
| S28 | trans: finalize io error goes to invalid | `test_transition_table_every_cell` |
| S29 | trans: finalize fail from uploading | `test_transition_table_every_cell` |
| S30 | trans: index ok rejected | `test_transition_table_every_cell` |
| S31 | trans: index io error leaves ready (accepted from any state) | `test_transition_table_every_cell` |
| S32 | trans: index fail stays ready | `test_transition_table_every_cell` |
| S33 | trans: index fail from any state | `test_transition_table_every_cell` |
| S34 | trans: delete of none accepted | `test_transition_table_every_cell`, `test_delete_rows_by_flags` |
| S35 | trans: delete ignores mounted | `test_transition_rejection_leaves_next_equal_to_current`, `test_delete_rows_by_flags` |
| S36 | trans: delete rejects mounted in any state | `test_delete_rows_by_flags` |
| S37 | trans: delete rejects every ready image | `test_transition_table_every_cell`, `test_delete_rows_by_flags` |
| S38 | trans: delete mounted is a state error | `test_transition_rejection_leaves_next_equal_to_current`, `test_delete_rows_by_flags` |
| S39 | trans: delete goes to none | `test_transition_table_every_cell`, `test_delete_rows_by_flags` |
| S40 | trans: delete done from any state | `test_transition_table_every_cell` |
| S41 | trans: delete done goes to deleting | `test_transition_table_every_cell` |
| S43 | trans: next not written | `test_transition_table_every_cell`, `test_finish_rows_by_flags` |
| S44 | trans: NULL ctx not defaulted | `test_iso_transitions:crash/rc=1` |
| S45 | trans: default ctx is all true | `test_transition_rejection_leaves_next_equal_to_current`, `test_null_ctx_means_all_flags_false` |
| S46 | state_to_meta: ready -> 2 | `test_state_meta_mapping` |
| S47 | state_to_meta: deleting -> 3 | `test_state_meta_mapping` |
| S48 | state_to_meta: finalizing stored as ready | `test_state_meta_mapping` |
| S49 | state_to_meta: uploading -> 2 | `test_state_meta_mapping` |
| S50 | state_to_meta: invalid -> 0 | `test_state_meta_mapping` |
| S51 | state_from_meta: 1 -> incomplete | `test_state_meta_mapping` |
| S52 | state_from_meta: 3 -> ready | `test_state_meta_mapping` |
| S53 | state_from_meta: 4 -> invalid | `test_state_meta_mapping` |
| S54 | state_from_meta: 2 -> uploading | `test_state_meta_mapping` |
| S55 | state_from_meta: 0 -> ready | `test_state_meta_mapping` |
| S56 | state_from_meta: unknown -> ready | `test_state_meta_mapping` |
| L01 | choose: neither valid -> a | `test_slot_choose_table`, `test_slots_read_directory_without_meta` |
| L02 | choose: only a -> b | `test_slot_choose_table`, `test_slots_read_only_a_and_only_b` |
| L03 | choose: only b -> a | `test_slot_choose_table`, `test_slots_read_only_a_and_only_b` |
| L04 | choose: only-a case removed | `test_slot_choose_table` |
| L05 | choose: only-b case removed | `test_slot_choose_table` |
| L06 | choose: same generation -> b | `test_slot_choose_table`, `test_slots_read_picks_newer_generation` |
| L07 | choose: plain unsigned comparison (no wraparound) | `test_slot_choose_table`, `test_slots_read_generation_wraparound` |
| L08 | choose: direction swapped | `test_slot_choose_table`, `test_slots_read_picks_newer_generation` |
| L09 | choose: signed 16-bit difference | `test_slot_choose_table` |
| L10 | choose: always a | `test_slot_choose_table`, `test_slots_read_picks_newer_generation` |
| L11 | read_slot: not-found is an error | `test_slots_read_directory_without_meta`, `test_slots_read_only_a_and_only_b` |
| L12 | read_slot: stat error ignored | `test_slots_read_io_error_is_not_treated_as_invalid` |
| L13 | read_slot: directory accepted | `test_slots_read_directory_with_the_size_of_a_slot_is_invalid` |
| L14 | read_slot: size check removed | `test_slots_read_slot_length_boundaries`, `test_slots_read_short_slot_is_not_opened` |
| L15 | read_slot: size >= 256 accepted | `test_slots_read_slot_length_boundaries` |
| L16 | read_slot: size < 256 accepted | `test_slots_read_short_slot_is_not_opened` |
| L17 | read_slot: open error ignored | `test_slots_read_open_and_read_errors_are_not_treated_as_invalid` |
| L18 | read_slot: read error ignored | `test_slots_read_open_and_read_errors_are_not_treated_as_invalid` |
| L20 | read_slot: file not closed | `test_slots_read_only_a_and_only_b`, `test_slots_read_picks_newer_generation` |
| L21 | read_slot: decode result ignored | `test_slots_read_corrupt_slot_falls_back_to_the_other` |
| L22 | slots_read: bad id accepted | `test_slots_read_rejects_bad_id_and_args` |
| L23 | slots_read: NULL fs accepted | `test_iso_slots:crash/rc=1` |
| L24 | slots_read: NULL out accepted | `test_iso_slots:crash/rc=1` |
| L25 | slots_read: only slot a read | `test_slots_read_only_a_and_only_b`, `test_slots_read_picks_newer_generation` |
| L26 | slots_read: read error ignored | `test_slots_read_io_error_is_not_treated_as_invalid`, `test_slots_read_open_and_read_errors_are_not_treated_as_invalid` |
| L27 | slots_read: adopted not set | `test_slots_read_directory_without_meta`, `test_slots_read_only_a_and_only_b` |
| L28 | slots_read: generations swapped | `test_slots_read_picks_newer_generation`, `test_slots_read_generation_wraparound` |
| L29 | slot path: slot b is meta.a | `test_slots_read_only_a_and_only_b`, `test_slots_read_picks_newer_generation` |
| L30 | write_slot: encode error ignored | `test_write_meta_rejects_an_invalid_meta_without_writing` |
| L31 | write_slot: writes 255 bytes | `test_begin_creates_directory_then_meta_a`, `test_begin_with_declared_sha256` |
| C01 | init: fs not stored | `test_iso_begin_apply:crash/rc=1` |
| C02 | init: rnd not stored | `test_iso_begin_apply:crash/rc=1` |
| C03 | ensure_dirs: /iso not created | `test_scan_empty_card_creates_directories`, `test_ensure_dirs_reports_each_failure` |
| C04 | ensure_dirs: /iso error ignored | `test_ensure_dirs_reports_each_failure` |
| C05 | ensure_dirs: import dir not created | `test_scan_empty_card_creates_directories`, `test_ensure_dirs_reports_each_failure` |
| C06 | ensure_dirs: EXISTS is an error | `test_apply_delete_clears_reason_and_resume_mark`, `test_apply_generation_wraps_from_uint32_max_to_zero` |
| C07 | find: NULL id not handled | `test_iso_scan:crash/rc=1` |
| C08 | find: always the first | `test_begin_never_uses_the_reserved_id` |
| C09 | find: compares 7 characters | `test_begin_never_uses_the_reserved_id`, `test_apply_finalize_ok_sha_variants` |
| C10 | find: skips the last entry | `test_iso_begin_apply:crash/rc=1` |
| C11 | remove: entries not shifted | `test_remove_entry_in_the_middle_and_at_the_end` |
| C12 | remove: count not decremented | `test_apply_delete_and_delete_done` |
| C14 | remove: shifts one too few | `test_remove_entry_in_the_middle_and_at_the_end` |
| C15 | remove: stops after the first image | `test_remove_entry_in_the_middle_and_at_the_end` |
| C16 | name_exists: invalid-meta entries compared | `test_name_of_deleting_image_does_not_block_and_corrupt_has_no_name` |
| C17 | name_exists: deleting images compared | `test_name_of_deleting_image_does_not_block_and_corrupt_has_no_name` |
| C18 | name_exists: ready images skipped | `test_import_same_name_and_limit` |
| C19 | name_exists: always false after the first | `test_begin_same_name_is_rejected_by_exact_bytes`, `test_begin_image_limit_and_precedence` |
| C20 | name_exists: compares only the length | `test_begin_part_count_for_sizes`, `test_begin_same_name_is_rejected_by_exact_bytes` |
| C21 | received: stops at the first part | `test_scan_state_mapping_per_image`, `test_received_full_parts_and_last_short_part` |
| C22 | received: parts beyond part_count counted | `test_received_follows_part_count_of_the_meta` |
| C23 | received: expected size 0 not a stop | `test_received_does_not_look_at_parts_beyond_the_expected_count` |
| C24 | received: missing part is an error | `test_apply_delete_clears_reason_and_resume_mark`, `test_apply_generation_wraps_from_uint32_max_to_zero` |
| C25 | received: stat error ignored | `test_scan_io_errors_are_propagated`, `test_received_propagates_io_error` |
| C26 | received: directory counted | `test_received_part_that_is_a_directory_stops` |
| C27 | received: oversized part counted by its size | `test_received_clamps_oversized_part_and_ignores_parts_after_a_gap`, `test_received_single_part_import_size` |
| C28 | received: full part boundary > | `test_scan_state_mapping_per_image`, `test_received_full_parts_and_last_short_part` |
| C29 | received: short part not counted | `test_scan_state_mapping_per_image`, `test_received_counts_parts_in_order_and_stops_at_a_short_part` |
| C30 | received: short part does not stop | `test_received_counts_parts_in_order_and_stops_at_a_short_part` |
| C31 | received: *out not set | `test_scan_state_mapping_per_image`, `test_received_counts_parts_in_order_and_stops_at_a_short_part` |
| C32 | received: part path index off by one | `test_scan_state_mapping_per_image`, `test_scan_io_errors_are_propagated` |
| C33 | load: slots read error ignored | `test_scan_io_errors_are_propagated` |
| C34 | load: id not copied | `test_iso_begin_apply:crash/rc=1` |
| C35 | load: adopted not recorded | `test_apply_generation_wraps_from_uint32_max_to_zero` |
| C36 | load: corrupt entry is ready | `test_apply_on_corrupt_entry`, `test_apply_reboot_keeps_the_invalid_reason` |
| C37 | load: corrupt reason not set | `test_apply_reboot_keeps_the_invalid_reason` |
| C38 | load: valid meta flag not set | `test_apply_generation_wraps_from_uint32_max_to_zero` |
| C39 | load: wrong slot's meta | `test_apply_delete_clears_reason_and_resume_mark`, `test_apply_generation_wraps_from_uint32_max_to_zero` |
| C40 | load: reboot mapping skipped (uploading stays uploading) | `test_scan_state_mapping_per_image`, `test_scan_resume_delete_only_for_deleting` |
| C41 | load: resume_delete for incomplete | `test_scan_resume_delete_only_for_deleting` |
| C42 | load: resume_delete never set | `test_apply_delete_clears_reason_and_resume_mark` |
| C43 | load: invalid reason not copied | `test_apply_delete_clears_reason_and_resume_mark`, `test_apply_reboot_keeps_the_invalid_reason` |
| C44 | load: ready received is 0 | `test_scan_state_mapping_per_image` |
| C45 | load: incomplete received not computed | `test_scan_state_mapping_per_image`, `test_scan_io_errors_are_propagated` |
| C46 | load: invalid received not computed | `test_scan_state_mapping_per_image` |
| C47 | load: received error ignored | `test_scan_io_errors_are_propagated` |
| C48 | load: deleting received computed | `test_scan_state_mapping_per_image` |
| C49 | scan: count not reset | `test_iso_scan:crash/rc=1` |
| C50 | scan: hidden not reset | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order`, `test_scan_reserved_id_does_not_take_one_of_the_16_slots` |
| C52 | scan: directories not ensured | `test_scan_empty_card_creates_directories`, `test_ensure_dirs_reports_each_failure` |
| C53 | scan: ensure error ignored | `test_ensure_dirs_reports_each_failure` |
| C54 | scan: dir_open error ignored | `test_scan_io_errors_are_propagated` |
| C55 | scan: dir_next error ignored | `test_scan_io_errors_are_propagated` |
| C56 | scan: dir not closed on error | `test_scan_io_errors_are_propagated` |
| C57 | scan: dir not closed | `test_apply_delete_clears_reason_and_resume_mark`, `test_apply_generation_wraps_from_uint32_max_to_zero` |
| C58 | scan: files accepted as images | `test_scan_ignores_directories_that_are_not_ids`, `test_scan_reserved_name_as_a_file_is_ignored_not_counted` |
| C59 | scan: bad ids accepted | `test_apply_delete_clears_reason_and_resume_mark`, `test_apply_generation_wraps_from_uint32_max_to_zero` |
| C60 | scan: image limit +1 (overflows the array) | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order`, `test_scan_exactly_16_images_has_no_hidden` |
| C61 | scan: hidden not counted | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order`, `test_scan_reserved_id_does_not_take_one_of_the_16_slots` |
| C62 | scan: hidden counted twice | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order`, `test_scan_reserved_id_does_not_take_one_of_the_16_slots` |
| C63 | scan: id copied without NUL | `test_apply_delete_clears_reason_and_resume_mark`, `test_apply_generation_wraps_from_uint32_max_to_zero` |
| C64 | scan: load error ignored | `test_scan_io_errors_are_propagated` |
| C65 | scan: count not incremented | `test_iso_begin_apply:crash/rc=1` |
| P01 | reserved: generate does not redraw zero | `test_id_generate_redraws_when_the_random_value_is_zero`, `test_id_generate_zero_draws_count_as_tries_and_never_create_the_reserved_directory` |
| P02 | reserved: generate redraws every nonzero value | `test_begin_creates_directory_then_meta_a`, `test_begin_with_declared_sha256` |
| P03 | reserved: generate redraws 1 instead of 0 | `test_id_generate_formats_lowercase_hex_with_padding`, `test_id_generate_redraws_when_the_random_value_is_zero` |
| P04 | reserved: generate gives up at the first zero | `test_id_generate_redraws_when_the_random_value_is_zero`, `test_id_generate_zero_draws_count_as_tries_and_never_create_the_reserved_directory` |
| P05 | reserved: a zero draw is not counted as a try (a source that returns only 0 never ends) | `test_iso_begin_apply:crash/rc=1` |
| P06 | reserved: generate redraws values below 0x10000 (low half zero) | `test_begin_retries_the_id_on_collision`, `test_id_generate_formats_lowercase_hex_with_padding` |
| P07 | reserved: scan adopts the reserved id | `test_scan_does_not_adopt_the_reserved_id_and_counts_it_as_hidden`, `test_scan_counts_an_empty_reserved_directory_as_hidden_without_reading_it` |
| P08 | reserved: scan skips every id except the reserved one | `test_iso_begin_apply:crash/rc=1` |
| P09 | reserved: scan does not count it as hidden | `test_scan_does_not_adopt_the_reserved_id_and_counts_it_as_hidden`, `test_scan_counts_an_empty_reserved_directory_as_hidden_without_reading_it` |
| P10 | reserved: scan counts it as hidden and adopts it | `test_scan_does_not_adopt_the_reserved_id_and_counts_it_as_hidden`, `test_scan_counts_an_empty_reserved_directory_as_hidden_without_reading_it` |
| P11 | reserved: scan counts it as hidden twice | `test_scan_does_not_adopt_the_reserved_id_and_counts_it_as_hidden`, `test_scan_counts_an_empty_reserved_directory_as_hidden_without_reading_it` |
| P12 | reserved: scan compares only the first 7 digits | `test_iso_begin_apply:crash/rc=1` |
| P13 | reserved: scan compares only the first digit | `test_iso_begin_apply:crash/rc=1` |
| P14 | reserved: scan reserves 00000001 instead | `test_iso_begin_apply:crash/rc=1` |
| P15 | reserved: scan takes a slot for the reserved id and counts it hidden afterwards | `test_scan_does_not_adopt_the_reserved_id_and_counts_it_as_hidden`, `test_scan_counts_an_empty_reserved_directory_as_hidden_without_reading_it` |
| B01 | id: tries 15 (equivalent boundary is tested at 16) | `test_id_generate_gives_up_after_16_tries`, `test_id_generate_zero_draws_count_as_tries_and_never_create_the_reserved_directory` |
| B02 | id: tries 17 | `test_id_generate_gives_up_after_16_tries`, `test_id_generate_zero_draws_count_as_tries_and_never_create_the_reserved_directory` |
| B03 | id: uppercase hex | `test_begin_retries_the_id_on_collision`, `test_id_generate_formats_lowercase_hex_with_padding` |
| B04 | id: no zero padding | `test_begin_retries_the_id_on_collision`, `test_id_generate_formats_lowercase_hex_with_padding` |
| B05 | id: 7 digits | `test_begin_retries_the_id_on_collision`, `test_id_generate_formats_lowercase_hex_with_padding` |
| B06 | id: random value not used | `test_begin_creates_directory_then_meta_a`, `test_begin_part_count_for_sizes` |
| B07 | id: mkdir error treated as retry | `test_begin_mkdir_error_other_than_exists_is_returned_without_retry`, `test_id_generate_clears_the_output_on_a_mkdir_error` |
| B08 | id: every mkdir error ends with exists | `test_power_cut_at_every_write_and_every_tear`, `test_power_cut_during_import` |
| B09 | id: success not returned | `test_begin_creates_directory_then_meta_a`, `test_begin_with_declared_sha256` |
| B10 | id: exhausted code is exists | `test_id_generate_gives_up_after_16_tries`, `test_id_generate_zero_draws_count_as_tries_and_never_create_the_reserved_directory` |
| B11 | id: output not cleared when exhausted | `test_id_generate_gives_up_after_16_tries`, `test_id_generate_zero_draws_count_as_tries_and_never_create_the_reserved_directory` |
| B12 | id: output not cleared on mkdir error | `test_id_generate_clears_the_output_on_a_mkdir_error` |
| B13 | cleanup: meta.a not removed | `test_begin_meta_write_failure_cleans_up_the_directory` |
| B14 | cleanup: directory not removed | `test_begin_meta_write_failure_cleans_up_the_directory` |
| B15 | cleanup: removes meta.b instead of meta.a | `test_begin_meta_write_failure_cleans_up_the_directory` |
| B17 | begin: NULL out accepted | `test_iso_begin_apply:crash/rc=1` |
| B18 | begin: name rule not applied | `test_begin_validates_before_touching_the_card` |
| B19 | begin: size lower bound removed | `test_begin_validates_before_touching_the_card`, `test_begin_image_limit_and_precedence` |
| B20 | begin: size lower bound <= | `test_begin_part_count_for_sizes`, `test_begin_validates_before_touching_the_card` |
| B21 | begin: size upper bound >= | `test_begin_part_count_for_sizes`, `test_begin_validates_before_touching_the_card` |
| B22 | begin: size upper bound removed | `test_begin_validates_before_touching_the_card` |
| B23 | begin: same name allowed | `test_begin_same_name_is_rejected_by_exact_bytes`, `test_begin_image_limit_and_precedence` |
| B24 | begin: name exists reported as too many | `test_begin_same_name_is_rejected_by_exact_bytes`, `test_begin_image_limit_and_precedence` |
| B25 | begin: limit check > | `test_iso_begin_apply:crash/rc=1` |
| B26 | begin: limit removed | `test_iso_begin_apply:crash/rc=1` |
| B27 | begin: limit before the name check | `test_begin_image_limit_and_precedence` |
| B28 | begin: id error ignored | `test_begin_never_uses_the_reserved_id`, `test_begin_returns_id_exhausted_and_changes_nothing` |
| B29 | begin: state not uploading | `test_begin_creates_directory_then_meta_a`, `test_apply_idle_timeout_writes_incomplete_to_the_other_slot` |
| B30 | begin: size not stored | `test_begin_creates_directory_then_meta_a`, `test_begin_with_declared_sha256` |
| B31 | begin: part_blocks 1 GiB in bytes | `test_begin_creates_directory_then_meta_a` |
| B32 | begin: part count off by one | `test_begin_creates_directory_then_meta_a`, `test_begin_part_count_for_sizes` |
| B33 | begin: sha ignored | `test_begin_with_declared_sha256`, `test_apply_finalize_ok_sha_variants` |
| B34 | begin: sha flag computed | `test_begin_with_declared_sha256`, `test_apply_finalize_ok_sha_variants` |
| B35 | begin: sha value not copied | `test_begin_with_declared_sha256`, `test_apply_sha_update_only_for_finalize_ok_with_set_sha` |
| B36 | begin: sha always declared | `test_iso_begin_apply:crash/rc=1` |
| B37 | begin: name length wrong | `test_begin_creates_directory_then_meta_a`, `test_begin_with_declared_sha256` |
| B38 | begin: name not copied | `test_begin_creates_directory_then_meta_a`, `test_begin_with_declared_sha256` |
| B39 | begin: generation 0 | `test_begin_creates_directory_then_meta_a`, `test_apply_idle_timeout_writes_incomplete_to_the_other_slot` |
| B40 | begin: generation 2 | `test_begin_creates_directory_then_meta_a`, `test_apply_idle_timeout_writes_incomplete_to_the_other_slot` |
| B41 | begin: writes meta.b | `test_begin_creates_directory_then_meta_a`, `test_begin_with_declared_sha256` |
| B42 | begin: write error ignored | `test_begin_meta_write_failure_cleans_up_the_directory` |
| B43 | begin: no cleanup after a write error | `test_begin_meta_write_failure_cleans_up_the_directory` |
| B44 | begin: entry not added | `test_iso_begin_apply:crash/rc=1` |
| B45 | begin: entry state ready | `test_begin_creates_directory_then_meta_a`, `test_apply_idle_timeout_writes_incomplete_to_the_other_slot` |
| B46 | begin: entry meta_valid not set | `test_begin_creates_directory_then_meta_a`, `test_begin_same_name_is_rejected_by_exact_bytes` |
| B47 | begin: entry adopted b | `test_begin_creates_directory_then_meta_a`, `test_apply_idle_timeout_writes_incomplete_to_the_other_slot` |
| B48 | begin: entry meta not stored | `test_begin_creates_directory_then_meta_a`, `test_begin_with_declared_sha256` |
| B49 | begin: out id not returned | `test_iso_begin_apply:crash/rc=1` |
| B50 | begin: entry id not stored | `test_iso_begin_apply:crash/rc=1` |
| W01 | write_meta: corrupt entry accepted (equivalent: encode of the stale meta is not reached) | `test_apply_on_corrupt_entry` |
| W02 | write_meta: generation not incremented | `test_apply_idle_timeout_writes_incomplete_to_the_other_slot`, `test_apply_put_resume_writes_nothing` |
| W03 | write_meta: generation +2 | `test_apply_idle_timeout_writes_incomplete_to_the_other_slot`, `test_apply_put_resume_writes_nothing` |
| W04 | write_meta: generation capped at UINT32_MAX | `test_apply_generation_wraps_from_uint32_max_to_zero` |
| W05 | write_meta: always writes meta.a | `test_apply_idle_timeout_writes_incomplete_to_the_other_slot`, `test_apply_generations_alternate_slots` |
| W06 | write_meta: writes the adopted slot | `test_apply_idle_timeout_writes_incomplete_to_the_other_slot`, `test_apply_generations_alternate_slots` |
| W07 | write_meta: write error ignored | `test_apply_meta_write_failure_changes_nothing_and_retry_uses_the_same_slot`, `test_write_meta_rejects_an_invalid_meta_without_writing` |
| W08 | write_meta: meta not updated | `test_apply_idle_timeout_writes_incomplete_to_the_other_slot`, `test_apply_put_resume_writes_nothing` |
| W09 | write_meta: adopted not updated | `test_apply_idle_timeout_writes_incomplete_to_the_other_slot`, `test_apply_generations_alternate_slots` |
| W10 | apply: received_complete from the caller | `test_apply_finish_and_finalize_ok`, `test_apply_finalize_ok_sha_variants` |
| W11 | apply: complete when received >= size | `test_apply_finish_and_finalize_ok` |
| W12 | apply: complete when received <= size | `test_apply_finish_and_finalize_ok` |
| W14 | apply: ctx ignored | `test_apply_put_resume_writes_nothing`, `test_apply_generations_alternate_slots` |
| W15 | apply: rejection returns 0 | `test_apply_put_resume_writes_nothing`, `test_apply_finish_and_finalize_ok` |
| W16 | apply: rejection still applies | `test_apply_put_resume_writes_nothing`, `test_apply_finish_and_finalize_ok` |
| W17 | apply: idle timeout does not write | `test_apply_idle_timeout_writes_incomplete_to_the_other_slot`, `test_apply_put_resume_writes_nothing` |
| W18 | apply: write fail does not write | `test_apply_io_error_and_write_fail_go_to_incomplete` |
| W19 | apply: finalize ok does not write | `test_apply_finish_and_finalize_ok`, `test_apply_finalize_ok_sha_variants` |
| W20 | apply: finalize fail does not write | `test_apply_invalid_requires_a_valid_reason`, `test_apply_each_invalid_reason_is_persisted` |
| W21 | apply: finalize io error does not write | `test_apply_io_error_and_write_fail_go_to_incomplete` |
| W22 | apply: index fail does not write | `test_apply_index_events_on_ready` |
| W23 | apply: finish writes meta | `test_apply_finish_and_finalize_ok`, `test_apply_finalize_ok_sha_variants` |
| W24 | apply: put writes meta | `test_apply_put_resume_writes_nothing`, `test_apply_generations_alternate_slots` |
| W25 | apply: delete writes meta | `test_apply_delete_and_delete_done`, `test_apply_on_corrupt_entry` |
| W26 | apply: index ok writes meta | `test_apply_index_events_on_ready` |
| W28 | apply: persisted state not set | `test_apply_idle_timeout_writes_incomplete_to_the_other_slot`, `test_apply_finish_and_finalize_ok` |
| W29 | apply: invalid without arg accepted | `test_apply_invalid_requires_a_valid_reason` |
| W30 | apply: reason 0 accepted | `test_apply_invalid_requires_a_valid_reason` |
| W31 | apply: reason 1 rejected | `test_apply_each_invalid_reason_is_persisted` |
| W33 | apply: reason 4 rejected | `test_apply_each_invalid_reason_is_persisted`, `test_apply_index_events_on_ready` |
| W34 | apply: reason not saved to meta | `test_apply_each_invalid_reason_is_persisted` |
| W35 | apply: reason not recorded | `test_apply_each_invalid_reason_is_persisted`, `test_apply_index_events_on_ready` |
| W36 | apply: sha update also for other events | `test_apply_sha_update_only_for_finalize_ok_with_set_sha` |
| W37 | apply: sha update ignores set_sha | `test_apply_sha_update_only_for_finalize_ok_with_set_sha` |
| W38 | apply: sha computed without value accepted | `test_iso_begin_apply:crash/rc=1` |
| W39 | apply: sha value not copied | `test_apply_finish_and_finalize_ok` |
| W40 | apply: sha flag not set to computed | `test_apply_finish_and_finalize_ok` |
| W41 | apply: sha none keeps the value | `test_apply_finalize_ok_sha_variants` |
| W42 | apply: sha none keeps the flag | `test_apply_finalize_ok_sha_variants` |
| W43 | apply: sha declared accepted | `test_apply_finalize_ok_sha_variants` |
| W44 | apply: write error ignored | `test_apply_meta_write_failure_changes_nothing_and_retry_uses_the_same_slot` |
| W45 | apply: none state does not remove the entry | `test_apply_delete_and_delete_done` |
| W46 | apply: state not updated | `test_apply_idle_timeout_writes_incomplete_to_the_other_slot`, `test_apply_put_resume_writes_nothing` |
| W47 | apply: invalid reason not set | `test_apply_each_invalid_reason_is_persisted`, `test_apply_index_events_on_ready` |
| W48 | apply: invalid reason kept after leaving invalid | `test_apply_delete_clears_reason_and_resume_mark` |
| W49 | apply: resume_delete not cleared on delete | `test_apply_delete_clears_reason_and_resume_mark` |
| D01 | delete init: bad id accepted | `test_iso_delete:crash/rc=1` |
| D02 | delete init: NULL job accepted | `test_iso_delete:crash/rc=1` |
| D03 | delete init: id not copied | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D04 | delete init: starts at the files phase | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D06 | delete: remove treats NOT_FOUND as an error | `test_delete_resume_with_only_the_new_slot_left`, `test_delete_meta_corrupt_directory` |
| D07 | delete: missing directory is an error | `test_delete_directory_already_gone_is_done` |
| D08 | delete: dir_open error ignored | `test_delete_read_errors_are_reported` |
| D09 | delete: dir_next error ignored | `test_delete_read_errors_are_reported` |
| D10 | delete: all entries deleted, not only p* | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D11 | delete: only p0* names (prefix 'p0') | `test_delete_only_removes_p_entries_and_fails_on_foreign_files` |
| D12 | delete: meta files taken as parts | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D13 | delete: found flag not set | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D14 | delete: dir not closed after finding | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D15 | delete init: slot read error ignored | `test_delete_read_errors_are_reported` |
| D16 | delete init: keep not recorded | `test_delete_resume_when_slot_a_holds_the_deleting_meta` |
| D17 | delete init: always rewrites meta | `test_delete_resume_when_slot_a_holds_the_deleting_meta`, `test_delete_resume_when_meta_already_says_deleting` |
| D18 | delete init: never writes meta | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D19 | delete init: writes even without a valid slot | `test_delete_meta_corrupt_directory`, `test_delete_directory_already_gone_is_done` |
| D20 | delete init: state not set to deleting | `test_delete_full_sequence_and_order`, `test_delete_never_resurrects_ready_after_step_1` |
| D21 | delete init: generation not incremented | `test_delete_full_sequence_and_order`, `test_delete_never_resurrects_ready_after_step_1` |
| D22 | delete init: writes the adopted slot | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D23 | delete init: write error ignored | `test_delete_error_at_every_write_is_reported_and_retried`, `test_delete_first_step_write_failure_changes_nothing` |
| D24 | delete init: keep not moved to the new slot | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D25 | delete init: catalog entry meta not updated | `test_delete_full_sequence_and_order` |
| D26 | delete init: catalog entry adopted not updated | `test_delete_full_sequence_and_order` |
| D27 | delete init: catalog state not set | `test_delete_first_step_write_failure_changes_nothing` |
| D28 | delete init: does not continue to files | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D29 | delete init: returns done | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D30 | delete files: error ignored | `test_delete_read_errors_are_reported` |
| D31 | delete files: no fall through when none found | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D32 | delete files: stays in the phase when none found | `test_iso_scan:timeout` |
| D33 | delete files: remove error ignored | `test_delete_error_keeps_the_phase_and_retries_the_same_operation`, `test_delete_error_at_every_write_is_reported_and_retried` |
| D34 | delete files: removes the directory path | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D36 | delete slots: new slot first (wrong order) | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D37 | delete slots: new slot second is the old one | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D38 | delete slots: corrupt dir removes a twice | `test_delete_meta_corrupt_directory` |
| D39 | delete slots: corrupt dir removes b twice | `test_delete_meta_corrupt_directory` |
| D40 | delete slots: old slot error ignored | `test_delete_error_at_every_write_is_reported_and_retried` |
| D41 | delete slots: new slot error ignored | `test_delete_error_at_every_write_is_reported_and_retried` |
| D42 | delete slots: old slot phase skipped | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D43 | delete slots: new slot phase skipped | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D44 | delete slots: old slot phase does not advance | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D45 | delete rmdir: error ignored | `test_delete_only_removes_p_entries_and_fails_on_foreign_files`, `test_delete_error_at_every_write_is_reported_and_retried` |
| D46 | delete rmdir: entry not removed from the catalog | `test_delete_full_sequence_and_order`, `test_delete_meta_corrupt_directory` |
| D47 | delete rmdir: returns continue | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| D48 | delete rmdir: phase not set to done (equivalent: DONE is idempotent via rmdir NOT_FOUND) | `test_delete_full_sequence_and_order` |
| D49 | delete: error kind is continue | `test_delete_only_removes_p_entries_and_fails_on_foreign_files`, `test_delete_error_keeps_the_phase_and_retries_the_same_operation` |
| D50 | delete: error code dropped | `test_delete_only_removes_p_entries_and_fails_on_foreign_files`, `test_delete_error_keeps_the_phase_and_retries_the_same_operation` |
| D51 | delete: done kind is error | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| D52 | delete: after done, restarts | `test_delete_full_sequence_and_order` |
| I01 | import: extension .iso only lower case | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored` |
| I02 | import: extension 'I' ignored | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored` |
| I03 | import: extension 'S' ignored | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored` |
| I04 | import: extension 'O' ignored | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored` |
| I05 | import: dot not required | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored` |
| I06 | import: length check removed (name shorter than 4 reads before the buffer) | `test_iso_import:crash/rc=1` |
| I07 | import: length check <= 4 | `test_import_name_rules` |
| I08 | import: extension checked one byte before the end | `test_iso_import:crash/rc=1` |
| I09 | signature: offset 15 sectors | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I10 | signature: offset 17 sectors | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I11 | signature: offset 16 * 512 | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I12 | signature: open error ignored | `test_rescan_io_errors_are_propagated`, `test_rescan_counts_before_an_error_are_kept` |
| I13 | signature: read_at offset 0 | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I14 | signature: reads 5 bytes only | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I15 | signature: reads from SIG_OFFSET + 1 | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I16 | signature: sequential read instead of read_at (the file start) | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I18 | signature: file not closed | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I19 | signature: read error ignored | `test_rescan_read_errors_while_checking_the_signature` |
| I20 | signature: read length 7 | `test_iso_import:crash/rc=1` |
| I21 | signature: compares from byte 0 | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I22 | signature: CD001 not accepted | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I23 | signature: BEA01 not accepted | `test_import_signature` |
| I24 | signature: compares 4 bytes | `test_import_signature` |
| I25 | signature: ok not set | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I26 | crosslink: zero cluster compared | `test_import_cluster_zero_never_counts_as_crosslink` |
| I27 | crosslink: never reports | `test_import_crosslinked_file_is_skipped_and_left_alone`, `test_import_crosslink_between_two_files_in_one_batch` |
| I28 | crosslink: missing p000 is an error | `test_import_same_name_and_limit`, `test_import_image_limit` |
| I29 | crosslink: stat error ignored | `test_rescan_crosslink_stat_error_is_propagated` |
| I30 | crosslink: only the first image checked | `test_import_crosslink_is_checked_against_every_image_and_ignores_directories` |
| I31 | crosslink: compares p001 | `test_import_crosslinked_file_is_skipped_and_left_alone`, `test_import_crosslink_between_two_files_in_one_batch` |
| I32 | crosslink: cluster compare inverted | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored`, `test_import_name_rules` |
| I33 | crosslink: directory counted | `test_import_crosslink_is_checked_against_every_image_and_ignores_directories` |
| I34 | import: name rule not applied | `test_import_name_rules` |
| I35 | import: name rule applied to the whole file name | `test_import_name_rules` |
| I36 | import: size lower bound removed | `test_import_size_boundaries` |
| I37 | import: size lower bound <= | `test_import_size_boundaries` |
| I38 | import: part blocks limit removed | `test_import_part_size_limit` |
| I39 | import: part blocks limit >= | `test_import_part_size_limit` |
| I40 | import: same name allowed | `test_import_same_name_and_limit` |
| I41 | import: name compared with extension | `test_import_same_name_and_limit` |
| I42 | import: image limit removed | `test_iso_import:crash/rc=1` |
| I43 | import: image limit > | `test_iso_import:crash/rc=1` |
| I44 | import: crosslink not checked | `test_import_crosslinked_file_is_skipped_and_left_alone`, `test_import_crosslink_between_two_files_in_one_batch` |
| I45 | import: crosslink error ignored | `test_rescan_crosslink_stat_error_is_propagated` |
| I46 | import: signature not checked | `test_import_signature`, `test_import_signature_is_read_from_sector_16_bytes_1_to_5` |
| I47 | import: signature error ignored | `test_rescan_io_errors_are_propagated`, `test_rescan_read_errors_while_checking_the_signature` |
| I48 | import: id error ignored | `test_import_write_failures_clean_up_and_keep_the_file` |
| I49 | import: meta state incomplete | `test_import_one_file` |
| I50 | import: size not stored | `test_import_one_file`, `test_import_size_boundaries` |
| I51 | import: 2 parts | `test_import_one_file` |
| I52 | import: default part blocks | `test_import_one_file`, `test_import_size_boundaries` |
| I53 | import: sha computed flag | `test_import_one_file` |
| I54 | import: name includes the extension | `test_import_one_file`, `test_import_extension_is_case_insensitive_and_other_entries_are_ignored` |
| I55 | import: name not copied | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I56 | import: generation 2 | `test_import_one_file` |
| I57 | import: meta error ignored | `test_import_write_failures_clean_up_and_keep_the_file` |
| I58 | import: no cleanup after a meta error | `test_import_write_failures_clean_up_and_keep_the_file` |
| I59 | import: renamed to p001 | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I60 | import: file not moved | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I61 | import: rename error ignored | `test_import_write_failures_clean_up_and_keep_the_file` |
| I62 | import: no cleanup after a rename error | `test_import_write_failures_clean_up_and_keep_the_file` |
| I63 | import: entry not added | `test_iso_import:crash/rc=1` |
| I64 | import: entry state incomplete | `test_import_one_file` |
| I65 | import: entry received 0 | `test_import_one_file` |
| I66 | import: entry meta not stored | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored`, `test_import_name_rules` |
| I67 | import: imported flag not set | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| I68 | import: entry meta_valid not set | `test_import_one_file` |
| R01 | rescan: result not cleared (equivalent: every field is assigned or zero-initialised later) | `test_iso_import:crash/rc=1` |
| R02 | rescan: missing dir is an error | `test_rescan_without_import_directory_does_nothing` |
| R03 | rescan: dir_open error ignored | `test_rescan_io_errors_are_propagated` |
| R04 | rescan: dir_next error ignored | `test_rescan_io_errors_are_propagated` |
| R05 | rescan: directories counted | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored` |
| R06 | rescan: extension not checked | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored`, `test_rescan_cursor_counts_only_iso_files` |
| R07 | rescan: cursor skip not counted | `test_import_name_rules`, `test_import_signature` |
| R08 | rescan: cursor skips one more | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| R09 | rescan: cursor ignored | `test_import_name_rules`, `test_import_signature` |
| R10 | rescan: batch limit +1 (overflows the array) | `test_iso_import:crash/rc=1` |
| R11 | rescan: batch limit one less | `test_import_extension_is_case_insensitive_and_other_entries_are_ignored`, `test_import_signature` |
| R12 | rescan: more flag not set | `test_import_name_rules`, `test_import_signature` |
| R13 | rescan: loop does not stop at the batch | `test_iso_import:crash/rc=1` |
| R14 | rescan: entry name not copied | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| R15 | rescan: entry stat not copied | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| R16 | rescan: entry not counted | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| R17 | rescan: dir not closed | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| R18 | rescan: import error ignored | `test_rescan_io_errors_are_propagated`, `test_rescan_read_errors_while_checking_the_signature` |
| R19 | rescan: imported not counted | `test_import_one_file`, `test_import_never_uses_the_reserved_id` |
| R20 | rescan: skipped not counted | `test_import_name_rules`, `test_import_size_boundaries` |
| R21 | rescan: next cursor without skipped | `test_import_name_rules`, `test_import_signature` |
| R22 | rescan: next cursor counts imported | `test_import_name_rules`, `test_import_signature` |
| R23 | rescan: has_next not set | `test_import_name_rules`, `test_import_signature` |
| R24 | rescan: has_next always set | `test_import_one_file`, `test_import_extension_is_case_insensitive_and_other_entries_are_ignored` |
| Q01 | crosslink: p000 directory accepted | `test_delete_precheck_non_matches` |
| Q02 | crosslink: cluster 0 compared | `test_delete_precheck_non_matches` |
| Q03 | crosslink: missing p000 is an error | `test_scan_limits_to_16_images_and_counts_hidden_in_directory_order` |
| Q04 | crosslink: p000 stat error ignored | `test_delete_precheck_propagates_io_errors`, `test_delete_step_reports_io_errors_of_the_crosslink_check_without_changing_anything` |
| Q05 | crosslink: missing import dir is an error | `test_delete_precheck_non_matches` |
| Q06 | crosslink: import dir_open error ignored | `test_delete_precheck_propagates_io_errors`, `test_delete_step_reports_io_errors_of_the_crosslink_check_without_changing_anything` |
| Q07 | crosslink: dir_next error ignored | `test_delete_precheck_stops_at_the_first_match_and_does_not_read_further`, `test_delete_precheck_propagates_io_errors` |
| Q08 | crosslink: import directories compared | `test_delete_precheck_non_matches` |
| Q09 | crosslink: cluster compare inverted | `test_delete_precheck_refuses_when_import_has_the_same_first_cluster`, `test_delete_precheck_non_matches` |
| Q10 | crosslink: match not reported | `test_delete_precheck_refuses_when_import_has_the_same_first_cluster`, `test_delete_precheck_non_matches` |
| Q11 | crosslink: match does not stop (the next dir_next error turns crosslinked into an I/O error) | `test_delete_precheck_stops_at_the_first_match_and_does_not_read_further` |
| Q12 | crosslink: import dir not closed after the loop | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| Q13 | crosslink: import dir not closed on a dir_next error | `test_delete_precheck_stops_at_the_first_match_and_does_not_read_further`, `test_delete_precheck_propagates_io_errors` |
| Q14 | crosslink: p000 path is p001 | `test_delete_precheck_refuses_when_import_has_the_same_first_cluster`, `test_delete_precheck_non_matches` |
| Q15 | crosslink: the import directory is /iso | `test_delete_precheck_refuses_when_import_has_the_same_first_cluster`, `test_delete_precheck_non_matches` |
| Q20 | precheck: crosslink reported as OK | `test_delete_precheck_refuses_when_import_has_the_same_first_cluster`, `test_delete_precheck_non_matches` |
| Q21 | precheck: always crosslinked | `test_delete_precheck_refuses_when_import_has_the_same_first_cluster`, `test_delete_precheck_non_matches` |
| Q22 | precheck: error ignored | `test_delete_precheck_stops_at_the_first_match_and_does_not_read_further`, `test_delete_precheck_propagates_io_errors` |
| Q23 | precheck: bad id accepted | `test_delete_precheck_non_matches` |
| Q24 | precheck: NULL catalog accepted | `test_iso_delete:crash/rc=1` |
| Q30 | delete init: crosslink not checked | `test_delete_precheck_stops_at_the_first_match_and_does_not_read_further`, `test_delete_step_refuses_a_crosslinked_image_without_changing_anything` |
| Q31 | delete init: crosslink match ignored | `test_delete_precheck_stops_at_the_first_match_and_does_not_read_further`, `test_delete_step_refuses_a_crosslinked_image_without_changing_anything` |
| Q32 | delete init: crosslink check error ignored | `test_delete_step_reports_io_errors_of_the_crosslink_check_without_changing_anything` |
| Q33 | delete init: crosslink reported with another code | `test_delete_precheck_stops_at_the_first_match_and_does_not_read_further`, `test_delete_step_refuses_a_crosslinked_image_without_changing_anything` |
| Q35 | delete init: crosslink only checked when the meta is not deleting yet (the resume is not protected) | `test_delete_resume_with_crosslink_keeps_p000_and_everything_else` |
| Q36 | delete files: directories starting with p are removed | `test_delete_step_ignores_p_directories_and_stops_at_rmdir` |
| Q37 | delete files: only directories starting with p are removed | `test_delete_full_sequence_and_order`, `test_delete_when_slot_a_is_the_older_one` |
| Q40 | apply: invalid reason lost when staying invalid | `test_apply_reboot_keeps_the_invalid_reason` |

## 3. 設計に書かれていないことを、実装で決めた箇所(レビューで確認してほしい)

テストで固定してあるが、`ISO_MOUNT_DESIGN.md` に明示がないので、設計の文書への反映(または変更)を提案する。

1. **復号の範囲**: `size_bytes` は 1〜16 GiB(`ISO_MAX_PARTS` × 1 GiB)、`part_blocks` は 1〜2097152(= 4 GiB。FAT32 の 1 ファイルの上限)、`invalid` の理由は 0〜4(5 = `meta_corrupt` は保存しない)。予約領域と、名前の長さより後ろの枠は見ない(CRC が守る)。**名前は復号のときにも名前の規則で検査する**(CRC が合っていても、規則に反する名前は `meta_corrupt`)。
2. **`PUT` の再開は `meta` を書かない**。`incomplete` → `uploading` は実行時の状態だけで、`meta` は `incomplete` のまま(電源断で、同じ `incomplete` に戻る)。`IDLE_CLOSE`(`uploading` → `incomplete`)、`FINALIZE_*`、`INDEX_FAIL` が `meta` を書く。`DELETE` の `meta`(`deleting`)は、削除のステップの最初が書く(API の `DELETE` のハンドラーは書かない)。
3. **`received`** は、`ready` では `size_bytes`、`uploading`(→ `incomplete`)/ `incomplete` / `invalid` では「同期済みのパーツのファイルの大きさの合計」(先頭から順、満たない / 無いパーツで止める。期待する大きさを超える分は数えない)。`deleting` と `meta_corrupt` は 0。
4. **同名の判定**は、`deleting` のイメージと `meta_corrupt` のイメージを見ない(API から見えないため)。**上限(`ISO_MAX_IMAGES`)は、`deleting` のイメージも数える**(ディレクトリが残っている間は、枠を使う)。`begin` の検査の順は、名前 / 大きさ(400)→ 同名 → 上限。
5. **`import` の電源断**: `mkdir` → `meta`(`ready`)→ `rename` の順なので、`meta` と `rename` の間で落ちると、`p000` のない `ready` のイメージが残る。ファイルは `import/` に残るが、**同名のイメージが残っているので、次の `rescan` は `skipped` にする**(`INDEX` が `part_mismatch` にしたあと、利用者が削除すれば、取り込める)。`rename` を先にすると、ファイルが `<id>/p000` に移ったあとで `meta` がなく、`meta_corrupt` になる(データは残るが、`import/` からは見えない)ので、`meta` を先にした。
6. **`import` の署名の検査**は、`iso_fs_t.read_at(ctx, f, offset, buf, len, &got)`(C-10b で `iso_seams.h` に足した。位置指定の読み出し)で、**セクタ 16 の先頭から 6 バイトを 1 回で読む**(`test_import_signature_is_one_read_at_at_sector_16` が、`read_at` が 1 回、オフセット 32768、順次読みなし、を固定する)。C-10a の初版は、`read_at` がなく、先頭から 512 バイトずつ 65 回の順次読みだった。
7. **`close`** は、読み取り用に開いたハンドルにも使う(`iso_seams.h` のコメントは「失敗した `write` の後でも呼べる」とだけ書いている。読み取り用のハンドルも閉じられる、と明記してほしい)。
8. **削除のステップ**は `p` で始まるエントリだけを消す(設計の `p*`)。ほかのファイルが残っていると、`rmdir` が失敗し、`ERROR`(`ISO_ERR_INVALID` など、`remove` が返したエラー)になる。最後のスロットを消したあと `rmdir` の前で電源が落ちると、スロットのないディレクトリ(`meta_corrupt`)が残る。削除できる(上の網羅のテストで確認)。
9. **`iso_extent` の表の構築**は、`get_extents` の結果を、パーツごとの配列(`iso_part_input_t`)で、1 回の呼び出しで渡す。パーツの順に検査するので、先のパーツの `too_fragmented` が、後のパーツの `part_mismatch` より先に出る。
10. **削除とクロスリンク(設計 3.3。PR #150 の major の指摘)**: `iso_catalog_delete_precheck(cat, id)` が、`<id>/p000` の先頭クラスタが `/iso/import/` のファイル(ディレクトリを除く)と同じなら `ISO_CAT_ERR_CROSSLINKED`(API は 409 `crosslinked`)を返す。ハンドラーは、`iso_catalog_apply(ISO_EV_DELETE)` の前に呼ぶ。削除のステップの最初((1) の前)でも同じ照合をし、一致したら、何も変更せず(meta も、p000 も、他のパーツも)`ERROR(ISO_CAT_ERR_CROSSLINKED)` を返す。**再起動後の続き(meta がすでに deleting)でも同じ**。**設計が決めていなかった再開の扱い**: 一致した deleting は、**何も消さず**(p000 だけでなく、他のパーツと meta も残す)、`ERROR(crosslinked)` のまま、PC の chkdsk で直すまで残す(再試行は同じ結果になる)。警告のログは、純 C にログの手段がないので、`ERROR` を受けたグルー(ワーカー)が出す。
11. `p` で始まる**ディレクトリ**は、削除の対象にしない(`p000` などのファイルだけ)。`rmdir` が、既存のエラー経路で失敗する。
12. `iso_catalog_apply` は、INVALID のままのイベント(`REBOOT`)で、`invalid_reason`(`meta_corrupt` を含む)を保つ。INVALID に入るイベント(`FINALIZE_FAIL` / `INDEX_FAIL`)は引数の理由、INVALID を出る(`DELETE`)ときは 0。

## 4. PR #150 のレビュー(CodeRabbit)への対応

全体(578 個)を最初から実行し直した(554 個を殺し、24 個が同値)。足したミュータント Q01〜Q42(30 個)は、クロスリンクの照合と削除のステップ、`apply` の理由、`p` ディレクトリのもの。

| 指摘 | 対応 | 足したテスト |
|---|---|---|
| [major] `DELETE` と削除の再開で、`import/` の同じ先頭クラスタのファイルを照合する | 直した(上の 10.) | `test_delete_precheck_*`(一致、非一致、先頭クラスタ 0、ディレクトリ、`p000` なし、`import/` なし、I/O エラー)、`test_delete_step_refuses_a_crosslinked_image_without_changing_anything`、`test_delete_resume_with_crosslink_keeps_p000_and_everything_else`、`test_delete_step_reports_io_errors_of_the_crosslink_check_without_changing_anything`、`test_power_cut_in_rename_leaves_both_names_and_recovery_keeps_the_shared_cluster`(モックに rename の途中の電源断 `MOCK_TEAR_BOTH`: 2 つの名前が同じクラスタを指す状態から、再取り込みはスキップ、削除は拒否) |
| `p*` に一致するディレクトリを削除しない | 直した(11.) | `test_delete_step_ignores_p_directories_and_stops_at_rmdir`(Q36、Q37) |
| `REBOOT` で `invalid_reason` が消える | 直した(12.) | `test_apply_reboot_keeps_the_invalid_reason`(Q40) |
| C23 を同値としたのは誤り | その通り(期待する大きさ 0 のパーツの `stat` が I/O エラーなら、戻り値が変わる)。同値のリストから外し、テストを足した | `test_received_does_not_look_at_parts_beyond_the_expected_count` |
| L16 を同値としたのは誤り | その通り(`stat` が成功し `open` だけが失敗するとき、128 バイトの `meta.a` を開くと、有効な `meta.b` を採用できない)。同値のリストから外し、テストを足した | `test_slots_read_short_slot_is_not_opened` |
| 電源断のテスト: 復元状態を位置ごとに限定する | 直した(位置ごとの期待: 8〜10 は deleting、11 の完了は `meta_corrupt`、12 は `meta_corrupt` / ディレクトリなし。削除の途中の 1 回目の電源断の直後も、位置ごとに限定) | `test_power_cut_at_every_write_and_every_tear`、`test_double_power_cut_during_delete` |
| 二重の電源断: 2 回目の位置に到達したものだけを数える | 直した(到達 198、到達しない 108、1 回目で消えたもの 18 に分けて数える) | `test_double_power_cut_during_delete` |
| 二重の電源断: 旧 ready を許す条件 | 直した(2 回目の `deleting` の書き込みが完了していない `m = 1`、NONE / HALF のときだけ) | 同上 |
| ファズ: 電源断を予約時ではなく発生時に数える | 直した(`FS.dead` のとき) | `test_fuzz_random_operations_with_power_cuts` |
| ファズ: スキャン後の実行時状態も、別に照合する | 直した(手書きの対応表: uploading → incomplete、他はそのまま。`resume_delete` も) | 同上 |
| ファズ: `rnd()` 2 回の評価順 | 直した(別々の文) | - |
| モック: `rename` の両側に名前が残る電源断 | 足した(`MOCK_TEAR_BOTH`。既存の HALF / FULL は変えない) | 上の `test_power_cut_in_rename_...` |
| `test_transition_table`: 列数の検査 | 直した(走査の前に、`EV_NAME`、`TABLE`、状態の数を `ISO_EV_COUNT` / 7 と比較) | `test_transition_table_every_cell` |
| `test_iso_import`: 結果の初期化のテストに有効な初期値 | 直した(`has_next = true` と非 0 の値の指定初期化) | `test_import_result_is_cleared_on_each_call` |
| `mutate_iso_catalog.py`: 出力先の保護、未知の ID | 直した(共通ランナー `mutate_iso_extent.py` に入れた) | - |
| `test/host/CMakeLists.txt` の絶対パスの部分一致、CMake のサニタイザー、C11 | 直した(`ISO_EXTENT_TESTS.md` の 3. と同じ) | - |
