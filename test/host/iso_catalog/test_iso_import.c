/*
 * iso_catalog: PC から /iso/import/ にコピーした ISO の取り込み(設計 3.3)。期待値は手書き。
 * ISO_RESCAN_BATCH = 4。取り込みは rename で <id>/p000 へ。取り込めないものは skipped(ファイルは残す)。
 */
#include "ic_common.h"

void setUp(void) { fx_setup(); }
void tearDown(void) { fx_teardown(); }

static iso_rescan_result_t rescan(uint32_t cursor) {
    iso_rescan_result_t r;
    memset(&r, 0xEE, sizeof(r));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_rescan(&CAT, cursor, &r));
    return r;
}

/* 先頭の 5 バイトの署名つきの有効な ISO を置く。名前は "<base>.iso" */
static void put_iso(const char *file_name, uint64_t size) { put_import(file_name, size, "CD001", 0); }

void test_import_one_file(void) {
    put_iso("Ubuntu.iso", 40000);
    uint32_t cluster = mock_fs_first_cluster(&FS, "/iso/import/Ubuntu.iso");
    fx_settle();
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_EQUAL_UINT32(0, r.skipped);
    TEST_ASSERT_FALSE(r.has_next);
    /* 書き込みの順序: mkdir -> meta.a -> rename */
    TEST_ASSERT_EQUAL_INT(3, FS.nlog);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "mkdir /iso/11110001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "overwrite /iso/11110001/meta.a len=256"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "rename /iso/import/Ubuntu.iso -> /iso/11110001/p000"));
    /* ファイルは移った(コピーではない: 同じクラスタ) */
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/import/Ubuntu.iso"));
    TEST_ASSERT_EQUAL_INT64(40000, mock_fs_size(&FS, "/iso/11110001/p000"));
    TEST_ASSERT_EQUAL_UINT32(cluster, mock_fs_first_cluster(&FS, "/iso/11110001/p000"));
    /* meta */
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, "11110001", &s));
    TEST_ASSERT_EQUAL_INT8(0, s.adopted);
    const iso_meta_t *m = &s.meta[0];
    TEST_ASSERT_EQUAL_UINT8(ISO_META_READY, m->state);
    TEST_ASSERT_EQUAL_UINT8(0, m->invalid_reason);
    TEST_ASSERT_EQUAL_UINT64(40000, m->size_bytes);
    TEST_ASSERT_EQUAL_UINT16(1, m->part_count);
    TEST_ASSERT_EQUAL_UINT32(20, m->part_blocks); /* ceil(40000 / 2048) = 19.53 -> 20 */
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_NONE, m->sha_flag); /* SHA-256 は計算しない */
    TEST_ASSERT_EQUAL_UINT32(1, m->generation);
    TEST_ASSERT_EQUAL_STRING("Ubuntu", m->name);
    /* カタログ */
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    TEST_ASSERT_EQUAL_STRING("11110001", CAT.images[0].id);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, CAT.images[0].state);
    TEST_ASSERT_EQUAL_UINT64(40000, CAT.images[0].received);
    TEST_ASSERT_EQUAL_INT8(0, CAT.images[0].adopted);
    TEST_ASSERT_TRUE(CAT.images[0].meta_valid);
    /* 再スキャンしても同じ */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, CAT.images[0].state);
}

void test_import_extension_is_case_insensitive_and_other_entries_are_ignored(void) {
    put_iso("a.iso", 40000);
    put_iso("b.ISO", 40000);
    put_iso("c.Iso", 40000);
    put_iso("d.iSo", 40000);
    fx_default_rnd();
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/readme.txt", "x", 1, 0));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/e.iso.txt", "x", 1, 0));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/f.is", "x", 1, 0));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/giso", "x", 1, 0));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/h.iso ", "x", 1, 0)); /* 末尾に空白: *.iso ではない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/import/dir.iso"));
    /* 4 バイトより短い名前(拡張子の検査が、名前の手前を読まない) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/a", "x", 1, 0));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/ab", "x", 1, 0));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/abc", "x", 1, 0));
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(4, r.imported);
    TEST_ASSERT_EQUAL_UINT32(0, r.skipped); /* *.iso でないもの、ディレクトリは数えない */
    TEST_ASSERT_FALSE(r.has_next);
    TEST_ASSERT_EQUAL_STRING("a", iso_catalog_find(&CAT, "11110001")->meta.name);
    TEST_ASSERT_EQUAL_STRING("b", iso_catalog_find(&CAT, "11110002")->meta.name);
    TEST_ASSERT_EQUAL_STRING("c", iso_catalog_find(&CAT, "11110003")->meta.name);
    TEST_ASSERT_EQUAL_STRING("d", iso_catalog_find(&CAT, "11110004")->meta.name);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/readme.txt"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/dir.iso"));
}

