#include "iso_mock_fs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 内部 ---------------------------------------------------------------- */

static int find_node(const mock_fs_t *m, const char *path) {
    for (int i = 0; i < MOCK_MAX_NODES; i++) {
        if (m->nodes[i].used && strcmp(m->nodes[i].path, path) == 0) return i;
    }
    return -1;
}

/* path の親のパス ("/iso/a" -> "/iso"、"/iso" -> "/") */
static void parent_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n == 0) {
        snprintf(out, cap, "/");
    } else {
        snprintf(out, cap, "%.*s", (int)n, path);
    }
}

static const char *base_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool dir_exists(const mock_fs_t *m, const char *path) {
    if (strcmp(path, "/") == 0) return true;
    int i = find_node(m, path);
    return i >= 0 && m->nodes[i].is_dir;
}

static int alloc_node(mock_fs_t *m) {
    for (int i = 0; i < MOCK_MAX_NODES; i++) {
        if (!m->nodes[i].used) {
            memset(&m->nodes[i], 0, sizeof(m->nodes[i]));
            m->nodes[i].used = true;
            m->nodes[i].seq = ++m->seq;
            return i;
        }
    }
    return -1;
}

static void free_node(mock_fs_t *m, int i) {
    free(m->nodes[i].data);
    memset(&m->nodes[i], 0, sizeof(m->nodes[i]));
}

/* 書き込みの記録。b != NULL なら "op a -> b"、n != ULONG_MAX なら "op a len=n"、それ以外は "op a" */
static void log_add(mock_fs_t *m, const char *op, const char *a, const char *b, unsigned long n) {
    if (m->nlog >= MOCK_LOG_MAX) return;
    char *dst = m->log[m->nlog++];
    if (b) {
        snprintf(dst, MOCK_LOG_LEN, "%s %s -> %s", op, a, b);
    } else if (n != ULONG_MAX) {
        snprintf(dst, MOCK_LOG_LEN, "%s %s len=%lu", op, a, n);
    } else {
        snprintf(dst, MOCK_LOG_LEN, "%s %s", op, a);
    }
}

/* 書き込みの入口。電源断の判定。戻り値: 0 = 通常に実行、1 = この書き込みで電源が落ちる(tear に従う)、2 = この書き込みだけ err_code で失敗(何も起きない。電源は落ちない)、負 = すでに電源が落ちている */
static int begin_mutation(mock_fs_t *m) {
    if (m->dead) return -1;
    m->mut_count++;
    if (m->err_at > 0 && m->mut_count == m->err_at) return 2;
    if (m->cut_at > 0 && m->mut_count == m->cut_at) {
        m->dead = true;
        return 1;
    }
    return 0;
}

static bool read_fails(const mock_fs_t *m, const char *path) {
    return m->dead || (m->fail_path_set && strcmp(m->fail_path, path) == 0);
}

/* ---- iso_fs_t ------------------------------------------------------------ */

static int fs_mount_info(void *ctx, iso_mount_info_t *out) {
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->fat32 = true;
    out->cluster_bytes = 4096;
    return ISO_OK;
}

static int fs_mkdir(void *ctx, const char *path) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    int cut = begin_mutation(m);
    if (cut < 0) return ISO_ERR_IO;
    log_add(m, "mkdir", path, NULL, ULONG_MAX);
    if (cut == 2) return m->err_code;
    if (cut == 1 && m->tear != MOCK_TEAR_FULL) return ISO_ERR_IO;
    char parent[ISO_PATH_MAX];
    parent_of(path, parent, sizeof(parent));
    if (!dir_exists(m, parent)) return cut ? ISO_ERR_IO : ISO_ERR_NOT_FOUND;
    if (find_node(m, path) >= 0 || strcmp(path, "/") == 0) return cut ? ISO_ERR_IO : ISO_ERR_EXISTS;
    int i = alloc_node(m);
    if (i < 0) return ISO_ERR_NO_SPACE;
    snprintf(m->nodes[i].path, sizeof(m->nodes[i].path), "%s", path);
    m->nodes[i].is_dir = true;
    return cut ? ISO_ERR_IO : ISO_OK;
}

