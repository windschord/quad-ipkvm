#pragma once

#include "../../../components/auth_logic/include/auth_logic.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool g_ref_random_should_fail;
extern const auth_crypto_if_t ref_crypto_if;

#ifdef __cplusplus
}
#endif
