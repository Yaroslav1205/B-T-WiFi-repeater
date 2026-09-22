# B-T WiFi repeater

ESP32-C3 Wi-Fi repeater project in `AP + STA` mode with:

- uplink connection to an external access point
- automatic failover between primary and backup uplink access points
- local SoftAP for client devices
- NAPT/NAT for internet sharing
- web UI with Basic Auth
- NVS-persisted transmit power level
- client table with MAC and RSSI
- status LED that shows uplink state and connected client count, with a saved on/off setting
- OTA firmware updates from GitHub Releases

## Version 1.0.8

- Apply the saved light/dark theme to client-save confirmations, settings confirmations, and firmware update pages using the shared dashboard palette.
- Migrate client history from pre-1.0.7 and 1.0.7 storage layouts without discarding saved records.
- Protect unreadable or unsupported history from being overwritten, including after a settings factory reset.
- Log when emergency NVS recovery clears settings and client history.

History already overwritten by older firmware cannot be recovered. See the OTA history compatibility notes below.

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

## Status LED

In the web UI, open **Access and appearance → Appearance**, set **Status LED** to
**Off**, and click **Save settings**. The light turns off without restarting the
repeater. The setting persists across restarts and also suppresses the startup
blink pattern. Select **On** to restore normal indication.

The default is **On**. A factory reset restores this default; holding BOOT during
startup still uses the LED to indicate the reset procedure.
`PROJECT_STATUS_LED_ENABLED` remains the hardware-level switch in the build configuration.

## Factory reset

1. Power on (or press RESET) with **BOOT released**. On ESP32-C3, holding BOOT
   during power-on/reset selects the ROM download mode and the application cannot run.
2. Press BOOT within the first **5 seconds** of application startup, then hold it
   for **10 seconds**. The status LED stays on while the button is held.
3. **Three flashes** confirm that settings were erased successfully. Release BOOT;
   the repeater continues normal startup without another power cycle.
   Releasing BOOT before the 10 seconds cancels the reset.

Reset indication works even when the saved status LED option is Off. After reset,
**hidden SSID is Off** (the network is visible) and **status LED is On**.
The default network is **B-T WiFi repeater**, password **12345678**; the web login
is **admin / admin**. Client history is preserved.

## Client history across OTA updates

OTA writes the inactive application partition; client history stays in NVS.
The history reader supports the fixed 64-slot format used before 1.0.7 and
the compact format written by 1.0.7. MAC addresses, descriptions and timestamps
are retained; existing hostnames and IP addresses are retained when present.
The next history write saves the migrated records using format version 2.

Version 1.0.7 changed the record layout without migrating the previous format.
It could display an empty history and replace the old blob on the next client
connection. This fix can recover old records only if they have not already been
overwritten.

An unreadable or unsupported history blob now disables history writes and logs
the error, preserving the original data. A settings factory reset also preserves
this protection. If NVS initialization reports NO_FREE_PAGES or
NEW_VERSION_FOUND, startup logs a warning, erases NVS and initializes it again
so the device can boot with defaults. This emergency recovery resets all saved
settings and client history; it is not triggered by an unsupported history
record format. Other initialization errors, or a failed erase/retry, still stop
normal startup. Do not downgrade to firmware with the old destructive reader.

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
pass: admin
```

## OTA updates through GitHub

The firmware checker now uses the latest GitHub release assets from this repository:

- manifest: `https://github.com/Yaroslav1205/B-T-WiFi-repeater/releases/latest/download/repeater-ota-manifest.txt`
- firmware binary: `https://github.com/Yaroslav1205/B-T-WiFi-repeater/releases/latest/download/repeater-firmware.bin`

How publishing works:

1. Update `PROJECT_FIRMWARE_VERSION` in `main/project_wifi_config.h`.
2. Commit and push the change.
3. Create a matching tag like `v1.0.1`.
4. Push the tag to GitHub.

Example:

```bash
git add .
git commit -m "Release firmware v1.0.1"
git push origin main
git tag v1.0.1
git push origin v1.0.1
```

The GitHub Actions workflow `.github/workflows/release-firmware.yml` will build `softap_sta.bin`, generate the OTA manifest, and upload both assets to the tagged GitHub release. The repeater web UI can then check for updates and install the newest release over the air.
