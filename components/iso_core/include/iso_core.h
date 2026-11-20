#ifndef ISO_CORE_H
#define ISO_CORE_H

/*
 * iso_core: ワーカーのジョブの契約(キュー、優先順位、合流、ステップ)、公開ビューの更新、アイドルのタイムアウト、SD の再初期化。
 * 純 C、IDF 非依存。**ロックを持たない、スレッドを作らない。待たない(イベント・時間・ロックを待たない)が、シームの I/O(`write` / `sync` / `read` など)が終わるまでは、呼び出し側が待つ**(**1 回のステップは、複数の同期 I/O を順に行うことがある**: 例えば IDLE_CLOSE は、パーツの sync と close の後に meta を書く。FINISH の 1 ステップは、sync + close、または budget バイトまでの読み出しと SHA-256 の更新、または meta の書き込み。
 * グルーが iso_op_mutex の保持時間を見積もるときは、**ステップ 1 回の中の I/O を、すべて数える**(上限は FatFs の 1 回の呼び出しではない)。設計 4.2)。
 * 契約: docs/ISO_MOUNT_DESIGN.md 3.4(状態の遷移、SD の再初期化の手順)、4.1(ロック)、4.2(ワーカー)、docs/INTERFACES.md 7.5。
 *
 * ---- グルー(A-13 の iso_worker、A-15 の HTTP ハンドラー)が守ること -----------------------------------------
 *  ロック(設計 4.1 の表):
 *    - iso_core_* と iso_upload_*(iso_core_upload() が返すもの)、iso_job_step、iso_upload_tick は、**iso_op_mutex を取って**呼ぶ。
 *      ステップごとに取り、ステップの間は放す(DELETE や mount が割り込めるように)。
 *    - hooks.view_update は、iso_op_mutex の下で呼ばれる。実装は iso_state_mutex を取り、**短く、I/O なし、iso_core / iso_upload を呼ばない**。
 *      渡されるビューは、コールバックの間だけ有効。エクステントの表(images[].table)は、**保持するなら、コールバックの中で
 *      iso_extent_table_ref する**(MSC の READ が参照を取る元になる。参照を放すのは mmc_scsi_command_done だけ)。
 *    - マウント中のイメージ(iso_media)の情報は、このビューには入っていない(iso_media が持つ。GET は両方を iso_state_mutex の下で組み立てる)。
 *  ワーカーのループ:
 *      for (;;) {                                  // イベント待ちは、グルーの仕事
 *          lock(op); job = iso_core_pick(core); unlock(op);
 *          if (!job) { wait; continue; }
 *          do { lock(op); r = iso_job_step(core, job, budget); waiting = iso_job_waiting(job); unlock(op);
 *               if (waiting) delay(短く); } while (r.kind == ISO_STEP_CONTINUE);
 *          // ERROR(r.err): ログを出す。状態の遷移は iso_job_step が済ませている(FINISH → incomplete、DELETE → deleting のまま、INDEX → 表なしの ready)
 *      }
 *    iso_upload_tick(1 秒ごと、iso_op_mutex の下)がアイドルのタイムアウトで IDLE_CLOSE を積む(積んだ後、実行の前に PUT が成功すると、取り消される)。
 *  SD の再初期化(設計 3.4 末尾の ①〜⑦。**呼び出し順の契約**):
 *    ① グルーが **iso_state_mutex だけを取り、1 つの区間で**、(a) sd.state を ready 以外にして公開、(b) iso_media_force_eject()、
 *       (c) iso_blockdev_t.epoch_bump() を行う。放してから、
 *    ②〜⑦ iso_op_mutex を取って iso_core_sd_reinit() を呼ぶ(中で: ② ジョブの全取り消し → ③ 開いているファイルを捨てる →
 *       ④ アップロードを incomplete にして SHA-256 の状態を捨てる → ⑤ remount コールバック(f_mount。グルー)→ ⑥ カタログの再構築 →
 *       ⑦ ready のイメージを INDEX の集合へ、deleting のイメージの DELETE を積む)。自動ではマウントしない。
 *    成功したら、グルーが sd.state を ready に戻す(hooks.view_update が iso_core_sd_state() を使う)。
 *  起動時: iso_core_boot()(⑥⑦と同じ)。
 * ------------------------------------------------------------------------------------------------------------------
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "iso_seams.h"
#include "iso_extent.h"
#include "iso_catalog.h"
#include "iso_upload.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ISO_WORKER_QUEUE
#define ISO_WORKER_QUEUE 8u /* Kconfig ISO_WORKER_QUEUE */
#endif
#ifndef ISO_DELETE_REF_WAIT_S
#define ISO_DELETE_REF_WAIT_S 30u /* Kconfig ISO_DELETE_REF_WAIT_S */
#endif

