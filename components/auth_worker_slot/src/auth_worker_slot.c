#include "auth_worker_slot.h"

void auth_worker_slot_init(auth_worker_slot_t *slot) {
    if (slot) {
        slot->in_use = false;
    }
}

bool auth_worker_slot_try_acquire(auth_worker_slot_t *slot) {
    if (!slot) return false;
    if (slot->in_use) {
        return false;
    }
    slot->in_use = true;
    return true;
}

void auth_worker_slot_release(auth_worker_slot_t *slot) {
    if (slot) {
        slot->in_use = false;
    }
}