void test_import_name_rules(void) {
    char big64[80], big63[80];
    memset(big64, 'a', 64);
    strcpy(big64 + 64, ".iso");
    memset(big63, 'b', 63);
    strcpy(big63 + 63, ".iso");
    put_iso(big64, 40000); /* 名前 64 バイト: skipped */
    put_iso(big63, 40000); /* 名前 63 バイト: OK */
    put_iso(" lead.iso", 40000);   /* 先頭の空白 */
    put_iso("trail .iso", 40000);  /* 名前の末尾が空白 */
    put_iso(".iso", 40000);        /* 名前が空 */
    put_iso("ctl\x01x.iso", 40000);
    put_iso("bad\xC0\xAF.iso", 40000);
    put_iso("\xE3\x82\xA4\xE3\x83\xA1.iso", 40000); /* "イメ" (UTF-8): OK */
    put_iso("My Linux.iso", 40000);                  /* 空白を含む: OK */
    char huge[270];
    memset(huge, 'h', 251);
    strcpy(huge + 251, ".iso"); /* 255 バイトのファイル名 */
    put_iso(huge, 40000);
    /* 全部を 1 回に調べるため、cursor を進めて繰り返す */
    uint32_t imported = 0, skipped = 0, cursor = 0;
    int guard = 0;
    for (;;) {
        iso_rescan_result_t r = rescan(cursor);
        imported += r.imported;
        skipped += r.skipped;
        if (!r.has_next) break;
        cursor = r.next_cursor;
        TEST_ASSERT_TRUE(++guard < 20);
    }
    TEST_ASSERT_EQUAL_UINT32(3, imported); /* 63 バイト、イメ、My Linux */
    TEST_ASSERT_EQUAL_UINT32(7, skipped);  /* 64 バイト、先頭の空白、末尾の空白、名前が空、制御文字、不正な UTF-8、255 バイトのファイル名 */
    TEST_ASSERT_NOT_NULL(mock_fs_data(&FS, "/iso/import/trail .iso", NULL));
    /* 取り込まれた名前 */
    bool seen63 = false, seen_ja = false, seen_space = false;
    for (size_t i = 0; i < CAT.count; i++) {
        const char *n = CAT.images[i].meta.name;
        if (strlen(n) == 63 && n[0] == 'b') seen63 = true;
        if (strcmp(n, "\xE3\x82\xA4\xE3\x83\xA1") == 0) seen_ja = true;
        if (strcmp(n, "My Linux") == 0) seen_space = true;
    }
    TEST_ASSERT_TRUE(seen63);
    TEST_ASSERT_TRUE(seen_ja);
    TEST_ASSERT_TRUE(seen_space);
}

void test_import_size_boundaries(void) {
    put_iso("small.iso", 34815);  /* 2048 x 17 - 1: skipped */
    put_iso("exact.iso", 34816);  /* ちょうど: OK */
    put_iso("empty.iso", 0);      /* 空 */
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_EQUAL_UINT32(2, r.skipped);
    TEST_ASSERT_FALSE(r.has_next);
    TEST_ASSERT_EQUAL_STRING("exact", CAT.images[0].meta.name);
    TEST_ASSERT_EQUAL_UINT32(17, CAT.images[0].meta.part_blocks);
    TEST_ASSERT_EQUAL_UINT64(34816, CAT.images[0].meta.size_bytes);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/small.iso")); /* 残す */
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/empty.iso"));
}