static int fs_remove(void *ctx, const char *path) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    int cut = begin_mutation(m);
    if (cut < 0) return ISO_ERR_IO;
    log_add(m, "remove", path, NULL, ULONG_MAX);
    if (cut == 2) return m->err_code;
    if (cut == 1 && m->tear != MOCK_TEAR_FULL) return ISO_ERR_IO;
    int i = find_node(m, path);
    if (i < 0) return cut ? ISO_ERR_IO : ISO_ERR_NOT_FOUND;
    if (m->nodes[i].is_dir && mock_fs_child_count(m, path) > 0) return cut ? ISO_ERR_IO : ISO_ERR_INVALID;
    free_node(m, i);
    return cut ? ISO_ERR_IO : ISO_OK;
}

static int fs_rename(void *ctx, const char *from, const char *to) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    int cut = begin_mutation(m);
    if (cut < 0) return ISO_ERR_IO;
    log_add(m, "rename", from, to, ULONG_MAX);
    if (cut == 2) return m->err_code;
    if (cut == 1 && m->tear == MOCK_TEAR_BOTH) {
        int si = find_node(m, from);
        char parent_to[ISO_PATH_MAX];
        parent_of(to, parent_to, sizeof(parent_to));
        if (si >= 0 && !m->nodes[si].is_dir && find_node(m, to) < 0 && dir_exists(m, parent_to)) {
            int di = alloc_node(m);
            if (di >= 0) {
                snprintf(m->nodes[di].path, sizeof(m->nodes[di].path), "%s", to);
                m->nodes[di].data = (uint8_t *)malloc(m->nodes[si].data_len ? m->nodes[si].data_len : 1);
                memcpy(m->nodes[di].data, m->nodes[si].data, m->nodes[si].data_len);
                m->nodes[di].data_len = m->nodes[si].data_len;
                m->nodes[di].size = m->nodes[si].size;
                m->nodes[di].first_cluster = m->nodes[si].first_cluster;
            }
        }
        return ISO_ERR_IO;
    }
    if (cut == 1 && m->tear != MOCK_TEAR_FULL) return ISO_ERR_IO;
    int i = find_node(m, from);
    if (i < 0) return cut ? ISO_ERR_IO : ISO_ERR_NOT_FOUND;
    if (m->nodes[i].is_dir) return cut ? ISO_ERR_IO : ISO_ERR_INVALID;
    if (find_node(m, to) >= 0) return cut ? ISO_ERR_IO : ISO_ERR_EXISTS;
    char parent[ISO_PATH_MAX];
    parent_of(to, parent, sizeof(parent));
    if (!dir_exists(m, parent)) return cut ? ISO_ERR_IO : ISO_ERR_NOT_FOUND;
    snprintf(m->nodes[i].path, sizeof(m->nodes[i].path), "%s", to);
    m->nodes[i].seq = ++m->seq;
    return cut ? ISO_ERR_IO : ISO_OK;
}

static int fs_stat(void *ctx, const char *path, iso_stat_t *out) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    if (read_fails(m, path)) return ISO_ERR_IO;
    memset(out, 0, sizeof(*out));
    if (strcmp(path, "/") == 0) {
        out->is_dir = true;
        return ISO_OK;
    }
    int i = find_node(m, path);
    if (i < 0) return ISO_ERR_NOT_FOUND;
    out->is_dir = m->nodes[i].is_dir;
    out->size = m->nodes[i].size;
    out->first_cluster = m->nodes[i].first_cluster;
    return ISO_OK;
}

static int fs_dir_open(void *ctx, const char *path, iso_dir_t **out) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    if (read_fails(m, path)) return ISO_ERR_IO;
    if (!dir_exists(m, path)) return ISO_ERR_NOT_FOUND;
    for (int h = 0; h < MOCK_MAX_HANDLES; h++) {
        if (m->dirs[h].used) continue;
        mock_dir_t *d = &m->dirs[h];
        memset(d, 0, sizeof(*d));
        d->used = true;
        d->fail_next = m->fail_dir_next_set && strcmp(m->fail_dir_next_path, path) == 0;
        /* 子を、seq の順に */
        int order[MOCK_MAX_NODES];
        int n = 0;
        for (int i = 0; i < MOCK_MAX_NODES; i++) {
            char parent[ISO_PATH_MAX];
            if (!m->nodes[i].used) continue;
            parent_of(m->nodes[i].path, parent, sizeof(parent));
            if (strcmp(parent, path) == 0) order[n++] = i;
        }
        for (int a = 1; a < n; a++) {
            int v = order[a], b = a - 1;
            while (b >= 0 && m->nodes[order[b]].seq > m->nodes[v].seq) {
                order[b + 1] = order[b];
                b--;
            }
            order[b + 1] = v;
        }
        memcpy(d->idx, order, sizeof(int) * (size_t)n);
        d->n = n;
        m->open_dirs++;
        *out = (iso_dir_t *)(void *)d;
        return ISO_OK;
    }
    return ISO_ERR_NO_SPACE;
}