/* ---- ジョブ -------------------------------------------------------------------------- */

typedef enum {
    ISO_JOB_NONE = 0,
    ISO_JOB_DELETE,      /* 優先順位 1(キューの先頭に割り込む。動いている同じイメージのジョブに中止の印) */
    ISO_JOB_FINISH,      /* 2 */
    ISO_JOB_IDLE_CLOSE,  /* 3 */
    ISO_JOB_INDEX        /* 4(キューに積まない。INDEX が必要なイメージの集合から、ワーカーが空いたとき 1 つずつ選ぶ) */
} iso_job_kind_t;

typedef struct iso_job {
    iso_job_kind_t kind;
    char id[ISO_ID_LEN + 1];
    bool aborted;   /* 中止の印(DELETE が立てる)。ステップの先頭で見る */
    bool waiting;   /* DELETE: エクステントの表の参照を待っている(ワーカーは、短く待ってから、ステップを続ける) */
    iso_finish_job_t fin;
    iso_index_job_t idx;
    struct {
        iso_delete_job_t dj;
        uint8_t phase;
        bool wait_started;
        uint64_t wait_start_ms;
    } del;
} iso_job_t;

/* ---- 公開ビュー ---------------------------------------------------------------------- */

typedef struct {
    char id[ISO_ID_LEN + 1];
    iso_state_t state;           /* 表示の状態。ready でエクステントの表がまだない(INDEX 待ち・中)ものは ISO_STATE_FINALIZING */
    uint8_t invalid_reason;      /* state == INVALID のとき: 1..4、または ISO_INVALID_META_CORRUPT */
    bool meta_valid;             /* false = meta_corrupt(name / size は無意味) */
    uint8_t name_len;
    char name[ISO_NAME_MAX_BYTES + 1];
    uint64_t size_bytes;
    uint64_t received;
    bool has_sha;                /* ready で、SHA-256 の計算値(フラグ 2)がある */
    uint8_t sha256[ISO_SHA256_LEN];
    iso_extent_table_t *table;   /* ready でインデックス済みのとき。コールバックの間だけ有効(保持するなら ref する) */
} iso_view_image_t;

typedef struct {
    iso_sd_state_t sd_state;     /* iso_sd_status_t から。グルーが ① で別に持つ値を優先してよい */
    bool free_known;
    uint64_t total_bytes;
    uint64_t free_bytes;         /* キャッシュ値(マウント時と、FINISH / DELETE の完了だけが更新) */
    uint32_t hidden;
    uint32_t chunk_bytes;
    size_t count;
    iso_view_image_t images[ISO_MAX_IMAGES];
    bool upload_active;          /* 枠を使っている(uploading または FINISH 中の finalizing) */
    char upload_id[ISO_ID_LEN + 1];
    iso_upload_activity_t upload_state; /* RECEIVING / IDLE / FINALIZING(枠が空いていれば NONE) */
} iso_view_t;

typedef struct {
    void *ctx;
    /* 公開ビューの更新(上の規則)。NULL でよい。 */
    void (*view_update)(void *ctx, const iso_view_t *view);
    /* id のイメージが、いまマウント中か(iso_media。グルーが iso_state_mutex の下で調べる。短く、I/O なし)。NULL なら、マウントなし。 */
    bool (*is_mounted)(void *ctx, const char *id);
} iso_core_hooks_t;