void test_import_part_size_limit(void) {
    /* part_blocks の上限 2097152 (4 GiB)。それより大きい(FAT32 では置けない)ものは skipped */
    put_iso("big.iso", 4294967296ull);
    put_iso("bigger.iso", 4294967297ull);
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_EQUAL_UINT32(1, r.skipped);
    TEST_ASSERT_EQUAL_UINT32(2097152, CAT.images[0].meta.part_blocks);
    TEST_ASSERT_EQUAL_UINT64(4294967296ull, CAT.images[0].meta.size_bytes);
}

void test_import_signature(void) {
    put_import("ok1.iso", 40000, "CD001", 0);
    put_import("ok2.iso", 40000, "BEA01", 0);
    put_import("bad1.iso", 40000, "CD002", 0);
    put_import("bad2.iso", 40000, "cd001", 0);
    put_import("bad3.iso", 40000, "CD00", 0);   /* 5 バイト目が 0 */
    put_import("bad4.iso", 40000, NULL, 0);     /* 署名なし */
    put_import("bad5.iso", 40000, "BEA02", 0);
    iso_rescan_result_t r1 = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(2, r1.imported); /* ok1, ok2 */
    TEST_ASSERT_EQUAL_UINT32(2, r1.skipped);  /* bad1, bad2 (4 個を調べた) */
    TEST_ASSERT_TRUE(r1.has_next);
    TEST_ASSERT_EQUAL_UINT32(2, r1.next_cursor);
    iso_rescan_result_t r2 = rescan(r1.next_cursor);
    TEST_ASSERT_EQUAL_UINT32(0, r2.imported);
    TEST_ASSERT_EQUAL_UINT32(3, r2.skipped);
    TEST_ASSERT_FALSE(r2.has_next);
}

void test_import_signature_is_read_from_sector_16_bytes_1_to_5(void) {
    /* バイト 0 (記述子の種類) は見ない。バイト 1〜5 だけ。位置が 1 つずれていれば不可 */
    static uint8_t head[40000];
    memset(head, 0, sizeof(head));
    memcpy(head + 32768 + 1, "CD001", 5);
    head[32768] = 0xFF;
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/import/t0.iso", 40000, head, 32776, 0));
    memset(head, 0, sizeof(head));
    memcpy(head + 32768, "CD001", 5); /* 1 バイト手前 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/import/t1.iso", 40000, head, 32776, 0));
    memset(head, 0, sizeof(head));
    memcpy(head + 32768 + 2, "CD001", 5); /* 1 バイト後ろ */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/import/t2.iso", 40000, head, 32776, 0));
    memset(head, 0, sizeof(head));
    memcpy(head + 32768 - 2048 + 1, "CD001", 5); /* セクタ 15 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/import/t3.iso", 40000, head, 32776, 0));
    memset(head, 0, sizeof(head));
    memcpy(head + 32768 + 2048 + 1, "CD001", 5); /* セクタ 17 (頭が 40000 バイトなので存在する) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/import/t4.iso", 40000, head, 32768 + 2048 + 8, 0));
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_EQUAL_UINT32(3, r.skipped);
    TEST_ASSERT_TRUE(r.has_next);
    r = rescan(r.next_cursor);
    TEST_ASSERT_EQUAL_UINT32(0, r.imported);
    TEST_ASSERT_EQUAL_UINT32(1, r.skipped);
    TEST_ASSERT_FALSE(r.has_next);
    TEST_ASSERT_EQUAL_STRING("t0", CAT.images[0].meta.name);
}

void test_import_same_name_and_limit(void) {
    iso_meta_t m = mk_meta("dup", 100000, ISO_META_READY, 1);
    put_slot("00000001", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    put_iso("dup.iso", 40000);   /* 同名: skipped */
    put_iso("Dup.iso", 40000);   /* 大文字小文字が違う: 別の名前 */
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_EQUAL_UINT32(1, r.skipped);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/dup.iso"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/import/Dup.iso"));
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
}

