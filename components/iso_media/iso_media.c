#include "iso_media.h"

#include <string.h>

void iso_media_init(iso_media_t *m, iso_media_force_eject_cb_t on_force_eject, void *ctx) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->on_force_eject = on_force_eject;
    m->on_force_eject_ctx = ctx;
}

iso_media_result_t iso_media_mount(iso_media_t *m, uint32_t id, bool force) {
    if (!m) return ISO_MEDIA_INVALID;
    if (m->loaded && m->id == id) return ISO_MEDIA_ALREADY_MOUNTED;
    if (m->loaded && m->prevent_removal && !force) return ISO_MEDIA_LOCKED;
    m->loaded = true;
    m->id = id;
    m->prevent_removal = false;
    m->unit_attention_media = true;
    m->event_pending = ISO_MEDIA_EVENT_NEW_MEDIA;
    m->ejected_by_target = false;
    return ISO_MEDIA_MOUNTED;
}

iso_media_result_t iso_media_eject(iso_media_t *m, bool force) {
    if (!m) return ISO_MEDIA_INVALID;
    if (!m->loaded) {
        m->ejected_by_target = false;
        return ISO_MEDIA_NOT_MOUNTED;
    }
    if (m->prevent_removal && !force) return ISO_MEDIA_LOCKED;
    m->loaded = false;
    m->id = 0;
    m->prevent_removal = false;
    m->event_pending = ISO_MEDIA_EVENT_MEDIA_REMOVAL;
    m->ejected_by_target = false;
    return ISO_MEDIA_EJECTED;
}

iso_media_result_t iso_media_target_eject(iso_media_t *m) {
    if (!m) return ISO_MEDIA_INVALID;
    if (!m->loaded) return ISO_MEDIA_NOT_MOUNTED;
    if (m->prevent_removal) return ISO_MEDIA_LOCKED;
    m->loaded = false;
    m->id = 0;
    m->event_pending = ISO_MEDIA_EVENT_MEDIA_REMOVAL;
    m->ejected_by_target = true;
    return ISO_MEDIA_EJECTED;
}

bool iso_media_set_prevent(iso_media_t *m, bool prevent) {
    if (!m) return false;
    if (prevent && !m->loaded) return false;
    m->prevent_removal = prevent;
    return true;
}

bool iso_media_request_eject(iso_media_t *m) {
    if (!m || !m->loaded) return false;
    if (m->event_pending == ISO_MEDIA_EVENT_NEW_MEDIA || m->event_pending == ISO_MEDIA_EVENT_MEDIA_REMOVAL) {
        return false;
    }
    m->event_pending = ISO_MEDIA_EVENT_EJECT_REQUEST;
    return true;
}

void iso_media_bus_reset(iso_media_t *m) {
    if (!m) return;
    m->prevent_removal = false;
    m->unit_attention_reset = true;
    m->abort_generation++;
}

void iso_media_force_eject(iso_media_t *m) {
    if (!m) return;
    if (m->loaded) {
        m->loaded = false;
        m->id = 0;
        m->unit_attention_media = true;
        m->event_pending = ISO_MEDIA_EVENT_MEDIA_REMOVAL;
    }
    m->prevent_removal = false;
    m->abort_generation++;
    if (m->on_force_eject) m->on_force_eject(m->on_force_eject_ctx);
}

void iso_media_get_status(const iso_media_t *m, iso_media_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!m) return;
    out->mounted = m->loaded;
    out->id = m->loaded ? m->id : 0;
    out->locked = m->loaded && m->prevent_removal;
    out->ejected_by_target = m->ejected_by_target;
}

bool iso_media_is_loaded(const iso_media_t *m) { return m && m->loaded; }

bool iso_media_get_id(const iso_media_t *m, uint32_t *id) {
    if (!m || !m->loaded) return false;
    if (id) *id = m->id;
    return true;
}

bool iso_media_prevent_removal(const iso_media_t *m) { return m && m->prevent_removal; }

uint32_t iso_media_abort_generation(const iso_media_t *m) { return m ? m->abort_generation : 0; }

bool iso_media_take_unit_attention(iso_media_t *m, uint8_t *asc, uint8_t *ascq) {
    if (!m) return false;
    if (m->unit_attention_reset) {
        m->unit_attention_reset = false;
        if (asc) *asc = 0x29; /* POWER ON, RESET, OR BUS DEVICE RESET OCCURRED */
        if (ascq) *ascq = 0x00;
        return true;
    }
    if (m->unit_attention_media) {
        m->unit_attention_media = false;
        if (asc) *asc = 0x28; /* NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED */
        if (ascq) *ascq = 0x00;
        return true;
    }
    return false;
}

bool iso_media_unit_attention_pending(const iso_media_t *m) {
    return m && (m->unit_attention_reset || m->unit_attention_media);
}

iso_media_event_t iso_media_peek_event(const iso_media_t *m) { return m ? m->event_pending : ISO_MEDIA_EVENT_NONE; }

iso_media_event_t iso_media_take_event(iso_media_t *m) {
    if (!m) return ISO_MEDIA_EVENT_NONE;
    iso_media_event_t e = m->event_pending;
    m->event_pending = ISO_MEDIA_EVENT_NONE;
    return e;
}
