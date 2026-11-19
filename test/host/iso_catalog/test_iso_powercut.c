/*
 * iso_catalog: 電源断の網羅。モックの fs で、書き込み(mkdir / remove / rename / overwrite)のすべての位置で電源を落とし
 * (落とし方 3 通り: 何も起きない / overwrite が前半だけ書けた / 書き込みは完了して戻り値だけ IO エラー)、
 * 再起動(スキャン)して、不変条件を確かめる:
 *   (1) 有効なスロットが残る(その内容は、最後に完了した meta の書き込み、または電源断の書き込みが完了していればその次)、
 *       または meta_corrupt として削除できる。
 *   (2) 削除の手順の (1) が済んだ後は、どこで電源が落ちても ready / incomplete は復活しない(deleting を続ける、または meta_corrupt)。
 *   (3) どの状態からでも、削除のステップを最後まで進めると、ディレクトリが残らない。
 * 期待する meta の列(世代、状態、理由)は、手書き。
 */
#include "ic_common.h"

void setUp(void) { fx_setup(); }
void tearDown(void) { fx_teardown(); }

#define ID "11110001"

typedef struct {
    uint32_t gen;
    uint8_t state;
    uint8_t reason;
} expect_t;

/* シナリオ 1: begin -> idle -> resume -> idle -> finish -> finalize_ok -> index_fail -> delete。
 * 書き込みの列(通し番号 1 から):
 *   1 mkdir  2 L1  3 L2  4 L3  5 L4  6 L5  7 L6  8 remove p000  9 remove p001  10 remove meta.a  11 remove meta.b  12 rmdir
 * L[i] = i 番目の meta の書き込み。 */
static const expect_t L[7] = {
    {0, 0, 0},
    {1, ISO_META_UPLOADING, 0},    /* L1: begin -> meta.a */
    {2, ISO_META_INCOMPLETE, 0},   /* L2: idle -> meta.b */
    {3, ISO_META_INCOMPLETE, 0},   /* L3: idle (resume の後) -> meta.a */
    {4, ISO_META_READY, 0},        /* L4: finalize ok -> meta.b */
    {5, ISO_META_INVALID, 4},      /* L5: index fail (part_mismatch) -> meta.a */
    {6, ISO_META_DELETING, 4},     /* L6: 削除の手順 (1) -> meta.b */
};
#define N_MUT 12

/* 書き込みの通し番号 -> L の何番目か(0 = meta の書き込みではない) */
static int meta_index_of_mutation(int n) {
    if (n >= 2 && n <= 7) return n - 1;
    return 0;
}

/* シナリオ 1 を、最後まで(または最初のエラーまで)実行する。戻り値: 完了すれば ISO_OK */
static int run_scenario(iso_catalog_t *cat) {
    char id[9];
    int rc = iso_catalog_begin(cat, "Alpha", 5, 100000, NULL, id);
    if (rc < 0) return rc;
    iso_catalog_entry_t *e = iso_catalog_find(cat, id);
    /* アップロードしたパーツ(準備。書き込みの数には入らない) */
    char p[ISO_PATH_MAX];
    snprintf(p, sizeof(p), "/iso/%s/p000", id);
    mock_fs_add_sparse(&FS, p, 100000, NULL, 0, 0);
    snprintf(p, sizeof(p), "/iso/%s/p001", id);
    mock_fs_add_sparse(&FS, p, 10, NULL, 0, 0);
    const iso_trans_ctx_t free_ctx = {false, true, false};
    rc = iso_catalog_apply(cat, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL);
    if (rc != 0) return rc ? rc : ISO_ERR_INVALID;
    rc = iso_catalog_apply(cat, e, ISO_EV_PUT, &free_ctx, NULL);
    if (rc != 0) return rc ? rc : ISO_ERR_INVALID;
    rc = iso_catalog_apply(cat, e, ISO_EV_IDLE_TIMEOUT, NULL, NULL);
    if (rc != 0) return rc ? rc : ISO_ERR_INVALID;
    e->received = 100000;
    rc = iso_catalog_apply(cat, e, ISO_EV_FINISH, &free_ctx, NULL);
    if (rc != 0) return rc ? rc : ISO_ERR_INVALID;
    rc = iso_catalog_apply(cat, e, ISO_EV_FINALIZE_OK, NULL, NULL);
    if (rc != 0) return rc ? rc : ISO_ERR_INVALID;
    iso_apply_arg_t arg;
    memset(&arg, 0, sizeof(arg));
    arg.invalid_reason = ISO_INVALID_PART_MISMATCH;
    rc = iso_catalog_apply(cat, e, ISO_EV_INDEX_FAIL, NULL, &arg);
    if (rc != 0) return rc ? rc : ISO_ERR_INVALID;
    rc = iso_catalog_apply(cat, e, ISO_EV_DELETE, &free_ctx, NULL);
    if (rc != 0) return rc ? rc : ISO_ERR_INVALID;
    iso_delete_job_t job;
    rc = iso_delete_job_init(&job, id);
    if (rc < 0) return rc;
    for (int i = 0; i < 20; i++) {
        iso_step_result_t r = iso_catalog_delete_step(cat, &job);
        if (r.kind == ISO_STEP_DONE) return ISO_OK;
        if (r.kind == ISO_STEP_ERROR) return r.err;
    }
    return ISO_ERR_BUSY;
}