static int fs_dir_next(void *ctx, iso_dir_t *dir, char *name, iso_stat_t *st) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    mock_dir_t *d = (mock_dir_t *)(void *)dir;
    if (m->dead || d->fail_next) return ISO_ERR_IO;
    if (d->pos >= d->n) return 1;
    const mock_node_t *n = &m->nodes[d->idx[d->pos++]];
    snprintf(name, ISO_NAME_MAX + 1, "%s", base_of(n->path));
    memset(st, 0, sizeof(*st));
    st->is_dir = n->is_dir;
    st->size = n->size;
    st->first_cluster = n->first_cluster;
    return ISO_OK;
}

static void fs_dir_close(void *ctx, iso_dir_t *dir) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    mock_dir_t *d = (mock_dir_t *)(void *)dir;
    if (d->used) {
        d->used = false;
        m->open_dirs--;
    }
}

static int fs_overwrite(void *ctx, const char *path, const void *data, size_t len) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    int cut = begin_mutation(m);
    if (cut < 0) return ISO_ERR_IO;
    log_add(m, "overwrite", path, NULL, (unsigned long)len);
    if (cut == 2) return m->err_code;
    if (cut == 1 && m->tear == MOCK_TEAR_NONE) return ISO_ERR_IO;
    char parent[ISO_PATH_MAX];
    parent_of(path, parent, sizeof(parent));
    if (!dir_exists(m, parent)) return cut ? ISO_ERR_IO : ISO_ERR_NOT_FOUND;
    int i = find_node(m, path);
    if (i >= 0 && m->nodes[i].is_dir) return cut ? ISO_ERR_IO : ISO_ERR_INVALID;
    if (i < 0) {
        i = alloc_node(m);
        if (i < 0) return ISO_ERR_NO_SPACE;
        snprintf(m->nodes[i].path, sizeof(m->nodes[i].path), "%s", path);
        m->nodes[i].first_cluster = ++m->next_cluster;
    }
    size_t n = len;
    if (cut == 1 && m->tear == MOCK_TEAR_HALF) n = len / 2;
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
    memcpy(buf, data, n);
    free(m->nodes[i].data);
    m->nodes[i].data = buf;
    m->nodes[i].data_len = n;
    m->nodes[i].size = n;
    return cut ? ISO_ERR_IO : ISO_OK;
}

static int fs_open_for_read(void *ctx, const char *path, iso_file_t **out) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    if (read_fails(m, path)) return ISO_ERR_IO;
    if (m->fail_open_set && strcmp(m->fail_open_path, path) == 0) return ISO_ERR_IO;
    int i = find_node(m, path);
    if (i < 0) return ISO_ERR_NOT_FOUND;
    if (m->nodes[i].is_dir) return ISO_ERR_INVALID;
    for (int h = 0; h < MOCK_MAX_HANDLES; h++) {
        if (m->files[h].used) continue;
        m->files[h].used = true;
        m->files[h].node = i;
        m->files[h].pos = 0;
        m->open_files++;
        *out = (iso_file_t *)(void *)&m->files[h];
        return ISO_OK;
    }
    return ISO_ERR_NO_SPACE;
}

static int fs_read(void *ctx, iso_file_t *f, void *buf, size_t len, size_t *got) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    mock_file_t *h = (mock_file_t *)(void *)f;
    if (m->dead) return ISO_ERR_IO;
    const mock_node_t *n = &m->nodes[h->node];
    if (m->fail_path_set && strcmp(m->fail_path, n->path) == 0) return ISO_ERR_IO;
    if (m->fail_read_set && strcmp(m->fail_read_path, n->path) == 0) {
        m->fail_read_calls++;
        if (m->fail_read_nth == 0 || m->fail_read_calls == m->fail_read_nth) return ISO_ERR_IO;
    }
    size_t k = 0;
    uint8_t *out = (uint8_t *)buf;
    while (k < len && h->pos < n->size) {
        out[k] = (h->pos < n->data_len) ? n->data[h->pos] : 0;
        k++;
        h->pos++;
    }
    *got = k;
    return ISO_OK;
}

