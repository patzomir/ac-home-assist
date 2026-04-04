import logging
from datetime import timedelta

from django.utils import timezone
from rest_framework import status
from rest_framework.decorators import api_view
from rest_framework.response import Response

from .models import Hub, IREmitter, ACEvent, Schedule, PendingCommand
from .energy_model import estimate_watts, compute_interval_cost
from .weather import get_outdoor_temp

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Hub registration / heartbeat
# ---------------------------------------------------------------------------

@api_view(["POST"])
def hub_heartbeat(request, hub_id):
    """Hub calls this periodically to signal it's alive."""
    hub, _ = Hub.objects.get_or_create(
        identifier=hub_id,
        defaults={"name": f"Hub {hub_id[:8]}"},
    )
    hub.last_seen = timezone.now()
    if "lat" in request.data:
        hub.latitude  = float(request.data["lat"])
        hub.longitude = float(request.data["lon"])
    hub.save(update_fields=["last_seen", "latitude", "longitude"])
    return Response({"status": "ok"})


# ---------------------------------------------------------------------------
# Events — hub POSTs here on every AC command
# ---------------------------------------------------------------------------

@api_view(["POST"])
def receive_event(request):
    """
    Payload (from hub firmware http_reporter.c):
    {
        "addr":     1,        // Zigbee short address (int)
        "setpoint": 21,       // °C
        "mode":     "heat",
        "power":    true,
        "ts":       1712345678  // unix timestamp (optional)
    }
    Hub identifier is sent in X-Hub-Id header.
    """
    data    = request.data
    hub_id  = request.headers.get("X-Hub-Id", "unknown")
    hub, _  = Hub.objects.get_or_create(
        identifier=hub_id,
        defaults={"name": f"Hub {hub_id[:8]}"},
    )
    hub.last_seen = timezone.now()
    hub.save(update_fields=["last_seen"])

    short_addr = int(data.get("addr", 0))
    emitter, _ = IREmitter.objects.get_or_create(
        hub=hub,
        short_addr=short_addr,
        defaults={"name": f"AC 0x{short_addr:04X}"},
    )
    emitter.online    = True
    emitter.last_seen = timezone.now()
    emitter.save(update_fields=["online", "last_seen"])

    mode       = data.get("mode", "heat")
    setpoint_c = int(data.get("setpoint", 21))
    power_on   = bool(data.get("power", True))

    outdoor_temp = get_outdoor_temp(hub.latitude, hub.longitude)
    watts = estimate_watts(
        mode=mode,
        setpoint_c=setpoint_c,
        outdoor_temp_c=outdoor_temp if outdoor_temp is not None else 5.0,
        power_on=power_on,
    )

    ts = timezone.now()
    if data.get("ts"):
        from datetime import datetime
        ts = datetime.utcfromtimestamp(int(data["ts"])).replace(
            tzinfo=timezone.utc)

    ACEvent.objects.create(
        emitter=emitter,
        setpoint_c=setpoint_c,
        mode=mode,
        power_on=power_on,
        outdoor_temp_c=outdoor_temp,
        estimated_watts=watts,
        ts=ts,
    )

    logger.info("Event from hub=%s addr=0x%04X mode=%s setpoint=%d°C "
                "power=%s outdoor=%.1f°C → %dW",
                hub_id, short_addr, mode, setpoint_c,
                power_on, outdoor_temp or 0, watts)

    return Response({"watts": watts}, status=status.HTTP_201_CREATED)


# ---------------------------------------------------------------------------
# Dashboard data API
# ---------------------------------------------------------------------------

def _cost_for_period(emitter: IREmitter, since, until=None):
    until = until or timezone.now()
    events = list(
        ACEvent.objects
        .filter(emitter=emitter, ts__gte=since, ts__lte=until)
        .order_by("ts")
    )
    # Append a synthetic "now" event at current watts so the last interval
    # is counted up to this moment, not just to the last event timestamp.
    if events:
        last = events[-1]
        synthetic = ACEvent(
            emitter=emitter,
            estimated_watts=last.estimated_watts,
            ts=until,
        )
        events.append(synthetic)
    return compute_interval_cost(events)


def _daily_breakdown(emitter: IREmitter, days=7):
    """Return list of {date, kwh, cost_bgn} for the past N days."""
    result = []
    now = timezone.now()
    for d in range(days - 1, -1, -1):
        day_start = (now - timedelta(days=d)).replace(
            hour=0, minute=0, second=0, microsecond=0)
        day_end = day_start + timedelta(days=1)
        cost = _cost_for_period(emitter, day_start, day_end)
        result.append({
            "date":     day_start.strftime("%a %d/%m"),
            "kwh":      cost["kwh"],
            "cost_bgn": cost["cost_bgn"],
        })
    return result


