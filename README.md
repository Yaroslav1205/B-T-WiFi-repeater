# B-T WiFi repeater

ESP32-C3 Wi-Fi repeater project in `AP + STA` mode with:

- uplink connection to an external access point
- automatic failover between primary and backup uplink access points
- local SoftAP for client devices
- NAPT/NAT for internet sharing
- web UI with Basic Auth
- NVS-persisted transmit power level
- client table with MAC and RSSI
- status LED that shows uplink state and connected client count

## Project structure

- `main/app_main.c`  
  Thin bootstrap: initializes ESP-IDF services, NVS, repeater core, reboot scheduler, and LED module.

- `main/repeater_core.c` / `main/repeater_core.h`  
  Composition root of the device runtime. Starts internal modules and owns shared runtime state.

- `main/repeater_wifi.c` / `main/repeater_wifi.h`  
  Wi-Fi lifecycle, AP/STA event handling, DHCP DNS sync, NAPT enablement, and runtime network reconfiguration.

- `main/repeater_web_bridge.c` / `main/repeater_web_bridge.h`  
  Adapter between core/settings and the HTTP UI. Keeps web concerns away from Wi-Fi internals and rolls back Wi-Fi changes if runtime apply fails.

- `main/repeater_runtime.h` / `main/repeater_types.h`  
  Shared internal runtime state and cross-module data types.

- `main/status_led.c` / `main/status_led.h`  
  Status LED behavior isolated from networking logic.

- `main/router_web.c` / `main/router_web.h`  
  Generic HTTP/UI layer: Basic Auth, HTML rendering, form parsing, and request handlers.

- `main/repeater_settings.c` / `main/repeater_settings.h`  
  NVS-backed configuration store for radio power, UI theme, reboot policy, Wi-Fi credentials, and device labels.

- `main/project_wifi_config.h`  
  Single place for defaults such as device name, Wi-Fi credentials, LED config, time sync, and web login/password.

## Main configuration points

Edit `main/project_wifi_config.h` to change:

- project display name
- primary upstream SSID/password
- backup upstream SSID/password and failover thresholds
- repeater SoftAP SSID/password
- LED GPIO and active level
- periodic log interval
- web UI login and password

## Build and flash

Run:

```bash
idf.py -p PORT flash monitor
```

Web UI is available at:

```text
http://192.168.4.1/
```

Default web login:

```text
user: admin
pass: change-me
```