void test_import_image_limit(void) {
    for (int i = 0; i < 15; i++) {
        char id[9];
        snprintf(id, sizeof(id), "%08x", 0x300 + i);
        iso_meta_t m = mk_meta("x", 100000, ISO_META_READY, 1);
        m.name[0] = (char)('A' + i);
        put_slot(id, 0, &m);
    }
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(15, CAT.count);
    put_iso("one.iso", 40000);
    put_iso("two.iso", 40000);
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported); /* 16 個目までは OK */
    TEST_ASSERT_EQUAL_UINT32(1, r.skipped);  /* 17 個目は skipped */
    TEST_ASSERT_EQUAL_UINT(16, CAT.count);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/two.iso"));
}

void test_import_crosslinked_file_is_skipped_and_left_alone(void) {
    /* rename の途中の電源断: import/foo.iso と <id>/p000 が、同じ先頭クラスタを指している */
    iso_meta_t m = mk_meta("orig", 40000, ISO_META_READY, 1); /* import/foo.iso とは別の名前(名前の重複で skipped にならないように) */
    m.part_count = 1;
    m.part_blocks = 20;
    put_slot("00000001", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/00000001/p000", 40000, NULL, 0, 777));
    put_import("foo.iso", 40000, "CD001", 777);
    put_import("other.iso", 40000, "CD001", 778);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    fx_settle();
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported); /* other */
    TEST_ASSERT_EQUAL_UINT32(1, r.skipped);  /* foo (クロスリンク) */
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/foo.iso")); /* 消さない(消すと p000 のクラスタが解放される) */
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000001/p000"));
    TEST_ASSERT_EQUAL_UINT32(777, mock_fs_first_cluster(&FS, "/iso/00000001/p000"));
    for (int i = 0; i < FS.nlog; i++) {
        TEST_ASSERT_NULL_MESSAGE(strstr(FS.log[i], "foo.iso"), "no write may touch the crosslinked file");
    }
}

void test_import_crosslink_between_two_files_in_one_batch(void) {
    put_import("first.iso", 40000, "CD001", 555);
    put_import("second.iso", 40000, "CD001", 555); /* 同じ先頭クラスタ */
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_EQUAL_UINT32(1, r.skipped);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/import/first.iso"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/second.iso"));
}

void test_import_cluster_zero_never_counts_as_crosslink(void) {
    put_import("a.iso", 40000, "CD001", 5);
    put_import("b.iso", 40000, "CD001", 6);
    TEST_ASSERT_TRUE(mock_fs_set_first_cluster(&FS, "/iso/import/a.iso", 0));
    TEST_ASSERT_TRUE(mock_fs_set_first_cluster(&FS, "/iso/import/b.iso", 0));
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(2, r.imported); /* 0 同士は一致扱いにしない */
    TEST_ASSERT_EQUAL_UINT32(0, r.skipped);
}

void test_import_crosslink_check_tolerates_images_without_p000(void) {
    iso_meta_t m = mk_meta("empty", 40000, ISO_META_INCOMPLETE, 1);
    put_slot("00000001", 0, &m); /* p000 はまだ無い */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    put_import("a.iso", 40000, "CD001", 0);
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
}

void test_rescan_batch_of_four_and_cursor(void) {
    char name[16];
    for (int i = 0; i < 10; i++) {
        snprintf(name, sizeof(name), "f%d.iso", i);
        put_iso(name, 40000);
    }
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(4, r.imported);
    TEST_ASSERT_EQUAL_UINT32(0, r.skipped);
    TEST_ASSERT_TRUE(r.has_next);
    TEST_ASSERT_EQUAL_UINT32(0, r.next_cursor); /* 取り込んだものはディレクトリから消える = 位置は進まない */
    TEST_ASSERT_EQUAL_UINT(4, CAT.count);
    TEST_ASSERT_EQUAL_INT(6, mock_fs_child_count(&FS, "/iso/import"));
    r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(4, r.imported);
    TEST_ASSERT_TRUE(r.has_next);
    r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(2, r.imported);
    TEST_ASSERT_FALSE(r.has_next);
    TEST_ASSERT_EQUAL_UINT(10, CAT.count);
    TEST_ASSERT_EQUAL_INT(0, mock_fs_child_count(&FS, "/iso/import"));
}

