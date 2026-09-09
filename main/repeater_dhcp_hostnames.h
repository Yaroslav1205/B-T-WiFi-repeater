#pragma once

#include <stdbool.h>
#include <stddef.h>

bool repeater_dhcp_hostnames_get(const char *mac, char *out_hostname, size_t out_hostname_size);