/* 再起動: 電源を戻し、新しいカタログでスキャンする */
static void reboot(iso_catalog_t *c2) {
    mock_fs_power_on(&FS);
    iso_catalog_init(c2, &IFS, &IRND);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(c2));
}

/* すべてのエントリを、削除のステップで最後まで消す。/iso にディレクトリが残らないことを確かめる */
static void delete_everything_and_check_clean(iso_catalog_t *c) {
    for (size_t i = 0; i < 16 && c->count > 0; i++) {
        iso_delete_job_t job;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, c->images[0].id));
        iso_step_result_t r;
        int guard = 0;
        do {
            r = iso_catalog_delete_step(c, &job);
            TEST_ASSERT_TRUE_MESSAGE(++guard < 30, "delete did not finish");
        } while (r.kind == ISO_STEP_CONTINUE);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STEP_DONE, r.kind, "delete must finish after a reboot");
    }
    TEST_ASSERT_EQUAL_UINT(0, c->count);
    char names[8][ISO_NAME_MAX + 1];
    int n = mock_fs_children(&FS, "/iso", names, 8);
    for (int i = 0; i < n; i++) TEST_ASSERT_EQUAL_STRING_MESSAGE("import", names[i], "only /iso/import may remain");
}

static const char *TEAR_NAME[] = {"NONE", "HALF", "FULL"};

void test_scenario_without_a_cut_has_the_expected_write_sequence(void) {
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, run_scenario(&CAT));
    TEST_ASSERT_EQUAL_INT(N_MUT, FS.mut_count);
    TEST_ASSERT_EQUAL_INT(N_MUT, FS.nlog);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "mkdir /iso/11110001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "overwrite /iso/11110001/meta.a len=256"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "overwrite /iso/11110001/meta.b len=256"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "overwrite /iso/11110001/meta.a len=256"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 4, "overwrite /iso/11110001/meta.b len=256"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 5, "overwrite /iso/11110001/meta.a len=256"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 6, "overwrite /iso/11110001/meta.b len=256"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 7, "remove /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 8, "remove /iso/11110001/p001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 9, "remove /iso/11110001/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 10, "remove /iso/11110001/meta.b"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 11, "remove /iso/11110001"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/11110001"));
}

