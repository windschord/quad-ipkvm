def M(id, desc, *edits, equivalent=None):
    return {"id": id, "desc": desc, "edits": list(edits), "equivalent": equivalent}


# 対象: components/ws_clients/ws_clients.c の、世代番号の追加分(1 か所ずつ変える)。old 文字列は、ファイルの中で 1 回だけ現れること。
# "equivalent" に理由を書いたものは、同値(振る舞いが変わらない)と判断した変更。生き残ることを期待する。
FG_HEAD = "int ws_clients_find_gen(const ws_clients_t *ctx, int fd, uint32_t gen) {\n    if (!ctx) return -1;\n"
FG_LOOP = "    for (int i = 0; i < ctx->max_clients; i++) {\n        if (ctx->clients[i].state != WS_CLIENT_FREE && ctx->clients[i].fd == fd) {\n            return (ctx->clients[i].gen == gen) ? i : -1;"
SG_HEAD = "uint32_t ws_clients_slot_gen(const ws_clients_t *ctx, int slot) {\n"

MUTANTS = [
    # ---- ws_clients_add: the generation ----
    M("G01", "add: the generation is not stored in the slot",
      ("            ctx->clients[i].gen = gen;\n", "")),
    M("G02", "add: the counter is not stored (every connection gets last_gen + 1 = 1)",
      ("            ctx->last_gen = gen;\n", "")),
    M("G03", "add: 0 is not skipped at the wrap-around",
      ("            if (gen == 0) gen = 1;\n", "")),
    M("G04", "add: the wrap-around goes to 2 instead of 1",
      ("            if (gen == 0) gen = 1;\n", "            if (gen == 0) gen = 2;\n")),
    M("G05", "add: the generation is the slot index + 1 (not unique per connection)",
      ("            ctx->clients[i].gen = gen;\n", "            ctx->clients[i].gen = (uint32_t)i + 1;\n")),
    M("G06", "add: the counter is not incremented (gen = last_gen)",
      ("            uint32_t gen = ctx->last_gen + 1;\n", "            uint32_t gen = ctx->last_gen;\n")),
    M("G07", "add: the counter goes up by 2 (first generation is 2)",
      ("            uint32_t gen = ctx->last_gen + 1;\n", "            uint32_t gen = ctx->last_gen + 2;\n")),
    M("G08", "add: a duplicate fd (-2) takes a generation",
      ("            return -2;\n", "            ctx->last_gen++;\n            return -2;\n")),
    M("G09", "add: a full table (-1) takes a generation",
      ("    return -1; // Full\n", "    ctx->last_gen++;\n    return -1; // Full\n")),
    M("G10", "add: the counter is given back when a slot is freed (remove, FREE branch)",
      ("        ctx->clients[slot].state = WS_CLIENT_FREE;\n        ctx->clients[slot].fd = -1;\n    }\n    return 0;\n}\n\nbool ws_clients_try_begin_send",
       "        ctx->clients[slot].state = WS_CLIENT_FREE;\n        ctx->clients[slot].fd = -1;\n        ctx->last_gen--;\n    }\n    return 0;\n}\n\nbool ws_clients_try_begin_send")),
    M("G11", "add: a slot reused after being freed keeps the old connection's generation",
      ("            uint32_t gen = ctx->last_gen + 1;\n            if (gen == 0) gen = 1;\n            ctx->last_gen = gen;\n            ctx->clients[i].gen = gen;\n",
       "            uint32_t gen = ctx->last_gen + 1;\n            if (gen == 0) gen = 1;\n            ctx->last_gen = gen;\n            if (ctx->clients[i].gen == 0) ctx->clients[i].gen = gen;\n")),
    # ---- ws_clients_slot_gen ----
    M("G12", "slot_gen: ctx NULL not checked",
      (SG_HEAD + "    if (!ctx || slot < 0 ||", SG_HEAD + "    if (slot < 0 ||")),
    M("G13", "slot_gen: negative slot not checked",
      ("    if (!ctx || slot < 0 || slot >= ctx->max_clients) return 0;\n    if (ctx->clients[slot].state == WS_CLIENT_FREE) return 0;",
       "    if (!ctx || slot >= ctx->max_clients) return 0;\n    if (ctx->clients[slot].state == WS_CLIENT_FREE) return 0;")),
    M("G14", "slot_gen: slot == max_clients is accepted (>= becomes >)",
      ("    if (!ctx || slot < 0 || slot >= ctx->max_clients) return 0;\n    if (ctx->clients[slot].state == WS_CLIENT_FREE) return 0;",
       "    if (!ctx || slot < 0 || slot > ctx->max_clients) return 0;\n    if (ctx->clients[slot].state == WS_CLIENT_FREE) return 0;")),
    M("G15", "slot_gen: a FREE slot returns its old generation",
      ("    if (ctx->clients[slot].state == WS_CLIENT_FREE) return 0;\n    return ctx->clients[slot].gen;", "    return ctx->clients[slot].gen;")),
    M("G16", "slot_gen: CLOSING slot returns 0 (only ACTIVE has a generation)",
      ("    if (ctx->clients[slot].state == WS_CLIENT_FREE) return 0;\n    return ctx->clients[slot].gen;",
       "    if (ctx->clients[slot].state != WS_CLIENT_ACTIVE) return 0;\n    return ctx->clients[slot].gen;")),
    M("G17", "slot_gen: returns gen + 1",
      ("    return ctx->clients[slot].gen;", "    return ctx->clients[slot].gen + 1;")),
    # ---- ws_clients_find_gen ----
    M("G18", "find_gen: ctx NULL not checked",
      (FG_HEAD, "int ws_clients_find_gen(const ws_clients_t *ctx, int fd, uint32_t gen) {\n")),
    M("G19", "find_gen: the generation is not compared (same as find)",
      ("            return (ctx->clients[i].gen == gen) ? i : -1;", "            return i;")),
    M("G20", "find_gen: the generation comparison is inverted",
      ("            return (ctx->clients[i].gen == gen) ? i : -1;", "            return (ctx->clients[i].gen != gen) ? i : -1;")),
    M("G21", "find_gen: the fd is not compared (first used slot with the generation)",
      (FG_LOOP, FG_LOOP.replace(" && ctx->clients[i].fd == fd", ""))),
    M("G22", "find_gen: a FREE slot can match (state not checked)",
      (FG_LOOP, FG_LOOP.replace("ctx->clients[i].state != WS_CLIENT_FREE && ", ""))),
    M("G23", "find_gen: a CLOSING slot is not found (only ACTIVE)",
      (FG_LOOP, FG_LOOP.replace("ctx->clients[i].state != WS_CLIENT_FREE", "ctx->clients[i].state == WS_CLIENT_ACTIVE"))),
    M("G24", "find_gen: loop reads one slot past max_clients (< becomes <=)",
      (FG_LOOP, FG_LOOP.replace("i < ctx->max_clients", "i <= ctx->max_clients"))),
    M("G25", "find_gen: returns the slot index + 1",
      ("            return (ctx->clients[i].gen == gen) ? i : -1;", "            return (ctx->clients[i].gen == gen) ? i + 1 : -1;")),
    M("G26", "find_gen: a generation mismatch keeps searching instead of returning -1",
      ("            return (ctx->clients[i].gen == gen) ? i : -1;", "            if (ctx->clients[i].gen == gen) return i;"),
      equivalent="a used fd is in the table once (ws_clients_add rejects a duplicate fd, -2), so no later slot can have the same fd; "
                 "the search that goes on finds nothing and returns -1 as well"),
]