void test_rescan_exactly_four_entries_has_no_next(void) {
    char name[16];
    for (int i = 0; i < 4; i++) {
        snprintf(name, sizeof(name), "f%d.iso", i);
        put_iso(name, 40000);
    }
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(4, r.imported);
    TEST_ASSERT_FALSE(r.has_next); /* 5 個目がない */
    /* 5 個なら続きがある */
    for (int i = 4; i < 9; i++) {
        snprintf(name, sizeof(name), "g%d.iso", i);
        put_iso(name, 40000);
    }
    r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(4, r.imported);
    TEST_ASSERT_TRUE(r.has_next);
    r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_FALSE(r.has_next);
}

void test_rescan_skipped_entries_advance_the_cursor(void) {
    put_iso("s0.iso", 100);  /* skipped */
    put_iso("s1.iso", 100);  /* skipped */
    put_iso("g0.iso", 40000);
    put_iso("g1.iso", 40000);
    put_iso("g2.iso", 40000);
    put_iso("g3.iso", 40000);
    put_iso("g4.iso", 40000);
    iso_rescan_result_t r = rescan(0); /* s0 s1 g0 g1 を調べる */
    TEST_ASSERT_EQUAL_UINT32(2, r.imported);
    TEST_ASSERT_EQUAL_UINT32(2, r.skipped);
    TEST_ASSERT_TRUE(r.has_next);
    TEST_ASSERT_EQUAL_UINT32(2, r.next_cursor); /* C' = C + M */
    r = rescan(2); /* ディレクトリは s0 s1 g2 g3 g4。先頭の 2 個(skipped を残したもの)を読み飛ばす */
    TEST_ASSERT_EQUAL_UINT32(3, r.imported);
    TEST_ASSERT_EQUAL_UINT32(0, r.skipped);
    TEST_ASSERT_FALSE(r.has_next);
    TEST_ASSERT_EQUAL_UINT(5, CAT.count);
}

void test_rescan_cursor_adds_up_over_several_calls(void) {
    char name[16];
    for (int i = 0; i < 9; i++) { /* 9 個とも skipped */
        snprintf(name, sizeof(name), "s%d.iso", i);
        put_iso(name, 100);
    }
    uint32_t cursor = 0;
    iso_rescan_result_t r = rescan(cursor);
    TEST_ASSERT_EQUAL_UINT32(4, r.skipped);
    TEST_ASSERT_TRUE(r.has_next);
    TEST_ASSERT_EQUAL_UINT32(4, r.next_cursor);
    r = rescan(r.next_cursor);
    TEST_ASSERT_EQUAL_UINT32(4, r.skipped);
    TEST_ASSERT_TRUE(r.has_next);
    TEST_ASSERT_EQUAL_UINT32(8, r.next_cursor);
    r = rescan(r.next_cursor);
    TEST_ASSERT_EQUAL_UINT32(1, r.skipped);
    TEST_ASSERT_FALSE(r.has_next);
}

