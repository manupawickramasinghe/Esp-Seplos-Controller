/* Minimal captive-portal DNS server: answers every A query with the AP address. */
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t dns_server_start(uint32_t ip_addr_network_order);
void dns_server_stop(void);
