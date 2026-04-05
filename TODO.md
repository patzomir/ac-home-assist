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

## Open questions

- Full MQTT (drop HTTP uploads too) vs hybrid (HTTP uploads + MQTT commands)?
- Multi-tenant: one Mosquitto instance shared, or per-customer ACL?
