def M(id, desc, *edits):
    return {"id": id, "desc": desc, "edits": list(edits)}


# usb_vbus_ctrl.c への、振る舞いが変わらない(同値の)変更。ミュータントとしては数えない。
# 「本当に同値か」を確かめるために、同じ実行器(mutate_usb_vbus_ctrl.py)で流し、全部が survived になることを見る。
# 理由は docs/USB_VBUS_TESTS.md 3.2。
BACKWARD = "    if (in->now_ms < c->last_now_ms) {\n        c->run_start_ms = in->now_ms;\n        c->disc_active = false; /* 数え直し。強制遮断の待ちも最初から */\n    }"

MUTANTS = [
    M("E01", "init: disconnect clamp > -> >= (assigns MAX to MAX)",
      ("    if (c->config.disconnect_debounce_ms > USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS) {",
       "    if (c->config.disconnect_debounce_ms >= USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS) {")),
    M("E02", "init: force-off clamp > -> >= (assigns MAX to MAX)",
      ("    if (c->config.force_off_after_ms > USB_VBUS_CTRL_FORCE_OFF_MAX_MS) {",
       "    if (c->config.force_off_after_ms >= USB_VBUS_CTRL_FORCE_OFF_MAX_MS) {")),
    M("E03", "init: force-off zero test == 0 -> < 1 (unsigned)",
      ("    if (c->config.force_off_after_ms == 0) {", "    if (c->config.force_off_after_ms < 1) {")),
    M("E04", "update: NULL input is (valid = true, present = false): still VBUS absent",
      ("    none_in.vbus_present = false;\n    none_in.sample_valid = false;", "    none_in.vbus_present = false;\n    none_in.sample_valid = true;")),
    M("E05", "update: NULL input is (valid = false, present = true): still VBUS absent",
      ("    none_in.vbus_present = false;\n    none_in.sample_valid = false;", "    none_in.vbus_present = true;\n    none_in.sample_valid = false;")),
    M("E06", "clock: a backward step drops the run (run_active = false) instead of moving run_start",
      (BACKWARD, "    if (in->now_ms < c->last_now_ms) {\n        c->run_active = false;\n        c->disc_active = false;\n    }")),
]
