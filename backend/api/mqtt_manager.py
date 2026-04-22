"""
MQTT client for the backend.

Responsibilities:
- Publish commands to hub/{hub_id}/commands (QoS 1) when a PendingCommand is created.
- Subscribe to hub/+/status to track hub online/offline state (replaces heartbeat endpoint).
- Subscribe to hub/+/plug/+/metering for smart plug power readings.
- Subscribe to hub/+/network for on-demand Zigbee network scan results.

Topic layout:
  hub/{hub_id}/commands              ← backend publishes, hub subscribes  (QoS 1)
  hub/{hub_id}/status                ← hub publishes "online" on connect, LWT sends "offline"
  hub/{hub_id}/plug/{addr}/metering  ← hub publishes plug power readings  (QoS 0)
  hub/{hub_id}/network               ← hub publishes scan results: {"plugs": [addr, ...]}
"""

import json
import logging
import threading

import paho.mqtt.client as mqtt
from django.conf import settings
from django.utils import timezone

logger = logging.getLogger(__name__)

_client: mqtt.Client | None = None
_lock = threading.Lock()


def _on_connect(client, userdata, flags, rc):
    if rc == 0:
        logger.info("MQTT connected to %s:%d", settings.MQTT_BROKER_HOST, settings.MQTT_BROKER_PORT)
        client.subscribe([
            ("hub/+/status", 1),
            ("hub/+/plug/+/metering", 0),
            ("hub/+/network", 1),
        ])
    else:
        logger.error("MQTT connection failed (rc=%d)", rc)


def _on_disconnect(client, userdata, rc):
    if rc != 0:
        logger.warning("MQTT disconnected unexpectedly (rc=%d) — will reconnect", rc)


