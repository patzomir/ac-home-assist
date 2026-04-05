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

## Open questions

- Full MQTT (drop HTTP uploads too) vs hybrid (HTTP uploads + MQTT commands)?
- Per-hub MQTT credentials vs shared broker password?
- Multi-tenant: one Mosquitto instance shared, or per-customer ACL?
