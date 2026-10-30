#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_ETH_STATE_DOWN,
    NET_ETH_STATE_LINK_UP,
    NET_ETH_STATE_GOT_IP,
} net_eth_state_t;

/**
 * @brief Initialize Ethernet interface
 *
 * Does not abort on failure. Returns ESP_ERR_...
 */
esp_err_t net_eth_init(void);

/**
 * @brief Get the IP address as a string
 */
bool net_eth_get_ip_str(char *buf, size_t len);

/**
 * @brief Get the link speed in Mbps
 */
int net_eth_get_speed_mbps(void);

/**
 * @brief Get the current Ethernet state
 */
net_eth_state_t net_eth_get_state(void);

typedef void (*net_eth_cb_t)(net_eth_state_t state);

/**
 * @brief Register a callback for Ethernet state changes.
 *
 * NOTE: The callback is invoked from the event loop task and MUST NOT block.
 */
void net_eth_register_callback(net_eth_cb_t cb);

#ifdef __cplusplus
}
#endif
