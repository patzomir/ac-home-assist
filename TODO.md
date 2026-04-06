# TODO

## Firmware

- [ ] Implement MQTT command subscription (`hub/{hub_id}/commands`) — see `mqtt-hub.md`
- [ ] Implement MQTT LWT for hub online/offline status
- [ ] Remove HTTP command polling (`GET /api/hubs/{hub_id}/commands/`) once MQTT is in place
- [ ] Implement heartbeat (currently defined in backend but not called by firmware)
- [ ] Evaluate dropping `esp_http_client` entirely (save ~15–25 KB flash) if all traffic moves to MQTT

## Backend

- [ ] Add Mosquitto broker to deployment — see `mqtt-hub.md`
- [ ] Add `paho-mqtt` publisher: fire to `hub/{hub_id}/commands` on `PendingCommand` creation
- [ ] Subscribe to `hub/{hub_id}/status` to update hub online/offline in DB (replaces heartbeat endpoint)
- [ ] Replace SQLite with PostgreSQL before multi-tenant rollout
- [ ] Replace Django dev server with gunicorn
- [ ] Wire up APScheduler (already in requirements) for background jobs

## Docs

- [ ] `mqtt-hub.md` — drafted, needs review
- [ ] `architecture.md` — update to reflect MQTT decision once implemented
- [ ] `DEPLOYMENT.md` — add Mosquitto setup steps once broker config is finalised

## Hub Authentication (planned)

### Hub identity
- [ ] Derive hub ID from Wi-Fi MAC address at boot (`esp_wifi_get_mac(WIFI_IF_STA, mac)`, format as `hub_AABBCCDDEEFF`)
- [ ] Send `X-Hub-Id` header on all HTTP requests (`http_reporter.c`) — fixes current "unknown" hub issue

### Claim-code provisioning flow
1. User opens backend UI → clicks "Add Hub" → backend generates short-lived (15 min), single-use claim code
2. User enters claim code in SoftAP captive portal alongside WiFi credentials
3. After WiFi connects, firmware POSTs `{"hub_id": "hub_MAC", "claim_code": "XXXX-XXXX"}` to `/api/provision`
4. Backend validates code (exists, not expired, not used), creates Hub record, generates per-hub MQTT password, returns credentials
5. Firmware saves `mqtt_user` + `mqtt_pass` to NVS; claim code discarded

### Firmware changes
- [ ] Add claim code field to SoftAP captive portal HTML (`wifi_manager.c:127-160`)
- [ ] Save claim code to NVS alongside WiFi creds
- [ ] After WiFi connects, call `POST /api/provision`; save returned MQTT credentials to NVS
- [ ] New `mqtt_client.c`: connect to Mosquitto with NVS credentials; subscribe to `hub/{id}/commands`; LWT for heartbeat

### Backend changes
- [ ] New `HubClaimCode` model: `code`, `created_at`, `expires_at`, `used` (`models.py`)
- [ ] Add `mqtt_password_hash` field to `Hub` model (`models.py`)
- [ ] `POST /api/provision` endpoint: validate claim code → generate MQTT password → return credentials (`views.py`)
- [ ] Claim code generation endpoint (called from backend UI when user clicks "Add Hub") (`views.py`)
- [ ] Add `MQTT_BROKER_HOST`, `MQTT_BROKER_PORT` to `settings.py`
- [ ] New `mqtt_manager.py`: paho-mqtt client; subscribe to `hub/+/status`; publish to `hub/{id}/commands`

### Mosquitto broker
- [ ] Configure per-hub username/password (dynamic security plugin or managed password file)
- [ ] ACL: each hub restricted to `hub/<its-own-id>/#` topics only
- [ ] TLS: self-signed CA cert; pin in firmware via `esp_mqtt_client_config_t.broker.verification.certificate`

### Migration path
1. Fix now: send `X-Hub-Id` in all HTTP requests
2. Add MQTT transport (`mqtt_client.c`) for command subscription + LWT
3. Add claim-code provisioning auth
4. Run hybrid (HTTP events + MQTT commands) until stable
5. Drop HTTP polling: remove `poll_commands` endpoint; remove `esp_http_client` from firmware

## Backend Workers (APScheduler)

**Depends on:** MQTT broker live + `hub/{hub_id}/status` LWT wired up (so offline detection is event-driven rather than polled).

### Context

`APScheduler` is already in `requirements.txt` but not started. The Django dev server runs as a single process, so a `BackgroundScheduler` started in `AppConfig.ready()` is sufficient for MVP. Production (gunicorn) will need `--preload` or a dedicated worker process.

### Jobs