def _on_hub_status(hub_id: str, raw_payload):
    """Handle hub/{hub_id}/status — update Hub.last_seen."""
    try:
        payload = json.loads(raw_payload.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        payload = raw_payload.decode().strip()

    hub_status = payload.get("status", "online") if isinstance(payload, dict) else payload

    from .models import Hub

    hub, _ = Hub.objects.get_or_create(
        identifier=hub_id,
        defaults={"name": f"Hub {hub_id[:8]}"},
    )
    if hub_status == "online":
        hub.last_seen = timezone.now()
        hub.save(update_fields=["last_seen"])
        logger.info("Hub %s online (MQTT)", hub_id)
    else:
        logger.info("Hub %s offline (MQTT LWT)", hub_id)


def _on_plug_metering(hub_id: str, addr_hex: str, raw_payload):
    """Handle hub/{hub_id}/plug/{addr}/metering — store SmartPlugEvent."""
    try:
        data = json.loads(raw_payload.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        logger.warning("Invalid JSON in plug metering from %s/%s", hub_id, addr_hex)
        return

    from .models import Hub, SmartPlug, SmartPlugEvent

    hub, _ = Hub.objects.get_or_create(
        identifier=hub_id,
        defaults={"name": f"Hub {hub_id[:8]}"},
    )
    hub.last_seen = timezone.now()
    hub.save(update_fields=["last_seen"])

    short_addr = int(addr_hex, 16)
    plug, _ = SmartPlug.objects.get_or_create(
        hub=hub,
        short_addr=short_addr,
        defaults={"name": f"Plug 0x{short_addr:04X}"},
    )
    plug.online = True
    plug.last_seen = timezone.now()
    plug.save(update_fields=["online", "last_seen"])

    measured_watts = int(data["watts"])       if data.get("watts")      is not None else None
    voltage_dv     = int(data["voltage_dv"])  if data.get("voltage_dv") is not None else None
    current_ma     = int(data["current_ma"])  if data.get("current_ma") is not None else None
    energy_wh      = int(data["energy_wh"])   if data.get("energy_wh")  is not None else None

    ts = timezone.now()
    if data.get("ts"):
        from datetime import datetime
        from datetime import timezone as dt_tz
        ts = datetime.fromtimestamp(int(data["ts"]), tz=dt_tz.utc)

    # Merge attributes from the same poll cycle (two cluster responses arrive within seconds).
    MERGE_WINDOW_S = 60
    window_start = ts - timezone.timedelta(seconds=MERGE_WINDOW_S)
    existing = (SmartPlugEvent.objects
                .filter(plug=plug, ts__gte=window_start)
                .order_by("-ts")
                .first())

    if existing:
        update_fields = []
        for field, value in [("measured_watts", measured_watts), ("energy_wh", energy_wh),
                              ("voltage_dv", voltage_dv), ("current_ma", current_ma)]:
            if value is not None:
                setattr(existing, field, value)
                update_fields.append(field)
        if update_fields:
            existing.save(update_fields=update_fields)
    else:
        SmartPlugEvent.objects.create(
            plug=plug,
            power_on=True,
            measured_watts=measured_watts or 0,
            energy_wh=energy_wh,
            voltage_dv=voltage_dv,
            current_ma=current_ma,
            ts=ts,
        )

    logger.info("Plug metering hub=%s addr=0x%04X %sW %smV %smA",
                hub_id, short_addr, measured_watts, voltage_dv, current_ma)


def _on_hub_network(hub_id: str, raw_payload):
    """Handle hub/{hub_id}/network — reconcile SmartPlug records.

    Expected payload: {"plugs": [2, 5, 8]}
    Each value is a Zigbee short address (integer).
    Creates plug records for any address not already in the DB.
    """
    try:
        data = json.loads(raw_payload.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        logger.warning("Invalid JSON in network scan from %s", hub_id)
        return

    addrs = data.get("plugs", [])
    if not isinstance(addrs, list):
        logger.warning("Unexpected network payload from %s: %r", hub_id, data)
        return

    from .models import Hub, SmartPlug

    hub, _ = Hub.objects.get_or_create(
        identifier=hub_id,
        defaults={"name": f"Hub {hub_id[:8]}"},
    )

    for addr in addrs:
        short_addr = int(addr)
        _, created = SmartPlug.objects.get_or_create(
            hub=hub,
            short_addr=short_addr,
            defaults={"name": f"Plug 0x{short_addr:04X}"},
        )
        if created:
            logger.info("Network scan: new plug hub=%s addr=0x%04X", hub_id, short_addr)

    hub.last_network_scan = {"ts": timezone.now().isoformat(), "addrs": [int(a) for a in addrs]}
    hub.save(update_fields=["last_network_scan"])
    logger.info("Network scan: hub=%s reported %d plug(s)", hub_id, len(addrs))


def _on_message(client, userdata, msg):
    """Route incoming messages to the appropriate handler."""
    parts = msg.topic.split("/")
    # hub/{hub_id}/status
    if len(parts) == 3 and parts[2] == "status":
        _on_hub_status(parts[1], msg.payload)
    # hub/{hub_id}/network
    elif len(parts) == 3 and parts[2] == "network":
        _on_hub_network(parts[1], msg.payload)
    # hub/{hub_id}/plug/{addr}/metering
    elif len(parts) == 5 and parts[2] == "plug" and parts[4] == "metering":
        _on_plug_metering(parts[1], parts[3], msg.payload)


def start():
    """Connect to the broker and start the background network loop.

    Called once from ApiConfig.ready(). Safe to call multiple times — subsequent
    calls are no-ops.
    """
    global _client

    with _lock:
        if _client is not None:
            return

        client = mqtt.Client(client_id="ac-backend")
        client.on_connect = _on_connect
        client.on_disconnect = _on_disconnect
        client.on_message = _on_message
        client.reconnect_delay_set(min_delay=1, max_delay=30)

        try:
            client.connect_async(settings.MQTT_BROKER_HOST, settings.MQTT_BROKER_PORT, keepalive=60)
        except Exception:
            logger.exception("MQTT initial connect failed — will retry in background")

        client.loop_start()
        _client = client


def publish_command(hub_id: str, command_id: int, command_type: str, payload: dict):
    """Publish a single command to hub/{hub_id}/commands (QoS 1).

    If the broker is unavailable the message is queued by paho and delivered
    once the connection is re-established (as long as the process is still running).
    The PendingCommand row in the DB acts as the durable fallback.
    """
    if _client is None:
        logger.warning("MQTT not started — command %d not published to %s", command_id, hub_id)
        return

    topic = f"hub/{hub_id}/commands"
    message = json.dumps({"id": command_id, "type": command_type, "payload": payload})
    result = _client.publish(topic, message, qos=1)
    if result.rc != mqtt.MQTT_ERR_SUCCESS:
        logger.warning("MQTT publish queued (rc=%d) for %s", result.rc, topic)
    else:
        logger.debug("MQTT published to %s: %s", topic, message)