void test_rescan_cursor_counts_only_iso_files(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/readme.txt", "x", 1, 0));
    put_iso("a.iso", 40000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/import/b.txt", "x", 1, 0));
    put_iso("c.iso", 100); /* skipped */
    put_iso("d.iso", 100); /* skipped */
    iso_rescan_result_t r = rescan(1); /* a.iso を読み飛ばす (readme.txt, b.txt は数えない) */
    TEST_ASSERT_EQUAL_UINT32(0, r.imported);
    TEST_ASSERT_EQUAL_UINT32(2, r.skipped);
    TEST_ASSERT_FALSE(r.has_next);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/a.iso"));
}

void test_rescan_cursor_beyond_the_end(void) {
    put_iso("a.iso", 40000);
    iso_rescan_result_t r = rescan(100);
    TEST_ASSERT_EQUAL_UINT32(0, r.imported);
    TEST_ASSERT_EQUAL_UINT32(0, r.skipped);
    TEST_ASSERT_FALSE(r.has_next);
    r = rescan(1); /* ちょうど個数 */
    TEST_ASSERT_EQUAL_UINT32(0, r.imported);
    TEST_ASSERT_FALSE(r.has_next);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/a.iso"));
}

void test_rescan_loop_terminates_like_the_viewer(void) {
    char name[16];
    for (int i = 0; i < 7; i++) {
        snprintf(name, sizeof(name), "%c%d.iso", (i % 2) ? 's' : 'g', i);
        put_iso(name, (i % 2) ? 100 : 40000); /* 交互に skipped / 取り込み */
    }
    uint32_t cursor = 0, imported = 0, skipped = 0;
    int calls = 0;
    for (;;) {
        iso_rescan_result_t r = rescan(cursor);
        imported += r.imported;
        skipped += r.skipped;
        calls++;
        TEST_ASSERT_TRUE(calls < 10);
        if (!r.has_next) break;
        cursor = r.next_cursor;
    }
    TEST_ASSERT_EQUAL_UINT32(4, imported);
    TEST_ASSERT_EQUAL_UINT32(3, skipped);
    TEST_ASSERT_EQUAL_INT(3, mock_fs_child_count(&FS, "/iso/import"));
}

void test_rescan_without_import_directory_does_nothing(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/import"));
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(0, r.imported);
    TEST_ASSERT_EQUAL_UINT32(0, r.skipped);
    TEST_ASSERT_FALSE(r.has_next);
}

void test_rescan_io_errors_are_propagated(void) {
    put_iso("a.iso", 40000);
    iso_rescan_result_t r;
    mock_fs_fail_path(&FS, "/iso/import");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    mock_fs_fail_path(&FS, NULL);
    mock_fs_fail_dir_next(&FS, "/iso/import");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    mock_fs_fail_dir_next(&FS, NULL);
    mock_fs_fail_path(&FS, "/iso/import/a.iso"); /* 署名の読み出し(open_for_read)が IO エラー */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    mock_fs_fail_path(&FS, NULL);
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/a.iso"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
}

void test_rescan_read_errors_while_checking_the_signature(void) {
    put_iso("a.iso", 40000);
    iso_rescan_result_t r;
    /* 先頭から順に読む途中(1 回目 = 最初の 512 バイト)の IO エラー */
    mock_fs_fail_read(&FS, "/iso/import/a.iso", 1);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files); /* 失敗しても閉じる */
    /* 署名の 6 バイトの読み出し(65 回目 = 512 バイト x 64 の後)の IO エラー */
    mock_fs_fail_read(&FS, "/iso/import/a.iso", 65);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/a.iso"));
    /* 66 回目は読まない: 失敗の注入が当たらず、取り込める */
    mock_fs_fail_read(&FS, "/iso/import/a.iso", 66);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    mock_fs_fail_read(&FS, NULL, 0);
}

void test_import_crosslink_is_checked_against_every_image_and_ignores_directories(void) {
    iso_meta_t m1 = mk_meta("one", 40000, ISO_META_READY, 1);
    iso_meta_t m2 = mk_meta("two", 40000, ISO_META_READY, 1);
    put_slot("00000001", 0, &m1);
    put_slot("00000002", 0, &m2);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/00000001/p000", 40000, NULL, 0, 700));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/00000002/p000", 40000, NULL, 0, 701));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    put_import("x.iso", 40000, "CD001", 701); /* 2 つ目のイメージと同じ先頭クラスタ */
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(0, r.imported);
    TEST_ASSERT_EQUAL_UINT32(1, r.skipped);
    /* p000 がディレクトリなら、先頭クラスタが同じでも、クロスリンクとは見なさない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/00000002/p000"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000002/p000"));
    TEST_ASSERT_TRUE(mock_fs_set_first_cluster(&FS, "/iso/00000002/p000", 701));
    r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_EQUAL_UINT32(0, r.skipped);
}

void test_rescan_crosslink_stat_error_is_propagated(void) {
    iso_meta_t m = mk_meta("foo", 40000, ISO_META_READY, 1);
    put_slot("00000001", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/00000001/p000", 40000, NULL, 0, 777));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    put_iso("a.iso", 40000);
    mock_fs_fail_path(&FS, "/iso/00000001/p000");
    iso_rescan_result_t r;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_EQUAL_UINT32(0, r.imported);
}

void test_rescan_counts_before_an_error_are_kept(void) {
    put_iso("a.iso", 40000);
    put_iso("b.iso", 40000);
    iso_rescan_result_t r;
    /* a は取り込めるが、b の署名の読み出しで IO エラー */
    mock_fs_fail_path(&FS, "/iso/import/b.iso");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
}