void test_power_cut_at_every_write_and_every_tear(void) {
    unsigned cases = 0, saw_valid = 0, saw_corrupt = 0, saw_absent = 0, saw_deleting = 0;
    for (int n = 1; n <= N_MUT; n++) {
        for (int t = 0; t < 3; t++) {
            /* 新しいカード */
            mock_fs_free(&FS);
            fx_setup();
            fx_settle();
            mock_fs_cut_at(&FS, n, (mock_tear_t)t);
            int rc = run_scenario(&CAT);
            char msg[120];
            snprintf(msg, sizeof(msg), "cut at write %d, tear %s", n, TEAR_NAME[t]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_ERR_IO, rc, msg); /* 電源断のところで止まる */
            TEST_ASSERT_EQUAL_INT_MESSAGE(n, FS.mut_count, msg);

            iso_catalog_t c2;
            reboot(&c2);
            cases++;

            int meta_idx = meta_index_of_mutation(n);
            int completed = 0; /* 電源断より前に完了した meta の書き込みの数 */
            for (int k = 2; k < n && k <= 7; k++) completed++;
            bool applied_meta = (t == MOCK_TEAR_FULL && meta_idx > 0);
            int effective = completed + (applied_meta ? 1 : 0); /* この数番目までの meta が、ディスクに残っている */

            if (n >= 8) {
                /* 削除の手順 (1) の後: 電源断の位置ごとに、期待する状態が決まる。
                 *   8 / 9 / 10 (p000、p001、meta.a(古い方)の削除): どのときも deleting (meta.b = 世代 6 が残る)
                 *   11 (meta.b の削除): 完了していれば、スロットがなく meta_corrupt。そうでなければ deleting
                 *   12 (rmdir): 完了していれば、ディレクトリがない。そうでなければ、空のディレクトリ = meta_corrupt */
                bool expect_absent = (n == 12 && t == MOCK_TEAR_FULL);
                bool expect_corrupt = (n == 11 && t == MOCK_TEAR_FULL) || (n == 12 && t != MOCK_TEAR_FULL);
                if (expect_absent) {
                    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, c2.count, msg);
                    saw_absent++;
                } else {
                    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, c2.count, msg);
                    iso_catalog_entry_t *e = &c2.images[0];
                    if (expect_corrupt) {
                        TEST_ASSERT_TRUE_MESSAGE(e->state == ISO_STATE_INVALID && !e->meta_valid && e->invalid_reason == ISO_INVALID_META_CORRUPT, msg);
                        saw_corrupt++;
                    } else {
                        TEST_ASSERT_TRUE_MESSAGE(e->state == ISO_STATE_DELETING && e->resume_delete && e->meta_valid, msg);
                        TEST_ASSERT_EQUAL_UINT32_MESSAGE(L[6].gen, e->meta.generation, msg);
                        saw_deleting++;
                    }
                }
            } else if (n == 1 && t != MOCK_TEAR_FULL) {
                TEST_ASSERT_EQUAL_UINT_MESSAGE(0, c2.count, msg); /* mkdir が起きていない */
                saw_absent++;
            } else if (effective == 0) {
                /* ディレクトリはあるが、有効な meta がない(mkdir だけ、または meta.a が途中まで) */
                TEST_ASSERT_EQUAL_UINT_MESSAGE(1, c2.count, msg);
                TEST_ASSERT_FALSE_MESSAGE(c2.images[0].meta_valid, msg);
                TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STATE_INVALID, c2.images[0].state, msg);
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(ISO_INVALID_META_CORRUPT, c2.images[0].invalid_reason, msg);
                saw_corrupt++;
            } else {
                TEST_ASSERT_EQUAL_UINT_MESSAGE(1, c2.count, msg);
                iso_catalog_entry_t *e = &c2.images[0];
                TEST_ASSERT_TRUE_MESSAGE(e->meta_valid, msg);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(L[effective].gen, e->meta.generation, msg);
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(L[effective].state, e->meta.state, msg);
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(L[effective].reason, e->meta.invalid_reason, msg);
                TEST_ASSERT_EQUAL_STRING_MESSAGE("Alpha", e->meta.name, msg);
                /* uploading は incomplete として扱う */
                if (L[effective].state == ISO_META_UPLOADING) TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STATE_INCOMPLETE, e->state, msg);
                saw_valid++;
            }
            delete_everything_and_check_clean(&c2);
        }
    }
    TEST_ASSERT_EQUAL_UINT(36, cases);
    /* 空振りでないことの確認 */
    TEST_ASSERT_TRUE(saw_valid >= 15);
    TEST_ASSERT_TRUE(saw_corrupt >= 5);
    TEST_ASSERT_TRUE(saw_deleting >= 5);
    TEST_ASSERT_TRUE(saw_absent >= 3);
}