static int fs_close(void *ctx, iso_file_t *f) {
    mock_fs_t *m = (mock_fs_t *)ctx;
    mock_file_t *h = (mock_file_t *)(void *)f;
    if (h->used) {
        h->used = false;
        m->open_files--;
    }
    return ISO_OK;
}

void mock_fs_init(mock_fs_t *m) {
    memset(m, 0, sizeof(*m));
    m->next_cluster = 1000;
}

void mock_fs_free(mock_fs_t *m) {
    for (int i = 0; i < MOCK_MAX_NODES; i++) free(m->nodes[i].data);
    memset(m, 0, sizeof(*m));
}

iso_fs_t mock_fs_iface(mock_fs_t *m) {
    iso_fs_t f;
    memset(&f, 0, sizeof(f));
    f.ctx = m;
    f.mount_info = fs_mount_info;
    f.mkdir = fs_mkdir;
    f.remove = fs_remove;
    f.rename = fs_rename;
    f.stat = fs_stat;
    f.dir_open = fs_dir_open;
    f.dir_next = fs_dir_next;
    f.dir_close = fs_dir_close;
    f.overwrite = fs_overwrite;
    f.open_for_read = fs_open_for_read;
    f.read = fs_read;
    f.close = fs_close;
    return f;
}

/* ---- 準備 ---------------------------------------------------------------- */

int mock_fs_add_dir(mock_fs_t *m, const char *path) {
    if (find_node(m, path) >= 0) return ISO_ERR_EXISTS;
    int i = alloc_node(m);
    if (i < 0) return ISO_ERR_NO_SPACE;
    snprintf(m->nodes[i].path, sizeof(m->nodes[i].path), "%s", path);
    m->nodes[i].is_dir = true;
    return ISO_OK;
}

int mock_fs_add_sparse(mock_fs_t *m, const char *path, uint64_t size, const void *head, size_t head_len, uint32_t first_cluster) {
    if (find_node(m, path) >= 0) return ISO_ERR_EXISTS;
    int i = alloc_node(m);
    if (i < 0) return ISO_ERR_NO_SPACE;
    snprintf(m->nodes[i].path, sizeof(m->nodes[i].path), "%s", path);
    if ((uint64_t)head_len > size) head_len = (size_t)size;
    m->nodes[i].data = (uint8_t *)malloc(head_len ? head_len : 1);
    if (head_len) memcpy(m->nodes[i].data, head, head_len);
    m->nodes[i].data_len = head_len;
    m->nodes[i].size = size;
    m->nodes[i].first_cluster = first_cluster ? first_cluster : ++m->next_cluster;
    return ISO_OK;
}

int mock_fs_add_file(mock_fs_t *m, const char *path, const void *data, size_t len, uint32_t first_cluster) {
    return mock_fs_add_sparse(m, path, len, data, len, first_cluster);
}

/* ---- 検査 ---------------------------------------------------------------- */

bool mock_fs_exists(const mock_fs_t *m, const char *path) { return find_node(m, path) >= 0; }

bool mock_fs_is_dir(const mock_fs_t *m, const char *path) {
    int i = find_node(m, path);
    return i >= 0 && m->nodes[i].is_dir;
}

int64_t mock_fs_size(const mock_fs_t *m, const char *path) {
    int i = find_node(m, path);
    return i < 0 ? -1 : (int64_t)m->nodes[i].size;
}

const uint8_t *mock_fs_data(const mock_fs_t *m, const char *path, size_t *len) {
    int i = find_node(m, path);
    if (i < 0) return NULL;
    if (len) *len = m->nodes[i].data_len;
    return m->nodes[i].data;
}

uint32_t mock_fs_first_cluster(const mock_fs_t *m, const char *path) {
    int i = find_node(m, path);
    return i < 0 ? 0 : m->nodes[i].first_cluster;
}

int mock_fs_child_count(const mock_fs_t *m, const char *dir) {
    int n = 0;
    for (int i = 0; i < MOCK_MAX_NODES; i++) {
        char parent[ISO_PATH_MAX];
        if (!m->nodes[i].used) continue;
        parent_of(m->nodes[i].path, parent, sizeof(parent));
        if (strcmp(parent, dir) == 0) n++;
    }
    return n;
}

