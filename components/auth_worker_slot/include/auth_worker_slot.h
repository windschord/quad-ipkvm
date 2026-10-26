#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool in_use;
} auth_worker_slot_t;

// Initialize or reset a slot to the available (released) state.
void auth_worker_slot_init(auth_worker_slot_t *slot);
bool auth_worker_slot_try_acquire(auth_worker_slot_t *slot);
void auth_worker_slot_release(auth_worker_slot_t *slot);

#ifdef __cplusplus
}
#endif