/* 削除の最中の電源断を、さらに削除のやり直し(再起動後の続き)の最中にも落とす(二重の電源断) */
void test_double_power_cut_during_delete(void) {
    unsigned cases = 0, cases_double = 0, cases_finished = 0, cases_absent = 0;
    for (int n = 1; n <= 6; n++) {
        for (int t = 0; t < 3; t++) {
            for (int m = 1; m <= 6; m++) {
                for (int t2 = 0; t2 < 3; t2++) {
                    mock_fs_free(&FS);
                    fx_setup();
                    iso_meta_t a = mk_meta("Img", 5000000, ISO_META_READY, 7);
                    iso_meta_t b = mk_meta("Img", 5000000, ISO_META_INCOMPLETE, 6);
                    put_slot("00000001", 0, &a);
                    put_slot("00000001", 1, &b);
                    put_part("00000001", 0, 3000000);
                    put_part("00000001", 1, 2000000);
                    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
                    fx_settle();
                    char msg[120];
                    snprintf(msg, sizeof(msg), "cut %d/%s then %d/%s", n, TEAR_NAME[t], m, TEAR_NAME[t2]);

                    /* 1 回目: n 番目の書き込みで電源断 */
                    mock_fs_cut_at(&FS, n, (mock_tear_t)t);
                    iso_delete_job_t job;
                    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, "00000001"));
                    iso_step_result_t r;
                    int guard = 0;
                    do {
                        r = iso_catalog_delete_step(&CAT, &job);
                    } while (r.kind == ISO_STEP_CONTINUE && ++guard < 30);
                    TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STEP_ERROR, r.kind, msg);

                    iso_catalog_t c2;
                    reboot(&c2);
                    /* 書き込みの列: 1 meta (deleting)、2 p000、3 p001、4 meta.a (古い)、5 meta.b、6 rmdir。
                     * 1 回目の電源断の直後の状態: 1 の NONE / HALF は元の ready のまま(世代 7)。5 の FULL は meta_corrupt、6 の NONE / HALF も
                     * (空のディレクトリ)meta_corrupt、6 の FULL はディレクトリなし。それ以外は deleting */
                    bool old_ready = (n == 1 && t != MOCK_TEAR_FULL);
                    bool is_corrupt = (n == 5 && t == MOCK_TEAR_FULL) || (n == 6 && t != MOCK_TEAR_FULL);
                    bool is_absent = (n == 6 && t == MOCK_TEAR_FULL);
                    if (is_absent) {
                        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, c2.count, msg);
                        cases_absent++;
                    } else {
                        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, c2.count, msg);
                        iso_catalog_entry_t *e = &c2.images[0];
                        if (old_ready) {
                            TEST_ASSERT_TRUE_MESSAGE(e->state == ISO_STATE_READY && e->meta.generation == 7, msg);
                        } else if (is_corrupt) {
                            TEST_ASSERT_TRUE_MESSAGE(e->state == ISO_STATE_INVALID && !e->meta_valid, msg);
                        } else {
                            TEST_ASSERT_TRUE_MESSAGE(e->state == ISO_STATE_DELETING && e->resume_delete && e->meta_valid, msg);
                        }

                        /* 2 回目: 続きの m 番目の書き込みで、また電源断 */
                        mock_fs_cut_at(&FS, m, (mock_tear_t)t2);
                        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, e->id));
                        guard = 0;
                        do {
                            r = iso_catalog_delete_step(&c2, &job);
                        } while (r.kind == ISO_STEP_CONTINUE && ++guard < 30);
                        /* 2 回目の電源断の位置に到達したか(到達していれば、電源が落ちていて、ISO_STEP_ERROR / ISO_ERR_IO) */
                        bool reached = (r.kind == ISO_STEP_ERROR && r.err == ISO_ERR_IO && FS.dead);
                        if (reached) {
                            cases_double++;
                        } else {
                            TEST_ASSERT_FALSE_MESSAGE(FS.dead, msg);
                            TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STEP_DONE, r.kind, msg); /* 到達しなければ、削除が完了している */
                            cases_finished++;
                        }
                        iso_catalog_t c3;
                        reboot(&c3);
                        for (size_t i = 0; i < c3.count; i++) {
                            bool d3 = (c3.images[i].state == ISO_STATE_DELETING && c3.images[i].resume_delete);
                            bool k3 = (c3.images[i].state == ISO_STATE_INVALID && !c3.images[i].meta_valid);
                            /* 元の ready (世代 7)が許されるのは、2 回目も deleting の書き込みが完了していないとき(m = 1 の NONE / HALF)だけ */
                            bool o3 = (c3.images[i].state == ISO_STATE_READY && c3.images[i].meta.generation == 7 && n == 1 && t != MOCK_TEAR_FULL &&
                                       m == 1 && t2 != MOCK_TEAR_FULL);
                            TEST_ASSERT_TRUE_MESSAGE(d3 || k3 || o3, msg);
                        }
                        delete_everything_and_check_clean(&c3);
                    }
                    cases++;
                }
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(6 * 3 * 6 * 3, cases);
    /* 2 回目の電源断に到達したものだけが「二重の電源断」。到達しなかったもの(再開が短い)は、別に数える */
    printf("double power cut: reached=%u finished_before=%u absent_after_first=%u\n", cases_double, cases_finished, cases_absent);
    TEST_ASSERT_TRUE(cases_double > 150);
    TEST_ASSERT_TRUE(cases_finished > 20);
}

