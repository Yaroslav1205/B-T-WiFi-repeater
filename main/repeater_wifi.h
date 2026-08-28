#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "repeater_runtime.h"
#include "repeater_types.h"

esp_err_t repeater_wifi_start(repeater_runtime_t *runtime);
esp_err_t repeater_wifi_apply_saved_network_settings(repeater_runtime_t *runtime, bool reconnect_now);
esp_err_t repeater_wifi_get_softap_ip_info(const repeater_runtime_t *runtime,
                                           esp_netif_ip_info_t *out_ip_info);
esp_err_t repeater_wifi_get_upstream_ip_info(const repeater_runtime_t *runtime,
                                             esp_netif_ip_info_t *out_ip_info);
esp_err_t repeater_wifi_read_softap_mac(char *out_mac, size_t out_mac_size);
bool repeater_wifi_is_upstream_connected(const repeater_runtime_t *runtime);
bool repeater_wifi_is_using_backup_upstream(const repeater_runtime_t *runtime);
const char *repeater_wifi_get_active_upstream_ssid(const repeater_runtime_t *runtime);
int64_t repeater_wifi_get_upstream_connected_at_epoch(const repeater_runtime_t *runtime);
esp_err_t repeater_wifi_get_upstream_packet_counts(const repeater_runtime_t *runtime,
                                                   uint32_t *out_rx_packets,
                                                   uint32_t *out_tx_packets);
int repeater_wifi_get_client_count(const repeater_runtime_t *runtime);
size_t repeater_wifi_get_clients(repeater_client_info_t *clients, size_t max_clients);