int mock_fs_children(const mock_fs_t *m, const char *dir, char names[][ISO_NAME_MAX + 1], int max) {
    int order[MOCK_MAX_NODES];
    int n = 0;
    for (int i = 0; i < MOCK_MAX_NODES; i++) {
        char parent[ISO_PATH_MAX];
        if (!m->nodes[i].used) continue;
        parent_of(m->nodes[i].path, parent, sizeof(parent));
        if (strcmp(parent, dir) == 0) order[n++] = i;
    }
    for (int a = 1; a < n; a++) {
        int v = order[a], b = a - 1;
        while (b >= 0 && m->nodes[order[b]].seq > m->nodes[v].seq) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = v;
    }
    int out = 0;
    for (int a = 0; a < n && out < max; a++) snprintf(names[out++], ISO_NAME_MAX + 1, "%s", base_of(m->nodes[order[a]].path));
    return out;
}

bool mock_fs_poke(mock_fs_t *m, const char *path, size_t offset, uint8_t value) {
    int i = find_node(m, path);
    if (i < 0 || offset >= m->nodes[i].data_len) return false;
    m->nodes[i].data[offset] = value;
    return true;
}

bool mock_fs_set_first_cluster(mock_fs_t *m, const char *path, uint32_t first_cluster) {
    int i = find_node(m, path);
    if (i < 0) return false;
    m->nodes[i].first_cluster = first_cluster;
    return true;
}

bool mock_fs_set_size(mock_fs_t *m, const char *path, uint64_t size) {
    int i = find_node(m, path);
    if (i < 0) return false;
    if (size < m->nodes[i].data_len) m->nodes[i].data_len = (size_t)size;
    m->nodes[i].size = size;
    return true;
}

/* ---- 電源断、失敗、記録 ----------------------------------------------------- */

void mock_fs_cut_at(mock_fs_t *m, int n, mock_tear_t tear) {
    m->mut_count = 0;
    m->cut_at = n;
    m->tear = tear;
    m->dead = false;
}

void mock_fs_error_at(mock_fs_t *m, int n, int err_code) {
    m->mut_count = 0;
    m->err_at = n;
    m->err_code = err_code;
}

void mock_fs_power_on(mock_fs_t *m) {
    m->dead = false;
    m->cut_at = 0;
}

void mock_fs_reset_counter(mock_fs_t *m) {
    m->mut_count = 0;
    m->err_at = 0;
}

void mock_fs_fail_path(mock_fs_t *m, const char *path_or_null) {
    m->fail_path_set = path_or_null != NULL;
    if (path_or_null) snprintf(m->fail_path, sizeof(m->fail_path), "%s", path_or_null);
}

void mock_fs_fail_open(mock_fs_t *m, const char *path_or_null) {
    m->fail_open_set = path_or_null != NULL;
    if (path_or_null) snprintf(m->fail_open_path, sizeof(m->fail_open_path), "%s", path_or_null);
}

void mock_fs_fail_read(mock_fs_t *m, const char *path_or_null, int nth) {
    m->fail_read_set = path_or_null != NULL;
    m->fail_read_nth = nth;
    m->fail_read_calls = 0;
    if (path_or_null) snprintf(m->fail_read_path, sizeof(m->fail_read_path), "%s", path_or_null);
}

void mock_fs_fail_dir_next(mock_fs_t *m, const char *dir_or_null) {
    m->fail_dir_next_set = dir_or_null != NULL;
    if (dir_or_null) snprintf(m->fail_dir_next_path, sizeof(m->fail_dir_next_path), "%s", dir_or_null);
}

void mock_fs_clear_log(mock_fs_t *m) { m->nlog = 0; }

bool mock_fs_log_is(const mock_fs_t *m, int i, const char *s) { return i >= 0 && i < m->nlog && strcmp(m->log[i], s) == 0; }

/* ---- 乱数 ---------------------------------------------------------------- */

static uint32_t rnd_next(void *ctx) {
    mock_rnd_t *r = (mock_rnd_t *)ctx;
    r->calls++;
    if (r->n == 0) return 0;
    uint32_t v = r->seq[r->idx];
    if (r->idx + 1 < r->n) r->idx++;
    return v;
}

void mock_rnd_set(mock_rnd_t *r, const uint32_t *seq, int n) {
    memset(r, 0, sizeof(*r));
    if (n > 64) n = 64;
    memcpy(r->seq, seq, sizeof(uint32_t) * (size_t)n);
    r->n = n;
}

iso_random_t mock_rnd_iface(mock_rnd_t *r) {
    iso_random_t x;
    x.ctx = r;
    x.next = rnd_next;
    return x;
}