/* import の電源断: mkdir / meta / rename のそれぞれ */
void test_power_cut_during_import(void) {
    for (int n = 1; n <= 3; n++) {
        for (int t = 0; t < 3; t++) {
            mock_fs_free(&FS);
            fx_setup();
            put_import("Linux.iso", 40000, "CD001", 0);
            uint32_t cluster = mock_fs_first_cluster(&FS, "/iso/import/Linux.iso");
            fx_settle();
            char msg[100];
            snprintf(msg, sizeof(msg), "import: cut at write %d, tear %s", n, TEAR_NAME[t]);
            mock_fs_cut_at(&FS, n, (mock_tear_t)t);
            iso_rescan_result_t r;
            TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r), msg);

            iso_catalog_t c2;
            reboot(&c2);
            bool renamed = (n == 3 && t == MOCK_TEAR_FULL);
            if (renamed) {
                /* rename まで完了: ファイルは p000 にあり、import/ にはない */
                TEST_ASSERT_FALSE_MESSAGE(mock_fs_exists(&FS, "/iso/import/Linux.iso"), msg);
                TEST_ASSERT_EQUAL_UINT_MESSAGE(1, c2.count, msg);
                TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STATE_READY, c2.images[0].state, msg);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(cluster, mock_fs_first_cluster(&FS, "/iso/11110001/p000"), msg);
            } else {
                /* ファイルは import/ に残っている(ファイルは失われない) */
                TEST_ASSERT_TRUE_MESSAGE(mock_fs_exists(&FS, "/iso/import/Linux.iso"), msg);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(cluster, mock_fs_first_cluster(&FS, "/iso/import/Linux.iso"), msg);
                /* 残ったディレクトリは、meta_corrupt (mkdir だけ / meta が途中)、または meta だけの ready */
                for (size_t i = 0; i < c2.count; i++) {
                    bool corrupt = (c2.images[i].state == ISO_STATE_INVALID && !c2.images[i].meta_valid);
                    bool ready_without_part = (c2.images[i].state == ISO_STATE_READY && c2.images[i].meta_valid);
                    TEST_ASSERT_TRUE_MESSAGE(corrupt || ready_without_part, msg);
                }
                /* もう一度 rescan する。meta だけが残った ready (同名) があれば、同名として skipped (INDEX が part_mismatch にして、
                 * 利用者が削除するまで)。meta_corrupt のものは名前がないので、取り込める(新しい id で) */
                bool stale_named = false;
                for (size_t i = 0; i < c2.count; i++) stale_named = stale_named || c2.images[i].meta_valid;
                size_t before = c2.count;
                iso_rescan_result_t r2;
                TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_OK, iso_catalog_rescan(&c2, 0, &r2), msg);
                if (stale_named) {
                    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r2.imported, msg);
                    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r2.skipped, msg);
                    TEST_ASSERT_TRUE_MESSAGE(mock_fs_exists(&FS, "/iso/import/Linux.iso"), msg);
                    delete_everything_and_check_clean(&c2); /* 古いものを消せば、取り込める */
                    TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_OK, iso_catalog_rescan(&c2, 0, &r2), msg);
                    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r2.imported, msg);
                } else {
                    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r2.imported, msg);
                    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1, c2.count, msg);
                }
                TEST_ASSERT_FALSE_MESSAGE(mock_fs_exists(&FS, "/iso/import/Linux.iso"), msg);
            }
            delete_everything_and_check_clean(&c2);
        }
    }
}

