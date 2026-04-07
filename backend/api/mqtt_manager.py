"""
MQTT client for the backend.

Responsibilities:
- Publish commands to hub/{hub_id}/commands (QoS 1) when a PendingCommand is created.
- Subscribe to hub/+/status to track hub online/offline state (replaces heartbeat endpoint).

Topic layout:
  hub/{hub_id}/commands   ← backend publishes, hub subscribes  (QoS 1)
  hub/{hub_id}/status     ← hub publishes "online" on connect, LWT sends "offline"
"""

import json
import logging
import threading

import paho.mqtt.client as mqtt
from django.conf import settings

logger = logging.getLogger(__name__)

_client: mqtt.Client | None = None
_lock = threading.Lock()


def _on_connect(client, userdata, flags, rc):
    if rc == 0:
        logger.info("MQTT connected to %s:%d", settings.MQTT_BROKER_HOST, settings.MQTT_BROKER_PORT)
        client.subscribe("hub/+/status", qos=1)
    else:
        logger.error("MQTT connection failed (rc=%d)", rc)


def _on_disconnect(client, userdata, rc):
    if rc != 0:
        logger.warning("MQTT disconnected unexpectedly (rc=%d) — will reconnect", rc)


def _on_message(client, userdata, msg):
    """Update Hub.last_seen / online state from hub/{hub_id}/status messages."""
    parts = msg.topic.split("/")
    if len(parts) != 3:
        return

    hub_id = parts[1]

    try:
        payload = json.loads(msg.payload.decode())
    except (json.JSONDecodeError, UnicodeDecodeError):
        payload = msg.payload.decode().strip()

    # Payload can be the plain string "online"/"offline" or a JSON object with a "status" key.
    if isinstance(payload, dict):
        hub_status = payload.get("status", "online")
    else:
        hub_status = payload

    # Import inside the function to avoid hitting Django's app registry before it's ready.
    from django.utils import timezone
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
