#pragma once

#include "auth_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the mbedTLS-backed cryptographic interface for auth_logic.
const auth_crypto_if_t* auth_crypto_mbedtls_get_interface(void);

#ifdef __cplusplus
}
#endif