/* rename の途中の電源断で、import/ のファイルと <id>/p000 の両方が残る(同じ先頭クラスタ。設計 3.3)ときの復旧 */
void test_power_cut_in_rename_leaves_both_names_and_recovery_keeps_the_shared_cluster(void) {
    put_import("Linux.iso", 40000, "CD001", 0);
    uint32_t cluster = mock_fs_first_cluster(&FS, "/iso/import/Linux.iso");
    fx_settle();
    mock_fs_cut_at(&FS, 3, MOCK_TEAR_BOTH); /* mkdir、meta、rename の 3 つ目 */
    iso_rescan_result_t r;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_rescan(&CAT, 0, &r));
    iso_catalog_t c2;
    reboot(&c2);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/Linux.iso"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/11110001/p000"));
    TEST_ASSERT_EQUAL_UINT32(cluster, mock_fs_first_cluster(&FS, "/iso/import/Linux.iso"));
    TEST_ASSERT_EQUAL_UINT32(cluster, mock_fs_first_cluster(&FS, "/iso/11110001/p000"));
    TEST_ASSERT_EQUAL_UINT(1, c2.count);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, c2.images[0].state);
    /* 再取り込みはしない(クロスリンクとして skipped。同名でもある) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_rescan(&c2, 0, &r));
    TEST_ASSERT_EQUAL_UINT32(0, r.imported);
    TEST_ASSERT_EQUAL_UINT32(1, r.skipped);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/Linux.iso"));
    /* 削除は、共有しているクラスタを解放しない: 拒否し、何も消さない */
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, iso_catalog_delete_precheck(&c2, c2.images[0].id));
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, c2.images[0].id));
    iso_step_result_t st = iso_catalog_delete_step(&c2, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, st.kind);
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, st.err);
    TEST_ASSERT_EQUAL_INT(0, FS.mut_count);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/11110001/p000"));
    /* PC のチェックディスクで、どちらかの名前が消える。import/ 側が消えたら、削除できる */
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/import/Linux.iso"));
    delete_everything_and_check_clean(&c2);
}

/* begin の電源断: mkdir、meta.a のそれぞれ。どれも meta_corrupt として削除できる、または何も残らない */
void test_power_cut_during_begin(void) {
    for (int n = 1; n <= 2; n++) {
        for (int t = 0; t < 3; t++) {
            mock_fs_free(&FS);
            fx_setup();
            fx_settle();
            mock_fs_cut_at(&FS, n, (mock_tear_t)t);
            char id[9];
            char msg[100];
            snprintf(msg, sizeof(msg), "begin: cut at write %d, tear %s", n, TEAR_NAME[t]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_ERR_IO, iso_catalog_begin(&CAT, "A", 1, 100000, NULL, id), msg);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0, CAT.count, msg); /* 失敗した begin はカタログに入らない */
            iso_catalog_t c2;
            reboot(&c2);
            if (n == 1 && t != MOCK_TEAR_FULL) {
                TEST_ASSERT_EQUAL_UINT_MESSAGE(0, c2.count, msg);
            } else if (n == 2 && t == MOCK_TEAR_FULL) {
                TEST_ASSERT_EQUAL_UINT_MESSAGE(1, c2.count, msg);
                TEST_ASSERT_TRUE_MESSAGE(c2.images[0].meta_valid, msg); /* meta.a は書けていた: incomplete として復帰 */
                TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STATE_INCOMPLETE, c2.images[0].state, msg);
                TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, c2.images[0].received, msg);
            } else {
                TEST_ASSERT_EQUAL_UINT_MESSAGE(1, c2.count, msg);
                TEST_ASSERT_FALSE_MESSAGE(c2.images[0].meta_valid, msg);
            }
            delete_everything_and_check_clean(&c2);
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_scenario_without_a_cut_has_the_expected_write_sequence);
    RUN_TEST(test_power_cut_at_every_write_and_every_tear);
    RUN_TEST(test_double_power_cut_during_delete);
    RUN_TEST(test_power_cut_during_import);
    RUN_TEST(test_power_cut_in_rename_leaves_both_names_and_recovery_keeps_the_shared_cluster);
    RUN_TEST(test_power_cut_during_begin);
    return UNITY_END();
}
