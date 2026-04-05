# Wi-Fi Provisioning — SoftAP Captive Portal

Replace hardcoded credentials with a self-service provisioning flow.
On first boot (no saved credentials), the device starts a SoftAP and serves
a captive portal where the user enters their Wi-Fi SSID and password.
Credentials are saved to NVS; on subsequent boots the device connects directly.

## Tasks

- [x] Check NVS for saved Wi-Fi credentials at boot; skip provisioning if found
- [x] Add NVS read/write helpers for SSID and password in `wifi_manager.c`
- [x] Implement SoftAP mode fallback when no credentials are stored
- [x] Write a minimal HTTP server (`esp_http_server`) serving a captive portal page on the AP
- [x] Create the HTML form (SSID + password fields, submit button) served by the portal
- [x] Handle form POST: validate input, save credentials to NVS, restart in STA mode
- [x] Remove hardcoded `CONFIG_WIFI_SSID` / `CONFIG_WIFI_PASSWORD` from `main.c` and `sdkconfig.defaults`
- [ ] Test full flow: fresh device → AP appears → portal → submit → NVS saved → STA mode
