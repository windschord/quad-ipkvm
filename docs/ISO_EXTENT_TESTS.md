# iso_extent のテストとミューテーションテストの結果

作成: 2026-11-19(Issue #139、C-10 の前半)。`components/iso_extent`(CD のブロック → SD のセクタの区間、エクステントの表と参照カウント。純 C)のホスト単体テストの構成と、`iso_extent.c` に対するミューテーションテストの結果。契約は `docs/ISO_MOUNT_DESIGN.md` §3.1、§4.1、§9。

## 1. テストの構成

| ファイル | 内容 | 件数(Unity のテストケース) |
|---|---|---|
| `test/host/iso_extent/test_iso_extent_build.c` | 純粋な計算(`capacity_blocks`、`file_sectors`、`part_count`、`part_bytes`)、表の構築と検証(引数、大きさ・パーツ数の上限、`part_mismatch`、`too_fragmented`、クリップ、メモリ不足)、参照カウント | 26 |
| `test/host/iso_extent/test_iso_extent_map.c` | 変換 `iso_extent_map`(パーツの境界、エクステントの境界、最終ブロック、大きさが 2048 / 512 の倍数でない場合、64 ビットのセクタ、併合、範囲外、`SPANS_FULL`)、`iso_extent_valid_bytes` | 15 |
| `test/host/iso_extent/test_iso_extent_fuzz.c` | 固定シードのファズ(下記) | 2 |
| `test/host/iso_extent/ie_common.h` | 共通の部品(数え上げ付きのアロケータ、基準の表 A) | - |

合計 43 件(`test/host/CMakeLists.txt` の既存の自動発見で、ctest の実行ファイルは 3 つ増える)。

**期待値はすべて手書き**。基準の表 A(`ie_common.h`。`part_blocks` = 4 = 16 セクタ / パーツ、3 パーツ、`size_bytes` = 22628)は、論理セクタごとの SD 上の位置を、コメントに手計算で書いてある。

- 表 A: パーツ 0 = `[1000,10] [2000,6]`、パーツ 1 = `[2006,16]`(パーツ 0 の末尾の直後 = SD 上で連続)、パーツ 2 = `[5000,8] [6000,8] [9000,4]`(13 セクタにクリップされ `[5000,8] [6000,5]`)。`capacity_blocks` = 12(11.05 を切り上げ)、ファイルのセクタ数 = 45、最後のブロック(11)の有効バイトは 100。
- 境界値: LBA = capacity、LBA + blocks = capacity + 1、最終ブロック(有効バイトが 2048 未満)、パーツの境界で始まる / 終わる範囲、エクステントの境界で始まる / 終わる範囲、パーツをまたぐ併合(SD 上で連続なとき)と併合しないとき(1 セクタ空く、逆向きに接する)、64 ビットのセクタ番号(2^32 を超える開始)、`size_bytes` = 1 / 1536 / 2049 / 34816、4 GiB のパーツ(`part_blocks` × 2048 が 32 ビットで 0 に戻る)、パーツ 16 個(16 GiB)。

**ファズ**(`test_iso_extent_fuzz.c`。固定シード、ASan / UBSan): (1) ランダムな表(パーツの大きさ、エクステントの切り方: 隣接、逆向き、離れる、32 ビット超、長さ 0、余分な末尾)とランダムな範囲を作り、実装とは独立な基準(**論理セクタを 1 つずつ SD のセクタに展開し、連続するものを併合する**)と、返される区間、エラーの種類、有効バイト、区間の合計 = ceil(有効バイト / 512)、`SPANS_FULL` の境界(区間の数ちょうど = OK、1 つ少ない = `SPANS_FULL`)を全部比べる。実測(毎回同じ): 60000 回の変換、うち受理 32318 回、複数の区間に分かれたもの 12472 回、併合が起きたもの 22287 回。(2) 壊れた入力(エクステントの数・大きさ・パーツの数がランダム)を `build` に与え、クラッシュせず、失敗のときは何も確保しない(20000 回、受理 1126 回、拒否 18874 回)。

## 2. ミューテーションテスト

方法: `tools/test/mutate_iso_extent.py` が、`components/` と `test/` をスクラッチにコピーし、`iso_extent.c` の 1 か所を置換 → 再ビルド → `test_iso_extent_build` / `_map` / `_fuzz` を実行、を繰り返す(ミュータントの定義は `tools/test/iso_extent_mutants.py`)。1 つでも失敗(アサーション、ASan/UBSan、クラッシュ、タイムアウト)すれば「殺した」。最初の失敗で、そのミュータントの実行を止める。

```
python3 tools/test/mutate_iso_extent.py <repo> <スクラッチ> tools/test/iso_extent_mutants.py <結果.json> -j4
```

**結果**(ミュータントを入れる前のベースラインは、全テスト成功):

| 実行した数 | 殺した数 | 生き残った数(すべて同値と判断) | ビルドエラー |
|---|---|---|---|
| 180 | 162 | 18 | 0 |

経緯: 最初の実行では、殺した 151、生き残り 25、ビルドエラー 4(`-Werror` の未使用変数。ミュータントの書き方を直した)だった。生き残りのうち、同値でないものを調べ、次のテストを足した(足したあとに**全体を最初から実行し直した**のが上の結果)。

| 最初の実行で生き残ったミュータント | 足したテスト |
|---|---|
| E019(`part_bytes` の `pb` が 32 ビットの積) | `test_part_count_and_bytes` に 4 GiB のパーツ(`part_blocks` = 2097152)の例を追加 |
| E025、E030(最後のエクステントを、ファイルの末尾でクリップしない / 区間の数をクリップしない) | `test_map_clips_an_extent_that_runs_past_the_part_end`(パーツ 0 の最後のエクステントがファイルより長いとき、次のパーツの区間を飲み込まない) |
| E048(イメージの大きさの上限が効かない) | `test_build_image_size_limit_is_checked_by_itself`(4 GiB のパーツなら、16 GiB + 1 でもパーツ数は 5 で、大きさの上限だけが拒否する) |
| E055(パーツが 1 つ多いのに、それが空だと受理) | `test_build_part_count_mismatch` に、空の余分なパーツのケースを追加 |
| E061(エクステントが 1 個で、配列が NULL) | `test_build_null_extents_with_nonzero_count_is_invalid` に 1 個のケースを追加 |
| E206(`valid_bytes` の `len` が 32 ビットの積) | 16 GiB の表で `iso_extent_valid_bytes(t, 0, 8388608)` を確認 |

### 2.1 同値のミュータント(18 個)

| ID | 変更箇所 | 同値の理由 |
|---|---|---|
| E026 | clip: `n > need - taken` を `>=` | 等しいときの代入は同じ値 |
| E070 | build: ヘッダーのアラインを外す | `sizeof(struct iso_extent_table)` はすでに 8 の倍数なので、切り上げても同じ |
| E071 | build: パーツの領域を切り上げない | `sizeof(part_t)` = 8 の倍数なので、パーツ数に依らず同じ |
| E092 | build: `extent_count` を記録しない | エクステントは、パーツの順に連続して配列に置く。変換は `first_extent` から順に読み、パーツの終わりと次のパーツの先頭が同じ位置なので、`extent_count` が 0 でも、有効な表では読み出しの結果が変わらない(`extent_count` は、境界の検査用の防御) |
| E109 | unref: `refs == 0` の検査を外す | 参照が 0 になった時点で解放されるので、`refs == 0` の表は呼ばれない |
| E141 | map: `blocks == 0` の早期の戻りを外す | `remaining` が 0 になり、ループに入らず、区間 0 個で OK になる(同じ結果) |
| E145 | map: `want` を 32 ビットの積にする | `blocks` は範囲検査の後で、最大 8388608(16 GiB / 2048)なので、× 4 でも 32 ビットに収まる |
| E150 | map: `part_sectors` を 32 ビットの積にする | `part_blocks` は 2097152 以下なので、× 4 でも 32 ビットに収まる |
| E154 | map: パーツ番号の範囲検査を外す | 範囲検査で `first` はファイルのセクタ数未満なので、パーツ番号は常にパーツ数未満 |
| E158 | map: エクステント探索の `ei < end` を `ei <= end` | `local` は常にパーツの範囲内なので、`ei == end` の手前で必ず見つかる |
| E159 | map: 見つからないときの検査を外す | 同上(到達しない) |
| E163 | map: `take > remaining` を `>=` | 等しいときの代入は同じ値 |
| E174 | map: `n == max_spans` を `>=` | `n` は 1 ずつ増えるので、`max_spans` を超えない(== と同じ) |
| E176 | map: `SPANS_FULL` のとき `*n_spans = 0` を外す | 関数の先頭で 0 にしていて、途中で書き換えない |
| E187 | map: 最後のパーツの検査 `p >= n_parts` を `>` | 区間の合計がファイルのセクタ数に等しいので、最後のパーツの先に進むことがない |
| E190 | map: 空のパーツの検査を外す | 検査済みの表のパーツは、1 つ以上のエクステントを持つ |
| E202 | valid_bytes: 範囲検査を `>` | `lba == capacity` のとき、`start >= size_bytes` が 0 を返す |
| E204 | valid_bytes: `start >= size_bytes` を外す | 範囲検査を通った `lba` では、`start < size_bytes` |

### 2.2 殺したミュータントの一覧(162 個)

「殺したテスト」は、失敗したテストのうち先頭のもの(最初に落ちたテストの実行ファイルで止める)。ファズだけが殺したものは、`test_fuzz_*`。

| ID | 変更箇所 | 殺したテスト |
|---|---|---|
| E001 | capacity_blocks: divide by 512 | `test_capacity_blocks_rounds_up`, `test_build_table_a_attributes` |
| E002 | capacity_blocks: no round up | `test_capacity_blocks_rounds_up`, `test_build_table_a_attributes` |
| E003 | capacity_blocks: round up by 2 | `test_capacity_blocks_rounds_up`, `test_build_table_a_attributes` |
| E004 | capacity_blocks: always round up | `test_capacity_blocks_rounds_up`, `test_build_max_image_16_parts` |
| E005 | file_sectors: divide by 2048 | `test_file_sectors_rounds_up`, `test_build_table_a_attributes` |
| E006 | file_sectors: no round up | `test_file_sectors_rounds_up`, `test_build_extents_must_cover_the_file` |
| E007 | file_sectors: always round up | `test_file_sectors_rounds_up`, `test_build_table_a_attributes` |
| E008 | part_count: no round up | `test_part_count_and_bytes`, `test_build_table_a_attributes` |
| E009 | part_count: always round up | `test_part_count_and_bytes`, `test_build_max_image_16_parts` |
| E010 | part_count: part_blocks == 0 not rejected | `test_iso_extent_build:crash/rc=1` |
| E011 | part_count: part_blocks as 32-bit product | `test_iso_extent_build:crash/rc=1` |
| E012 | part_count: part is 512 B blocks | `test_part_count_and_bytes`, `test_build_table_a_attributes` |
| E013 | part_bytes: index >= n -> index > n | `test_part_count_and_bytes` |
| E014 | part_bytes: index check removed | `test_part_count_and_bytes` |
| E015 | part_bytes: last part detection index+1 <= n | `test_part_count_and_bytes`, `test_build_table_a_attributes` |
| E016 | part_bytes: non-last parts return remainder | `test_part_count_and_bytes`, `test_build_table_a_attributes` |
| E017 | part_bytes: remainder off (n instead of n-1) | `test_part_count_and_bytes`, `test_build_table_a_attributes` |
| E018 | part_bytes: remainder off by one | `test_part_count_and_bytes`, `test_build_table_a_attributes` |
| E019 | part_bytes: pb as 32-bit product | `test_part_count_and_bytes`, `test_build_image_size_limit_is_checked_by_itself` |
| E020 | clip: zero-length extents are kept | `test_build_clips_tail_and_drops_zero_length`, `test_build_too_fragmented_boundary_single_part` |
| E021 | clip: zero-length check removed | `test_build_clips_tail_and_drops_zero_length` |
| E022 | clip: overflow check >= | `test_build_extent_end_overflow_is_invalid` |
| E023 | clip: overflow check removed | `test_build_extent_end_overflow_is_invalid` |
| E024 | clip: overflow check uses wrapping sum | `test_build_invalid_args`, `test_build_size_and_part_blocks_limits` |
| E025 | clip: no clipping of the last extent | `test_iso_extent_map:crash/rc=1` |
| E027 | clip: loop goes on after the file is covered | `test_build_table_a_attributes`, `test_build_clips_tail_and_drops_zero_length` |
| E028 | clip: loop stops one extent early | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E029 | clip: emit start not copied | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E030 | clip: emit count not clipped | `test_iso_extent_map:crash/rc=1` |
| E031 | clip: kept not counted | `test_iso_extent_build:crash/rc=1` |
| E032 | clip: taken not advanced | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E033 | clip: short coverage accepted | `test_build_extents_must_cover_the_file` |
| E034 | clip: short coverage by one accepted | `test_build_extents_must_cover_the_file` |
| E035 | clip: exact coverage rejected | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E036 | clip: kept not reported | `test_iso_extent_build:crash/rc=1` |
| E040 | build: NULL out accepted | `test_iso_extent_build:crash/rc=1` |
| E041 | build: *out not cleared | `test_build_invalid_args`, `test_build_rejects_more_than_16_parts_but_accepts_16` |
| E042 | build: NULL alloc accepted | `test_iso_extent_build:crash/rc=1` |
| E043 | build: NULL alloc function accepted | `test_iso_extent_build:crash/rc=1` |
| E044 | build: NULL free function accepted | `test_build_invalid_args` |
| E045 | build: NULL parts accepted | `test_iso_extent_build:crash/rc=1` |
| E046 | build: size 0 accepted | `test_build_size_and_part_blocks_limits` |
| E047 | build: size limit >= | `test_build_max_image_16_parts`, `test_build_image_size_limit_is_checked_by_itself` |
| E048 | build: size limit removed | `test_build_image_size_limit_is_checked_by_itself` |
| E049 | build: part_blocks 0 accepted | `test_build_size_and_part_blocks_limits` |
| E050 | build: part_blocks limit >= | `test_build_size_and_part_blocks_limits`, `test_build_image_size_limit_is_checked_by_itself` |
| E051 | build: part_blocks limit removed | `test_build_size_and_part_blocks_limits` |
| E052 | build: more than 16 parts accepted | `test_build_rejects_more_than_16_parts_but_accepts_16` |
| E053 | build: 16 parts rejected | `test_build_max_image_16_parts`, `test_build_rejects_more_than_16_parts_but_accepts_16` |
| E054 | build: part count mismatch accepted | `test_build_part_count_mismatch` |
| E055 | build: too many parts accepted | `test_build_part_count_mismatch` |
| E056 | build: too few parts accepted | `test_build_part_count_mismatch` |
| E057 | build: part size mismatch accepted | `test_build_part_size_mismatch` |
| E058 | build: part larger than expected accepted | `test_build_part_size_mismatch` |
| E059 | build: part smaller than expected accepted | `test_build_part_size_mismatch` |
| E060 | build: NULL extents with count accepted | `test_iso_extent_build:crash/rc=1` |
| E061 | build: NULL extents check needs 2+ extents | `test_iso_extent_build:crash/rc=1` |
| E062 | build: clip result ignored | `test_build_extents_must_cover_the_file`, `test_build_extent_end_overflow_is_invalid` |
| E063 | build: clip error always part_mismatch | `test_build_extent_end_overflow_is_invalid` |
| E064 | build: too_fragmented check removed | `test_build_too_fragmented_boundary_single_part`, `test_build_too_fragmented_limit_is_for_the_whole_image` |
| E065 | build: too_fragmented check >= | `test_build_too_fragmented_boundary_single_part`, `test_build_too_fragmented_limit_is_for_the_whole_image` |
| E066 | build: too_fragmented limit is per part | `test_build_too_fragmented_limit_is_for_the_whole_image` |
| E067 | build: total not accumulated | `test_iso_extent_build:crash/rc=1` |
| E068 | build: file sectors of the wrong size passed to clip | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E072 | build: allocation too small by one extent | `test_iso_extent_build:crash/rc=1` |
| E073 | build: allocation counts extents as bytes | `test_iso_extent_build:crash/rc=1` |
| E074 | build: parts area too small | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E075 | build: NULL from alloc not checked | `test_iso_extent_build:crash/rc=1` |
| E076 | build: NOMEM reported as INVALID | `test_build_nomem` |
| E077 | build: allocator not stored | `test_iso_extent_build:crash/rc=1` |
| E078 | build: refcount starts at 2 | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E079 | build: refcount starts at 0 | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E080 | build: epoch not stored | `test_build_table_a_attributes` |
| E081 | build: epoch +1 | `test_build_table_a_attributes` |
| E082 | build: size not stored | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E083 | build: part_blocks not stored | `test_iso_extent_build:crash/rc=1` |
| E084 | build: n_parts not stored | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E085 | build: n_extents not stored | `test_build_table_a_attributes`, `test_build_max_image_16_parts` |
| E086 | build: n_extents stored without clipping | `test_build_table_a_attributes`, `test_build_max_image_16_parts` |
| E087 | build: parts pointer wrong | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E088 | build: extents pointer wrong | `test_iso_extent_build:crash/rc=1` |
| E089 | build: second pass clips by one sector more | `test_iso_extent_build:crash/rc=1` |
| E090 | build: extents written at wrong position | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E091 | build: first_extent not recorded | `test_iso_extent_build:crash/rc=1` |
| E093 | build: pos not advanced | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E094 | build: *out not set | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E100 | ref: increments by 2 | `test_iso_extent_build:crash/rc=1` |
| E101 | ref: does nothing | `test_iso_extent_build:crash/rc=1` |
| E102 | ref: NULL not checked | `test_iso_extent_build:crash/rc=1` |
| E103 | unref: does not decrement | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E104 | unref: frees one reference early | `test_refcount` |
| E105 | unref: never frees | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E106 | unref: frees a wrong pointer | `test_iso_extent_build:crash/rc=1` |
| E107 | unref: free with a NULL context | `test_iso_extent_build:crash/rc=1` |
| E108 | unref: NULL table not tolerated | `test_iso_extent_build:crash/rc=1` |
| E110 | unref: returns the count before decrement | `test_build_table_a_attributes`, `test_build_does_not_keep_pointers_to_the_input` |
| E111 | refs: NULL not tolerated | `test_iso_extent_build:crash/rc=1` |
| E120 | epoch: returns refs | `test_build_table_a_attributes` |
| E121 | size_bytes: returns part_blocks | `test_build_table_a_attributes` |
| E122 | capacity: returns size_bytes | `test_build_table_a_attributes`, `test_build_max_image_16_parts` |
| E123 | part_count: returns extent count | `test_build_table_a_attributes` |
| E124 | part_blocks: returns n_parts | `test_build_table_a_attributes` |
| E125 | extent_count: returns n_parts | `test_build_table_a_attributes`, `test_build_extents_must_cover_the_file` |
| E130 | map: NULL n_spans accepted | `test_iso_extent_map:crash/rc=1` |
| E131 | map: *n_spans not cleared on entry | `test_iso_extent_map:crash/rc=1` |
| E132 | map: NULL table accepted | `test_iso_extent_map:crash/rc=1` |
| E133 | map: NULL spans accepted | `test_iso_extent_map:crash/rc=1` |
| E134 | map: NULL spans with max_spans 0 rejected | `test_iso_extent_map:crash/rc=1` |
| E135 | map: lba == capacity accepted | `test_iso_extent_map:crash/rc=1` |
| E136 | map: lba range check removed | `test_iso_extent_map:crash/rc=1` |
| E137 | map: range end check >= | `test_build_clips_tail_and_drops_zero_length`, `test_build_clips_a_single_extent_longer_than_the_file` |
| E138 | map: range end check removed | `test_iso_extent_map:crash/rc=1` |
| E139 | map: range end computed with overflow | `test_iso_extent_map:crash/rc=1` |
| E140 | map: zero blocks is an error | `test_iso_extent_map:crash/rc=1` |
| E142 | map: first sector uses 512 | `test_iso_extent_map:crash/rc=1` |
| E143 | map: first sector uses 8 sectors per block | `test_iso_extent_map:crash/rc=1` |
| E144 | map: want uses 3 sectors per block | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E146 | map: avail adds instead of subtracts | `test_iso_extent_map:crash/rc=1` |
| E147 | map: avail uses capacity blocks | `test_build_clips_tail_and_drops_zero_length`, `test_build_clips_a_single_extent_longer_than_the_file` |
| E148 | map: remaining ignores the file end | `test_build_clips_tail_and_drops_zero_length`, `test_build_clips_a_single_extent_longer_than_the_file` |
| E149 | map: remaining takes the larger | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E151 | map: part sectors uses 8 per block | `test_iso_extent_map:crash/rc=1` |
| E152 | map: part index uses modulo | `test_iso_extent_map:crash/rc=1` |
| E153 | map: local offset uses division | `test_iso_extent_map:crash/rc=1` |
| E155 | map: extent search skips on equality | `test_iso_extent_map:crash/rc=1` |
| E156 | map: extent search stops at the first extent | `test_iso_extent_map:crash/rc=1` |
| E157 | map: extent search base not advanced | `test_iso_extent_map:crash/rc=1` |
| E160 | map: offset in extent wrong | `test_iso_extent_map:crash/rc=1` |
| E161 | map: take ignores the offset | `test_iso_extent_map:crash/rc=1` |
| E162 | map: take not limited by remaining | `test_build_does_not_keep_pointers_to_the_input` |
| E164 | map: start ignores the offset | `test_iso_extent_map:crash/rc=1` |
| E165 | map: start subtracts the offset | `test_iso_extent_map:crash/rc=1` |
| E166 | map: merge without n > 0 check | `test_iso_extent_build:crash/rc=1` |
| E167 | map: merge compares with != | `test_build_clips_tail_and_drops_zero_length` |
| E168 | map: merge compares start - count | `test_build_clips_tail_and_drops_zero_length` |
| E169 | map: merge compares with start - 1 (adjacent by one off) | `test_build_clips_tail_and_drops_zero_length` |
| E170 | map: merge replaces the count | `test_build_clips_tail_and_drops_zero_length` |
| E171 | map: merge never happens | `test_build_clips_tail_and_drops_zero_length` |
| E172 | map: merge always happens when n > 0 | `test_build_clips_tail_and_drops_zero_length` |
| E173 | map: spans-full check > | `test_iso_extent_map:crash/rc=1` |
| E175 | map: spans-full check removed | `test_iso_extent_map:crash/rc=1` |
| E177 | map: spans-full reports another error | `test_iso_extent_map:crash/rc=1` |
| E178 | map: new span start not stored | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E179 | map: new span count not stored | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E180 | map: new span not counted | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E181 | map: remaining not reduced | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E182 | map: offset not reset for the next extent | `test_iso_extent_map:crash/rc=1` |
| E183 | map: break when finished removed | `test_build_clips_tail_and_drops_zero_length`, `test_build_clips_a_single_extent_longer_than_the_file` |
| E184 | map: next extent not advanced | `test_build_clips_tail_and_drops_zero_length` |
| E185 | map: part end check > | `test_build_clips_tail_and_drops_zero_length` |
| E186 | map: next part not advanced | `test_build_clips_tail_and_drops_zero_length` |
| E188 | map: next part start wrong | `test_iso_extent_build:crash/rc=1` |
| E189 | map: next part end wrong | `test_build_clips_tail_and_drops_zero_length` |
| E191 | map: n_spans not stored | `test_build_does_not_keep_pointers_to_the_input`, `test_build_clips_tail_and_drops_zero_length` |
| E200 | valid_bytes: NULL not tolerated | `test_iso_extent_map:crash/rc=1` |
| E201 | valid_bytes: range check removed | `test_iso_extent_map:crash/rc=1` |
| E203 | valid_bytes: start uses 512 | `test_iso_extent_map:crash/rc=1` |
| E205 | valid_bytes: len uses 512 | `test_build_max_image_16_parts`, `test_build_image_size_limit_is_checked_by_itself` |
| E206 | valid_bytes: len as 32-bit product | `test_build_max_image_16_parts`, `test_build_image_size_limit_is_checked_by_itself` |
| E207 | valid_bytes: takes the larger | `test_iso_extent_map:crash/rc=1` |
| E208 | valid_bytes: remainder off by one | `test_iso_extent_map:crash/rc=1` |
| E209 | valid_bytes: always len | `test_iso_extent_map:crash/rc=1` |
