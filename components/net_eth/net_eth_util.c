#include "net_eth_util.h"
#include <stdio.h>

bool net_eth_ip4_to_str(uint32_t ip, char *buf, size_t len)
{
    if (buf == NULL || len < 16) {
        return false;
    }
    snprintf(buf, len, "%u.%u.%u.%u",
             (unsigned int)(ip & 0xFF),
             (unsigned int)((ip >> 8) & 0xFF),
             (unsigned int)((ip >> 16) & 0xFF),
             (unsigned int)((ip >> 24) & 0xFF));
    return true;
}
