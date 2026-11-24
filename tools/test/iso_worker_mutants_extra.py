# iso_worker_logic.c の、手書きのミュータント(C-15)。tools/test/gen_iso_mutants.py は、次の形を作らないので、ここで足す:
# 呼び出しの引数を別の値にする(budget、remount_ctx、wait_ms、now_ms)、`const bool x = (式);` の中の比較と論理演算、`has_pending_core_work` の項を落とす、
# iso_core_cfg_t へ渡す欄を 0 / NULL にする、deinit の remount の戻り値。iso_worker_mutants.py(機械が作る)とは別に、同じスクリプトで実行する:
#   python3 tools/test/mutate_iso_worker.py <repo> <workdir> tools/test/iso_worker_mutants_extra.py <out.json> -j3
WL = "components/iso_worker/iso_worker_logic.c"
INC = '#include "iso_worker_logic.h"'
PRAGMAS = '\n#pragma GCC diagnostic ignored "-Wparentheses"\n#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n#pragma GCC diagnostic ignored "-Wunused-variable"\n#pragma GCC diagnostic ignored "-Wunused-parameter"\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"\n#pragma GCC diagnostic ignored "-Wuninitialized"\n#pragma GCC diagnostic ignored "-Warray-parameter"\n#pragma GCC diagnostic ignored "-Wstringop-overflow"\n#pragma GCC diagnostic ignored "-Wtype-limits"\n#pragma GCC diagnostic ignored "-Wsign-compare"\n#pragma GCC diagnostic ignored "-Wreturn-type"'


def M(id, desc, old, new, file=WL):
    return {"id": id, "desc": desc, "edits": [(old, new), (INC, INC + PRAGMAS)], "file": file}