@api_view(["GET"])
def dashboard_data(request):
    """
    Returns all data needed to render the dashboard.
    Called every 30 s by the frontend.
    """
    now = timezone.now()
    hubs = Hub.objects.prefetch_related("emitters").all()

    units = []
    for hub in hubs:
        for emitter in hub.emitters.all():
            # Current state = last event
            last_event = (
                ACEvent.objects.filter(emitter=emitter).order_by("-ts").first()
            )

            # Cost periods
            today_start = now.replace(hour=0, minute=0, second=0, microsecond=0)
            week_start  = today_start - timedelta(days=now.weekday())
            month_start = today_start.replace(day=1)

            cost_today  = _cost_for_period(emitter, today_start)
            cost_week   = _cost_for_period(emitter, week_start)
            cost_month  = _cost_for_period(emitter, month_start)

            # Session cost (since last ON event)
            last_on = (
                ACEvent.objects.filter(emitter=emitter, power_on=True)
                .order_by("-ts").first()
            )
            cost_session = (
                _cost_for_period(emitter, last_on.ts)
                if last_on else {"kwh": 0, "cost_bgn": 0}
            )

            units.append({
                "id":        emitter.id,
                "name":      emitter.name,
                "addr":      f"0x{emitter.short_addr:04X}",
                "hub":       hub.name,
                "online":    emitter.online,
                "last_seen": emitter.last_seen.isoformat() if emitter.last_seen else None,
                "current": {
                    "mode":        last_event.mode      if last_event else "off",
                    "setpoint_c":  last_event.setpoint_c if last_event else 0,
                    "power_on":    last_event.power_on  if last_event else False,
                    "watts":       last_event.estimated_watts if last_event else 0,
                    "outdoor_c":   last_event.outdoor_temp_c if last_event else None,
                } if last_event else {},
                "cost": {
                    "session_bgn": cost_session["cost_bgn"],
                    "session_kwh": cost_session["kwh"],
                    "today_bgn":   cost_today["cost_bgn"],
                    "today_kwh":   cost_today["kwh"],
                    "week_bgn":    cost_week["cost_bgn"],
                    "month_bgn":   cost_month["cost_bgn"],
                },
                "daily": _daily_breakdown(emitter, days=7),
            })

    outdoor_temp = get_outdoor_temp()

    return Response({
        "outdoor_temp_c": outdoor_temp,
        "units": units,
        "server_time": now.isoformat(),
    })


# ---------------------------------------------------------------------------
# Schedule CRUD
# ---------------------------------------------------------------------------

@api_view(["GET", "POST"])
def schedules(request, emitter_id):
    try:
        emitter = IREmitter.objects.get(pk=emitter_id)
    except IREmitter.DoesNotExist:
        return Response({"error": "not found"}, status=404)

    if request.method == "GET":
        try:
            s = emitter.schedule
            return Response(_schedule_to_dict(s))
        except Schedule.DoesNotExist:
            return Response({})

    # POST — create or update
    d = request.data
    s, _ = Schedule.objects.update_or_create(
        emitter=emitter,
        defaults={
            "mode":            d.get("mode", "heat"),
            "comfort_temp_c":  int(d.get("comfort_temp_c", 21)),
            "setback_temp_c":  int(d.get("setback_temp_c", 18)),
            "sleep_hour":      int(d.get("sleep_hour", 23)),
            "sleep_minute":    int(d.get("sleep_minute", 0)),
            "wake_hour":       int(d.get("wake_hour", 7)),
            "wake_minute":     int(d.get("wake_minute", 0)),
            "preheat_minutes": int(d.get("preheat_minutes", 45)),
            "enabled":         bool(d.get("enabled", True)),
        },
    )

    # Queue command for hub to pick up
    _queue_schedule_command(emitter.hub, emitter, s)

    return Response(_schedule_to_dict(s), status=status.HTTP_200_OK)


def _schedule_to_dict(s: Schedule) -> dict:
    return {
        "emitter_id":      s.emitter_id,
        "mode":            s.mode,
        "comfort_temp_c":  s.comfort_temp_c,
        "setback_temp_c":  s.setback_temp_c,
        "sleep_hour":      s.sleep_hour,
        "sleep_minute":    s.sleep_minute,
        "wake_hour":       s.wake_hour,
        "wake_minute":     s.wake_minute,
        "preheat_minutes": s.preheat_minutes,
        "enabled":         s.enabled,
    }


def _queue_schedule_command(hub: Hub, emitter: IREmitter, s: Schedule):
    PendingCommand.objects.create(
        hub=hub,
        command_type="set_schedule",
        payload={
            "addr":            emitter.short_addr,
            "mode":            s.mode,
            "comfort_temp_c":  s.comfort_temp_c,
            "setback_temp_c":  s.setback_temp_c,
            "sleep_hour":      s.sleep_hour,
            "sleep_minute":    s.sleep_minute,
            "wake_hour":       s.wake_hour,
            "wake_minute":     s.wake_minute,
            "preheat_minutes": s.preheat_minutes,
            "enabled":         s.enabled,
        },
    )


# ---------------------------------------------------------------------------
# Hub command poll — hub GETs this to pick up pending commands
# ---------------------------------------------------------------------------

@api_view(["GET"])
def poll_commands(request, hub_id):
    hub = Hub.objects.filter(identifier=hub_id).first()
    if not hub:
        return Response([])

    pending = PendingCommand.objects.filter(hub=hub, delivered=False)
    payload = [
        {"id": c.id, "type": c.command_type, "payload": c.payload}
        for c in pending
    ]
    # Mark as delivered
    pending.update(delivered=True, delivered_at=timezone.now())
    return Response(payload)


# ---------------------------------------------------------------------------
# Emitter name update
# ---------------------------------------------------------------------------

@api_view(["PATCH"])
def update_emitter(request, emitter_id):
    try:
        emitter = IREmitter.objects.get(pk=emitter_id)
    except IREmitter.DoesNotExist:
        return Response({"error": "not found"}, status=404)

    if "name" in request.data:
        emitter.name = request.data["name"][:128]
        emitter.save(update_fields=["name"])

    return Response({"id": emitter.id, "name": emitter.name})