typedef struct {
    iso_catalog_t *cat;
    const iso_blockdev_t *bdev;
    const iso_clock_t *clock;
    const iso_sha256_t *sha;
    void *sha_state;
    uint8_t *io_buf;
    size_t io_buf_size;
    const iso_allocator_t *alloc;
    const iso_sd_status_t *sd;   /* NULL なら sd_state は NO_CARD */
    uint32_t chunk_bytes;        /* 0 = 既定(iso_upload_cfg_t と同じ) */
    uint32_t sync_bytes;         /* 0 = 既定 */
    iso_core_hooks_t hooks;
} iso_core_cfg_t;

typedef struct {
    iso_core_cfg_t cfg;
    iso_upload_t up;
    struct {
        iso_job_kind_t kind;
        char id[ISO_ID_LEN + 1];
    } queue[ISO_WORKER_QUEUE];
    size_t qn;
    char index_ids[ISO_MAX_IMAGES][ISO_ID_LEN + 1]; /* INDEX が必要なイメージの集合(先に入れたものから) */
    size_t index_n;
    iso_job_t cur;
    bool cur_active;
    struct {
        char id[ISO_ID_LEN + 1];
        iso_extent_table_t *table;                  /* カタログの分の参照(1) */
    } tables[ISO_MAX_IMAGES];
    size_t table_n;
    iso_view_t view;
    iso_upload_activity_t pub_act;
} iso_core_t;

/* cfg を検査して iso_upload を初期化する。ISO_ERR_INVALID / iso_upload_init のエラー。 */
int iso_core_init(iso_core_t *c, const iso_core_cfg_t *cfg);
/* begin / write_chunk / info / set_free などの窓口 */
iso_upload_t *iso_core_upload(iso_core_t *c);

/* ---- 起動、SD の再初期化 ---------------------------------------------------------------- */

/* 起動時: ジョブと表を空にし、カタログを作り直し(iso_catalog_scan)、free_bytes のキャッシュを採り、ready のイメージを INDEX の集合へ、
 * deleting のものの DELETE をキューへ入れる(キューがあふれた分は、deleting のまま。再度の DELETE か次の起動で続ける)。ビューを更新する。 */
int iso_core_boot(iso_core_t *c);
/* SD の再初期化の ②〜⑦(上の契約)。remount は ⑤(NULL なら何もしない)。失敗(remount、scan)したら、そのエラーを返す
 * (カタログは空、ジョブなし。グルーは sd.state を error にして、後でもう一度呼ぶ)。 */
int iso_core_sd_reinit(iso_core_t *c, int (*remount)(void *ctx), void *remount_ctx);

/* ---- 投入 -------------------------------------------------------------------------- */

/*
 * FINISH の投入(WEB_API 8.4): iso_upload_finish_begin の結果をそのまま返す(ISO_RES_NOT_FOUND / ALREADY_READY / INVALID_IMAGE / SIZE_MISMATCH /
 *   UPLOAD_IN_PROGRESS / 負の I/O エラー)。受理(ISO_RES_OK)なら、状態は finalizing になり、FINISH がキューに入る(待っている IDLE_CLOSE は取り除く)。
 * ISO_RES_ALREADY_FINALIZING で、FINISH のジョブがある: ISO_RES_JOINED(冪等)。ジョブがなければ、積み直す。
 * **キューがあふれたら ISO_ERR_BUSY**(状態は変えない = 受付を取り消す)。
 */
int iso_core_submit_finish(iso_core_t *c, const char *id, uint64_t *out_received);
/*
 * DELETE の投入(WEB_API 8.7): イメージがない(deleting でジョブがあるものは、冪等に ISO_RES_JOINED)→ ISO_RES_NOT_FOUND、マウント中 → ISO_RES_MOUNTED、
 * キューがあふれたら ISO_ERR_BUSY(何も変えない)。受理なら、状態は deleting、uploading / finalizing の枠は空き(ファイルを閉じ、SHA-256 を捨てる)、
 * 同じイメージの待っている FINISH / IDLE_CLOSE / INDEX は取り消され、動いているジョブには中止の印が立つ。DELETE はキューの先頭の優先順位で入る。
 */