MUTANTS = [
    # ---- has_pending_core_work: 項を 1 つずつ落とす ----
    M("X01", "pending: queue_len term dropped", "    return (iso_core_queue_len(&w->core) > 0u) ||", "    return false ||"),
    M("X02", "pending: index_pending term dropped", "           (iso_core_index_pending(&w->core) > 0u) ||", "           false ||"),
    M("X03", "pending: cur_active term dropped", "           w->core.cur_active ||", "           false ||"),
    M("X04", "pending: reinit_pending term dropped", "           atomic_load_explicit(&w->reinit_pending, memory_order_acquire) ||", "           false ||"),
    M("X05", "pending: reprobe_pending term dropped", "           atomic_load_explicit(&w->reprobe_pending, memory_order_acquire);", "           false;"),
    # ---- init: iso_core_cfg_t / iso_catalog_init / iso_media_init へ渡す値 ----
    M("X06", "init: rnd not passed to the catalog", "iso_catalog_init(&w->cat, cfg->fs, cfg->rnd);", "iso_catalog_init(&w->cat, cfg->fs, NULL);"),
    M("X07", "init: media force-eject ctx NULL", "iso_media_init(&w->media, on_media_force_eject_cb, w);", "iso_media_init(&w->media, on_media_force_eject_cb, NULL);"),
    M("X08", "init: core bdev dropped", "        .bdev = cfg->bdev,", "        .bdev = NULL,"),
    M("X09", "init: core sha dropped", "        .sha = cfg->sha,", "        .sha = NULL,"),
    M("X10", "init: core io_buf_size dropped", "        .io_buf_size = cfg->io_buf_size,", "        .io_buf_size = 0u,"),
    M("X11", "init: core sd dropped", "        .sd = cfg->sd,", "        .sd = NULL,"),
    M("X12", "init: core chunk_bytes dropped", "        .chunk_bytes = cfg->chunk_bytes,", "        .chunk_bytes = 0u,"),
    M("X13", "init: core sync_bytes dropped", "        .sync_bytes = cfg->sync_bytes,", "        .sync_bytes = 0u,"),
    M("X14", "init: core hook ctx NULL", "            .ctx = w,", "            .ctx = NULL,"),
    M("X15", "init: core view_update hook dropped", "            .view_update = on_core_view_update_cb,", "            .view_update = NULL,"),
    M("X16", "init: core is_mounted hook dropped", "            .is_mounted = on_core_is_mounted_cb,", "            .is_mounted = NULL,"),
    M("X17", "init: core clock dropped", "        .clock = cfg->clock,", "        .clock = NULL,"),
    # ---- boot ----
    M("X18", "boot: ready check inverted", "(w->cfg.sd->state(w->cfg.sd->ctx) == ISO_SD_READY);", "(w->cfg.sd->state(w->cfg.sd->ctx) != ISO_SD_READY);"),
    M("X19", "boot: NULL sd taken as ready (|| instead of &&)", "const bool ready = (w->cfg.sd != NULL) && (w->cfg.sd->state", "const bool ready = (w->cfg.sd != NULL) || (w->cfg.sd->state"),
    M("X20", "boot: ready without the NULL sd guard", "const bool ready = (w->cfg.sd != NULL) && (w->cfg.sd->state", "const bool ready = (1) && (w->cfg.sd->state"),
    M("X21", "boot: override value NO_CARD instead of ERROR", "            w->sd_override_val = ISO_SD_ERROR;", "            w->sd_override_val = ISO_SD_NO_CARD;"),
    M("X22", "boot: hw sd state NO_CARD instead of ERROR", "        w->cfg.os.set_hw_sd_state(w->cfg.os.ctx, ISO_SD_ERROR);", "        w->cfg.os.set_hw_sd_state(w->cfg.os.ctx, ISO_SD_NO_CARD);"),
    # ---- notify_sd_fault ----
    M("X23", "notify: ternary branches swapped", "(fault_state == ISO_SD_READY) ? ISO_SD_NO_CARD : fault_state;", "(fault_state == ISO_SD_READY) ? fault_state : ISO_SD_NO_CARD;"),
    M("X24", "notify: READY mapped to UNSUPPORTED_FS", "(fault_state == ISO_SD_READY) ? ISO_SD_NO_CARD : fault_state;", "(fault_state == ISO_SD_READY) ? ISO_SD_UNSUPPORTED_FS : fault_state;"),
    M("X25", "notify: hw state gets the raw fault_state", "        w->cfg.os.set_hw_sd_state(w->cfg.os.ctx, non_ready);", "        w->cfg.os.set_hw_sd_state(w->cfg.os.ctx, fault_state);"),
    M("X26", "notify: view callback gets consumed_since 0", "w->cfg.os.on_view_published(w->cfg.os.ctx, &w->pub_view, w->pub_consumed_since);\n    }\n    iso_media_force_eject", "w->cfg.os.on_view_published(w->cfg.os.ctx, &w->pub_view, 0u);\n    }\n    iso_media_force_eject"),
    # ---- run_once ----
    M("X27", "run_once: already_ready ignores the override", "            !w->sd_override_non_ready;", "            true;"),
    M("X28", "run_once: already_ready needs the override", "            !w->sd_override_non_ready;", "            w->sd_override_non_ready;"),
    M("X29", "run_once: already_ready state compare inverted", "(w->cfg.sd->state(w->cfg.sd->ctx) == ISO_SD_READY) &&\n            !w->sd_override", "(w->cfg.sd->state(w->cfg.sd->ctx) != ISO_SD_READY) &&\n            !w->sd_override"),
    M("X30", "run_once: already_ready without the NULL sd guard", "(w->cfg.sd != NULL) && (w->cfg.sd->state(w->cfg.sd->ctx) == ISO_SD_READY) &&\n            !w->sd_override", "(1) && (w->cfg.sd->state(w->cfg.sd->ctx) == ISO_SD_READY) &&\n            !w->sd_override"),
    M("X31", "run_once: remount_ctx not forwarded", "iso_core_sd_reinit(&w->core, remount, remount_ctx);\n        w->cfg.os.state_lock", "iso_core_sd_reinit(&w->core, remount, NULL);\n        w->cfg.os.state_lock"),
    M("X32", "run_once: remount not forwarded", "iso_core_sd_reinit(&w->core, remount, remount_ctx);\n        w->cfg.os.state_lock", "iso_core_sd_reinit(&w->core, NULL, remount_ctx);\n        w->cfg.os.state_lock"),
    M("X33", "run_once: fault_raced always false", "const bool fault_raced = atomic_load_explicit(&w->reinit_pending, memory_order_acquire);", "const bool fault_raced = false;"),
    M("X34", "run_once: fault_raced always true", "const bool fault_raced = atomic_load_explicit(&w->reinit_pending, memory_order_acquire);", "const bool fault_raced = true;"),
    M("X35", "run_once: step budget 0", "iso_job_step(&w->core, job, w->cfg.step_budget_bytes);", "iso_job_step(&w->core, job, 0u);"),
    M("X36", "run_once: step budget 1", "iso_job_step(&w->core, job, w->cfg.step_budget_bytes);", "iso_job_step(&w->core, job, 1u);"),
    M("X37", "run_once: step budget is the raw cfg value", "iso_job_step(&w->core, job, w->cfg.step_budget_bytes);", "iso_job_step(&w->core, job, ISO_WORKER_BUDGET_KIB);"),
    M("X38", "run_once: job id copy loses the NUL", "memcpy(job_id, job->id, sizeof(job_id));", "memcpy(job_id, job->id, sizeof(job_id) - 1u);"),
    M("X39", "run_once: error callback reads the kind after the step", "on_job_error(w->cfg.os.ctx, kind, job_id, r.err);", "on_job_error(w->cfg.os.ctx, job->kind, job_id, r.err);"),
    M("X40", "run_once: error callback reads the id after the step", "on_job_error(w->cfg.os.ctx, kind, job_id, r.err);", "on_job_error(w->cfg.os.ctx, kind, job->id, r.err);"),
    M("X41", "run_once: error callback gets 0 instead of the error", "on_job_error(w->cfg.os.ctx, kind, job_id, r.err);", "on_job_error(w->cfg.os.ctx, kind, job_id, 0);"),
    M("X42", "run_once: waiting always false", "const bool waiting = iso_job_waiting(job);", "const bool waiting = false;"),
    M("X43", "run_once: waiting always true", "const bool waiting = iso_job_waiting(job);", "const bool waiting = true;"),
    # ---- tick / trylock ----
    M("X44", "tick: now_ms not forwarded", "iso_upload_tick(&w->core, now_ms);", "iso_upload_tick(&w->core, 0u);"),
    M("X45", "trylock: wait_ms not forwarded", "return w->cfg.os.op_trylock(w->cfg.os.ctx, wait_ms);", "return w->cfg.os.op_trylock(w->cfg.os.ctx, 0u);"),
    M("X46", "trylock: result inverted", "return w->cfg.os.op_trylock(w->cfg.os.ctx, wait_ms);", "return !w->cfg.os.op_trylock(w->cfg.os.ctx, wait_ms);"),
    # ---- lookups ----
    M("X47", "lookup: returns the id instead of the size", "*out_bytes = w->pub_tables[i].size_bytes;", "*out_bytes = w->pub_tables[i].id_u32;"),
    M("X48", "view hook: slot size from the id", "next_slots[next_n].size_bytes = img->size_bytes;", "next_slots[next_n].size_bytes = id_u32;"),
    M("X49", "view hook: slot id_u32 not stored", "            next_slots[next_n].id_u32 = id_u32;\n", "            next_slots[next_n].id_u32 = 0u;\n"),
    # ---- deinit ----
    M("X50", "deinit: the failing remount succeeds (rescan)", "    (void)ctx;\n    return ISO_ERR_NO_CARD;", "    (void)ctx;\n    return ISO_OK;"),
    M("X51", "deinit: no sd_reinit", "    (void)iso_core_sd_reinit(&w->core, deinit_fail_remount_cb, NULL);\n", "    ;\n"),
    M("X52", "deinit: remount callback dropped", "iso_core_sd_reinit(&w->core, deinit_fail_remount_cb, NULL);", "iso_core_sd_reinit(&w->core, NULL, NULL);"),
]