void test_import_write_failures_clean_up_and_keep_the_file(void) {
    put_iso("a.iso", 40000);
    iso_rescan_result_t r;
    /* 1: mkdir が失敗 */
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_NO_SPACE);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_SPACE, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_INT(1, FS.nlog);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/a.iso"));
    /* 2: meta の書き込みが失敗 -> ディレクトリを片づける */
    fx_default_rnd();
    fx_settle();
    mock_fs_error_at(&FS, 2, ISO_ERR_IO);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/11110001"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/a.iso"));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    /* 3: rename が失敗 -> meta.a とディレクトリを片づける(ファイルは import/ に残る) */
    fx_default_rnd();
    fx_settle();
    mock_fs_error_at(&FS, 3, ISO_ERR_IO);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/11110001"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/a.iso"));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_INT(5, FS.nlog); /* mkdir、overwrite、rename(失敗)、remove meta.a、remove dir */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "remove /iso/11110001/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 4, "remove /iso/11110001"));
    /* 失敗がなければ取り込める */
    fx_default_rnd();
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
}

void test_import_id_collision_retries(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/11110001")); /* 乱数の最初の値と衝突 */
    put_iso("a.iso", 40000);
    iso_rescan_result_t r = rescan(0);
    TEST_ASSERT_EQUAL_UINT32(1, r.imported);
    TEST_ASSERT_EQUAL_STRING("11110002", CAT.images[0].id);
}

void test_import_result_is_cleared_on_each_call(void) {
    /* 有効な(bool が true の)非 0 の値で埋める。初期化を省くと、アサーションで見つかる(不正な bool を読まない) */
    iso_rescan_result_t r = {.imported = 7, .skipped = 9, .has_next = true, .next_cursor = 5};
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_rescan(&CAT, 0, &r));
    TEST_ASSERT_EQUAL_UINT32(0, r.imported);
    TEST_ASSERT_EQUAL_UINT32(0, r.skipped);
    TEST_ASSERT_FALSE(r.has_next);
    TEST_ASSERT_EQUAL_UINT32(0, r.next_cursor);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_import_one_file);
    RUN_TEST(test_import_extension_is_case_insensitive_and_other_entries_are_ignored);
    RUN_TEST(test_import_name_rules);
    RUN_TEST(test_import_size_boundaries);
    RUN_TEST(test_import_part_size_limit);
    RUN_TEST(test_import_signature);
    RUN_TEST(test_import_signature_is_read_from_sector_16_bytes_1_to_5);
    RUN_TEST(test_import_same_name_and_limit);
    RUN_TEST(test_import_image_limit);
    RUN_TEST(test_import_crosslinked_file_is_skipped_and_left_alone);
    RUN_TEST(test_import_crosslink_between_two_files_in_one_batch);
    RUN_TEST(test_import_cluster_zero_never_counts_as_crosslink);
    RUN_TEST(test_import_crosslink_check_tolerates_images_without_p000);
    RUN_TEST(test_rescan_batch_of_four_and_cursor);
    RUN_TEST(test_rescan_exactly_four_entries_has_no_next);
    RUN_TEST(test_rescan_skipped_entries_advance_the_cursor);
    RUN_TEST(test_rescan_cursor_adds_up_over_several_calls);
    RUN_TEST(test_rescan_cursor_counts_only_iso_files);
    RUN_TEST(test_rescan_cursor_beyond_the_end);
    RUN_TEST(test_rescan_loop_terminates_like_the_viewer);
    RUN_TEST(test_rescan_without_import_directory_does_nothing);
    RUN_TEST(test_rescan_io_errors_are_propagated);
    RUN_TEST(test_rescan_read_errors_while_checking_the_signature);
    RUN_TEST(test_import_crosslink_is_checked_against_every_image_and_ignores_directories);
    RUN_TEST(test_rescan_crosslink_stat_error_is_propagated);
    RUN_TEST(test_rescan_counts_before_an_error_are_kept);
    RUN_TEST(test_import_write_failures_clean_up_and_keep_the_file);
    RUN_TEST(test_import_id_collision_retries);
    RUN_TEST(test_import_result_is_cleared_on_each_call);
    return UNITY_END();
}