int iso_core_submit_delete(iso_core_t *c, const char *id);
/*
 * 汎用の投入(設計 4.2 の 4 種類): FINISH / DELETE は上と同じ。IDLE_CLOSE: 同じイメージのジョブがあれば合流(ISO_RES_JOINED)、なければ積む。
 * INDEX: イメージが ready なら集合に入れる(すでにあれば JOINED)。ready でなければ ISO_RES_NOT_FOUND / ISO_RES_NOT_UPLOADING。
 * あふれたら ISO_ERR_BUSY。
 */
int iso_core_submit(iso_core_t *c, iso_job_kind_t kind, const char *id);
/* ready でエクステントの表がなく、集合にも、動いているジョブにもないものを、集合に入れる(rescan の後に呼ぶ)。 */
void iso_core_mark_unindexed(iso_core_t *c);

/* ---- ワーカー ------------------------------------------------------------------------ */

/*
 * 次のジョブを選ぶ(優先順位: DELETE、FINISH、IDLE_CLOSE、INDEX。同じ種類は、入れた順)。ジョブが動いている間は、そのジョブを返す。
 * 何もなければ NULL。返したジョブは、iso_job_step が DONE / ERROR を返すまで有効。
 */
iso_job_t *iso_core_pick(iso_core_t *c);
/*
 * ジョブを 1 ステップ進める(budget_bytes: FINISH の SHA-256 の再計算の読み出しの上限。0 = 1 回)。
 *   CONTINUE: 続きがある(DELETE で job->waiting が真なら、参照の解放待ち。ワーカーは短く待つ)。
 *   DONE: 完了(中止されたもの、対象がなくなったものを含む)。
 *   ERROR(err): FINISH = incomplete(I/O エラー。received は実ファイルから)、INDEX = I/O エラーなら表なしの ready のまま / meta の書き込み失敗、
 *     DELETE = deleting のまま(I/O エラー、または参照の待ちが ISO_DELETE_REF_WAIT_S を超えた = ISO_ERR_BUSY)、IDLE_CLOSE = incomplete(meta の書き込みなどの失敗)。
 * 中止の印は、ステップの先頭で見る。DONE / ERROR で、ジョブは終わる(次の pick は別のジョブ)。ビューは、状態が変わったとき(iso_upload の遷移、submit、ジョブの終わり)に更新する。
 */
iso_step_result_t iso_job_step(iso_core_t *c, iso_job_t *job, uint32_t budget_bytes);
bool iso_job_waiting(const iso_job_t *job);

/* ---- アイドルのタイムアウト ----------------------------------------------------------------- */

/*
 * 1 秒ごとに呼ぶ。uploading の枠が ISO_UPLOAD_IDLE_S 秒(経過 >= 300 秒)PUT なしなら IDLE_CLOSE を投入する(キューがあふれていたら、次の tick で再試行)。
 * 時刻の巻き戻り(now_ms < 直近の PUT の時刻)は「経過 0」に倒し、基準を now_ms に引き直す。upload.state の receiving / idle が変わったら、ビューを更新する。
 * finalizing の枠は、タイムアウトしない。
 */
void iso_upload_tick(iso_core_t *c, uint64_t now_ms);

/* ---- 問い合わせ ----------------------------------------------------------------------- */

/* カタログが持つ、エクステントの表(参照は取らない。iso_op_mutex の下で、使う間だけ)。なければ NULL。 */
iso_extent_table_t *iso_core_table(const iso_core_t *c, const char *id);
size_t iso_core_queue_len(const iso_core_t *c);
size_t iso_core_index_pending(const iso_core_t *c);
/* ビューを組み立てて、hooks.view_update を呼ぶ(操作の最後に、core が自分で呼ぶ。グルーが sd.state を変えたときにも呼べる)。 */
void iso_core_publish(iso_core_t *c);
const iso_view_t *iso_core_view(const iso_core_t *c);

#ifdef __cplusplus
}
#endif

#endif /* ISO_CORE_H */
