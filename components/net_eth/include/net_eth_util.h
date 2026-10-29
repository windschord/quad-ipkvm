#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Converts an IPv4 address to a string representation ("a.b.c.d").
 *
 * @param ip The IPv4 address to convert (little-endian: first octet is in lowest byte).
 * @param buf The buffer to write the string into.
 * @param len The size of the buffer. Must be at least 16 to fit "255.255.255.255" and null terminator.
 * @return true if successful, false if buf is too small or NULL.
 */
bool net_eth_ip4_to_str(uint32_t ip, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