| Job | Interval | Purpose |
|-----|----------|---------|
| `check_hub_liveness` | every 5 min | Set `IREmitter.online = False` and `SmartPlug.online = False` for any hub whose `last_seen` is older than 10 min (covers MQTT LWT not arriving, e.g. power cut) |
| `cleanup_stale_commands` | every 24 h | Delete `PendingCommand` rows where `delivered=True` and `delivered_at` > 7 days ago |

### Tasks

#### Backend
- [ ] Create `backend/api/jobs.py`: define `check_hub_liveness()`, `cleanup_stale_commands()`, and `start()` that registers both with `BackgroundScheduler`
- [ ] Wire `start()` into `backend/api/apps.py` `ApiConfig.ready()` (guard with `os.environ.get("RUN_MAIN")` to avoid double-start under Django's reloader)
- [ ] Test: set `Hub.last_seen` to 15 min ago in shell → call `check_hub_liveness()` → assert `emitter.online = False`

#### Deployment
- [ ] Add `--preload` flag to gunicorn invocation (or run APScheduler in a separate `python manage.py runjobs` command) once we move off the dev server

## Remote Firmware Upgrade (OTA)

ESP-IDF's built-in OTA partition scheme (`esp_ota_ops.h`) allows the hub to download and apply a new firmware image at runtime with automatic rollback on boot failure.

### How it works (end-to-end)

1. Developer builds a new firmware binary (`flash.sh` / `idf.py build`) and uploads it to the backend.
2. Backend stores the binary and records the new version in the DB.
3. Backend publishes an MQTT command to `hub/{hub_id}/commands`:
   ```json
   { "type": "ota_update", "url": "https://<backend>/api/firmware/latest.bin", "version": "1.2.0" }
   ```
4. Firmware receives the command, opens an `esp_https_ota` session to the URL, streams the image into the OTA partition, then calls `esp_ota_set_boot_partition()` and reboots.
5. After reboot the new image runs; if it doesn't call `esp_ota_mark_app_valid_cancel_rollback()` within a watchdog window, the bootloader rolls back to the previous partition automatically.
6. Hub publishes `{ "type": "ota_result", "version": "1.2.0", "status": "ok"|"failed" }` to `hub/{hub_id}/status` so the backend can update the DB.

### Firmware tasks
- [ ] Ensure `partitions.csv` uses the dual OTA layout (`ota_0` / `ota_1`, each ≥ app size; remove `factory` if flash is tight)
- [ ] Handle `ota_update` command type in the MQTT command dispatcher (`mqtt_client.c` — to be created)
- [ ] Implement `ota_task()` using `esp_https_ota()`: validate version string, stream image, call `esp_ota_set_boot_partition()`, reboot
- [ ] Call `esp_ota_mark_app_valid_cancel_rollback()` after successful first MQTT message post-reboot (confirms new image is healthy)
- [ ] Publish OTA result (`ok` / `failed` + error reason) back to `hub/{hub_id}/status`
- [ ] Pin the backend TLS certificate in `esp_http_client_config_t.cert_pem` for the OTA download request (reuse the Mosquitto CA or a dedicated backend cert)

### Backend tasks
- [ ] Add `FirmwareImage` model: `version`, `binary` (FileField or S3 key), `sha256`, `uploaded_at`, `is_latest` (`models.py`)
- [ ] `POST /api/firmware/upload` — staff-only endpoint: accept `.bin`, compute SHA-256, save, mark as latest
- [ ] `GET /api/firmware/latest.bin` — serve the binary; require hub JWT or mutual TLS so random clients can't download
- [ ] `POST /api/hubs/{hub_id}/ota` — triggers upgrade: validates a newer version exists, publishes `ota_update` MQTT command, records `OTAJob` row (status=`pending`)
- [ ] Add `OTAJob` model: `hub`, `firmware_image`, `triggered_at`, `completed_at`, `status` (`pending`/`ok`/`failed`/`rolled_back`)
- [ ] MQTT subscriber: on `hub/{hub_id}/status` messages with `type=ota_result`, update `OTAJob.status` and `Hub.firmware_version`
- [ ] Expose current firmware version in `Hub` model (`firmware_version` CharField) and in the hub detail API response

### Security considerations
- [ ] Sign firmware images (e.g. with ESP-IDF secure boot v2 or a detached ECDSA signature checked before `esp_ota_set_boot_partition`) so a compromised backend can't push arbitrary code
- [ ] Include `sha256` in the MQTT command; firmware verifies checksum before rebooting
- [ ] Rate-limit `POST /api/hubs/{hub_id}/ota` (one in-flight OTA per hub at a time)

## Open questions

- Full MQTT (drop HTTP uploads too) vs hybrid (HTTP uploads + MQTT commands)?
- Multi-tenant: one Mosquitto instance shared, or per-customer ACL?
