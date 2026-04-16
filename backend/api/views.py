import calendar
import logging
from datetime import timedelta

from django.utils import timezone
from rest_framework import status
from rest_framework.decorators import api_view
from rest_framework.response import Response

from . import mqtt_manager
from django.conf import settings as django_settings

from .energy_model import (
    compute_interval_cost,
    compute_interval_cost_tou,
    estimate_watts,
    is_night_hour,
    split_interval_by_period,
)
from .models import ACEvent, Hub, IREmitter, PendingCommand, Schedule, SmartPlug, SmartPlugEvent
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
    now = timezone.localtime(timezone.now())
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


def _plug_cost_for_period(plug: SmartPlug, since, until=None):
    until = until or timezone.now()
    events = list(
        SmartPlugEvent.objects
        .filter(plug=plug, ts__gte=since, ts__lte=until)
        .order_by("ts")
    )
    if events:
        last = events[-1]
        synthetic = SmartPlugEvent(
            plug=plug,
            measured_watts=last.measured_watts,
            ts=until,
        )
        events.append(synthetic)
    # Reuse compute_interval_cost — it reads .estimated_watts; alias the field.
    for e in events:
        e.estimated_watts = e.measured_watts
    return compute_interval_cost(events)


def _plug_daily_breakdown(plug: SmartPlug, days=7):
    result = []
    now = timezone.localtime(timezone.now())
    for d in range(days - 1, -1, -1):
        day_start = (now - timedelta(days=d)).replace(
            hour=0, minute=0, second=0, microsecond=0)
        day_end = day_start + timedelta(days=1)
        cost = _plug_cost_for_period(plug, day_start, day_end)
        result.append({
            "date":     day_start.strftime("%a %d/%m"),
            "kwh":      cost["kwh"],
            "cost_bgn": cost["cost_bgn"],
        })
    return result


def _cost_for_period_tou(emitter: IREmitter, since, until=None):
    until = until or timezone.now()
    events = list(
        ACEvent.objects
        .filter(emitter=emitter, ts__gte=since, ts__lte=until)
        .order_by("ts")
    )
    if events:
        last = events[-1]
        events.append(ACEvent(emitter=emitter, estimated_watts=last.estimated_watts, ts=until))
    return compute_interval_cost_tou(events)


def _plug_cost_for_period_tou(plug: SmartPlug, since, until=None):
    until = until or timezone.now()
    events = list(
        SmartPlugEvent.objects
        .filter(plug=plug, ts__gte=since, ts__lte=until)
        .order_by("ts")
    )
    if events:
        last = events[-1]
        events.append(SmartPlugEvent(plug=plug, measured_watts=last.measured_watts, ts=until))
    for e in events:
        e.estimated_watts = e.measured_watts
    return compute_interval_cost_tou(events)


def _plug_cost_for_period_wh(plug: SmartPlug, since, until=None):
    """
    Cost for [since, until] using energy_wh counter differences.

    For each consecutive pair of energy_wh readings the day/night split is
    determined by the fraction of wall-clock time that falls in each tariff
    zone (via split_interval_by_period).  Falls back to the measured_watts
    estimate when energy_wh data is insufficient.
    """
    until = until or timezone.now()

    events_with_wh = list(
        SmartPlugEvent.objects
        .filter(plug=plug, energy_wh__isnull=False, ts__lte=until)
        .order_by("ts")
    )

    before_since = [e for e in events_with_wh if e.ts <= since]
    in_range     = [e for e in events_with_wh if e.ts >  since]

    # Anchor the sequence at 'since' using the last known reading before it.
    if before_since:
        sequence = [(since, before_since[-1].energy_wh)] + \
                   [(e.ts, e.energy_wh) for e in in_range]
    else:
        sequence = [(e.ts, e.energy_wh) for e in in_range]

    if len(sequence) < 2:
        return _plug_cost_for_period_tou(plug, since, until)

    kwh_day = 0.0
    kwh_night = 0.0

    for i in range(len(sequence) - 1):
        t1, wh1 = sequence[i]
        t2, wh2 = sequence[i + 1]
        delta_wh = wh2 - wh1
        if delta_wh <= 0:
            continue
        # split_interval_by_period gives us day/night time fractions.
        split = split_interval_by_period(t1, t2, 1000)
        frac_kwh = split["kwh"]
        if frac_kwh > 0:
            frac_day   = split["kwh_day"]   / frac_kwh
            frac_night = split["kwh_night"] / frac_kwh
        else:
            local_t1 = timezone.localtime(t1)
            frac_day, frac_night = (0.0, 1.0) if is_night_hour(local_t1.hour) else (1.0, 0.0)
        actual_kwh = delta_wh / 1000.0
        kwh_day   += actual_kwh * frac_day
        kwh_night += actual_kwh * frac_night

    rate_day   = django_settings.ELECTRICITY_RATE_DAY_BGN
    rate_night = django_settings.ELECTRICITY_RATE_NIGHT_BGN
    total_kwh  = kwh_day + kwh_night
    return {
        "kwh":            round(total_kwh,  4),
        "kwh_day":        round(kwh_day,    4),
        "kwh_night":      round(kwh_night,  4),
        "cost_bgn":       round(kwh_day * rate_day + kwh_night * rate_night, 4),
        "cost_day_bgn":   round(kwh_day   * rate_day,   4),
        "cost_night_bgn": round(kwh_night * rate_night, 4),
        "avg_watts":      0.0,
    }


def _daily_breakdown_tou(emitter: IREmitter, days=7):
    """Return list of {date, kwh, cost_bgn, day_kwh, day_cost_bgn, night_kwh, night_cost_bgn}."""
    result = []
    now = timezone.localtime(timezone.now())
    for d in range(days - 1, -1, -1):
        day_start = (now - timedelta(days=d)).replace(hour=0, minute=0, second=0, microsecond=0)
        day_end = min(day_start + timedelta(days=1), now)
        cost = _cost_for_period_tou(emitter, day_start, day_end)
        result.append({
            "date":           day_start.strftime("%a %d/%m"),
            "kwh":            cost["kwh"],
            "cost_bgn":       cost["cost_bgn"],
            "day_kwh":        cost["kwh_day"],
            "day_cost_bgn":   cost["cost_day_bgn"],
            "night_kwh":      cost["kwh_night"],
            "night_cost_bgn": cost["cost_night_bgn"],
        })
    return result


def _plug_daily_breakdown_tou(plug: SmartPlug, days=7):
    rate_day      = django_settings.ELECTRICITY_RATE_DAY_BGN
    rate_night    = django_settings.ELECTRICITY_RATE_NIGHT_BGN
    night_start_h = django_settings.NIGHT_START_HOUR
    night_end_h   = django_settings.NIGHT_END_HOUR

    # Pre-fetch all events that carry the cumulative energy counter.
    wh_events = list(
        SmartPlugEvent.objects
        .filter(plug=plug, energy_wh__isnull=False)
        .order_by("ts")
    )

    def last_wh_at(t):
        """Most recent energy_wh reading at or before time t."""
        return next((e.energy_wh for e in reversed(wh_events) if e.ts <= t), None)

    def wh_diff_kwh(t1, t2):
        """kWh consumed between t1 and t2 from the counter, or None if unavailable."""
        if t1 >= t2:
            return 0.0
        w1, w2 = last_wh_at(t1), last_wh_at(t2)
        if w1 is None or w2 is None or w2 < w1:
            return None
        return (w2 - w1) / 1000.0

    result = []
    now = timezone.localtime(timezone.now())
    for d in range(days - 1, -1, -1):
        day_start = (now - timedelta(days=d)).replace(hour=0, minute=0, second=0, microsecond=0)
        day_end   = min(day_start + timedelta(days=1), now)

        # Split the calendar day into its three tariff segments:
        #   night₁  00:00 → NIGHT_END_HOUR   (e.g. 06:00)
        #   day     NIGHT_END_HOUR → NIGHT_START_HOUR  (e.g. 06:00–22:00)
        #   night₂  NIGHT_START_HOUR → 24:00
        t_night1_end = min(day_start.replace(hour=night_end_h,   minute=0, second=0, microsecond=0), day_end)
        t_day_end    = min(day_start.replace(hour=night_start_h, minute=0, second=0, microsecond=0), day_end)

        night1_kwh = wh_diff_kwh(day_start,    t_night1_end)
        day_kwh    = wh_diff_kwh(t_night1_end, t_day_end)
        night2_kwh = wh_diff_kwh(t_day_end,    day_end)

        if any(x is not None for x in [night1_kwh, day_kwh, night2_kwh]):
            kwh_night = (night1_kwh or 0.0) + (night2_kwh or 0.0)
            kwh_day   = day_kwh or 0.0
            result.append({
                "date":           day_start.strftime("%a %d/%m"),
                "kwh":            round(kwh_night + kwh_day, 4),
                "cost_bgn":       round(kwh_day * rate_day + kwh_night * rate_night, 4),
                "day_kwh":        round(kwh_day, 4),
                "day_cost_bgn":   round(kwh_day * rate_day, 4),
                "night_kwh":      round(kwh_night, 4),
                "night_cost_bgn": round(kwh_night * rate_night, 4),
            })
        else:
            # Fall back to measured_watts-based estimate when energy_wh is absent.
            cost = _plug_cost_for_period_tou(plug, day_start, day_end)
            result.append({
                "date":           day_start.strftime("%a %d/%m"),
                "kwh":            cost["kwh"],
                "cost_bgn":       cost["cost_bgn"],
                "day_kwh":        cost["kwh_day"],
                "day_cost_bgn":   cost["cost_day_bgn"],
                "night_kwh":      cost["kwh_night"],
                "night_cost_bgn": cost["cost_night_bgn"],
            })
    return result


def _hourly_breakdown_ac(emitter: IREmitter, hours=24):
    """Return list of {hour, kwh, cost_bgn, period, partial} for past N hours."""
    result = []
    now = timezone.now()
    # Floor to current hour start
    current_hour_start = now.replace(minute=0, second=0, microsecond=0)

    for h in range(hours - 1, -1, -1):
        slot_start = current_hour_start - timedelta(hours=h)
        slot_end   = slot_start + timedelta(hours=1)
        is_partial = (h == 0)
        if is_partial:
            slot_end = now

        # Fetch events spanning this slot (include last event before slot for leading power state)
        events = list(
            ACEvent.objects
            .filter(emitter=emitter, ts__lt=slot_end)
            .order_by("ts")
        )
        # Only keep events within and the one immediately before the slot
        slot_events = [e for e in events if e.ts >= slot_start]
        leading = next((e for e in reversed(events) if e.ts < slot_start), None)
        if leading:
            # Clone with ts = slot_start so the interval starts from there
            stub = ACEvent(emitter=emitter, estimated_watts=leading.estimated_watts, ts=slot_start)
            slot_events = [stub] + slot_events
        if slot_events:
            slot_events.append(ACEvent(
                emitter=emitter,
                estimated_watts=slot_events[-1].estimated_watts,
                ts=slot_end,
            ))

        cost = compute_interval_cost_tou(slot_events)
        local_slot = timezone.localtime(slot_start)
        result.append({
            "hour":     local_slot.strftime("%H:00"),
            "kwh":      cost["kwh"],
            "cost_bgn": cost["cost_bgn"],
            "period":   "night" if is_night_hour(local_slot.hour) else "day",
            "partial":  is_partial,
        })
    return result


def _hourly_breakdown_plug(plug: SmartPlug, hours=24):
    rate_day   = django_settings.ELECTRICITY_RATE_DAY_BGN
    rate_night = django_settings.ELECTRICITY_RATE_NIGHT_BGN
    result = []
    now = timezone.now()
    current_hour_start = now.replace(minute=0, second=0, microsecond=0)

    # Pre-fetch all events with energy_wh to avoid per-slot queries.
    wh_events = list(
        SmartPlugEvent.objects
        .filter(plug=plug, energy_wh__isnull=False)
        .order_by("ts")
    )

    for h in range(hours - 1, -1, -1):
        slot_start = current_hour_start - timedelta(hours=h)
        slot_end   = slot_start + timedelta(hours=1)
        is_partial = (h == 0)
        if is_partial:
            slot_end = now

        local_slot = timezone.localtime(slot_start)
        is_night   = is_night_hour(local_slot.hour)

        # Try energy_wh counter difference first (more reliable than measured_watts).
        start_wh = next((e.energy_wh for e in reversed(wh_events) if e.ts <= slot_start), None)
        end_wh   = next((e.energy_wh for e in reversed(wh_events) if e.ts <= slot_end),   None)

        if start_wh is not None and end_wh is not None and end_wh >= start_wh:
            kwh      = round((end_wh - start_wh) / 1000.0, 4)
            rate     = rate_night if is_night else rate_day
            cost_bgn = round(kwh * rate, 4)
        else:
            # Fall back to measured_watts-based estimate.
            events = list(
                SmartPlugEvent.objects
                .filter(plug=plug, ts__lt=slot_end)
                .order_by("ts")
            )
            slot_events = [e for e in events if e.ts >= slot_start]
            leading = next((e for e in reversed(events) if e.ts < slot_start), None)
            if leading:
                stub = SmartPlugEvent(plug=plug, measured_watts=leading.measured_watts, ts=slot_start)
                slot_events = [stub] + slot_events
            if slot_events:
                slot_events.append(SmartPlugEvent(
                    plug=plug,
                    measured_watts=slot_events[-1].measured_watts,
                    ts=slot_end,
                ))
            for e in slot_events:
                e.estimated_watts = e.measured_watts
            c        = compute_interval_cost_tou(slot_events)
            kwh      = c["kwh"]
            cost_bgn = c["cost_bgn"]

        result.append({
            "hour":     local_slot.strftime("%H:00"),
            "kwh":      kwh,
            "cost_bgn": cost_bgn,
            "period":   "night" if is_night else "day",
            "partial":  is_partial,
        })
    return result


def _recommendations(devices_summary: list) -> list:
    """
    Rule-based recommendations.  devices_summary is a list of dicts:
      {name, type, kwh_day, kwh_night, has_schedule, preheat_starts_in_day}
    Returns up to 5 items sorted by potential_saving_bgn descending.
    """
    rate_day   = django_settings.ELECTRICITY_RATE_DAY_BGN
    rate_night = django_settings.ELECTRICITY_RATE_NIGHT_BGN
    savings_diff = rate_day - rate_night
    night_end = django_settings.NIGHT_END_HOUR

    recs = []

    total_day_kwh   = sum(d["kwh_day"]   for d in devices_summary)
    total_night_kwh = sum(d["kwh_night"] for d in devices_summary)
    total_kwh = total_day_kwh + total_night_kwh

    for d in devices_summary:
        device_total = d["kwh_day"] + d["kwh_night"]
        if device_total == 0:
            continue

        # Rule 1 / 3: shift peak usage to night
        if d["kwh_day"] / device_total > 0.3:
            monthly_shiftable_kwh = d["kwh_day"] * (30 / 7)
            saving = round(monthly_shiftable_kwh * savings_diff, 2)
            if saving >= 0.50:
                pct = int(d["kwh_day"] / device_total * 100)
                recs.append({
                    "type": "shift_ac" if d["type"] == "ac" else "shift_plug",
                    "message": (
                        f"{pct}% of {d['name']} usage is during peak hours — "
                        f"shifting to night could save {saving:.2f} BGN/month"
                    ),
                    "potential_saving_bgn": saving,
                    "device_name": d["name"],
                })

        # Rule 2: preheat tip (AC only)
        if d["type"] == "ac" and d.get("preheat_starts_in_day"):
            # Estimate saving: preheat_minutes * avg_watts / 1000 / 60 * savings_diff * 30
            preheat_kwh = d.get("preheat_kwh_per_day", 0)
            saving = round(preheat_kwh * 30 * savings_diff, 2)
            if saving >= 0.50:
                recs.append({
                    "type": "preheat_tip",
                    "message": (
                        f"Schedule preheat for {d['name']} to start before "
                        f"{night_end:02d}:00 to use cheaper night electricity"
                    ),
                    "potential_saving_bgn": saving,
                    "device_name": d["name"],
                })

    # Rule 4: global tip
    if total_kwh > 0 and total_night_kwh / total_kwh < 0.2:
        monthly_total_kwh = total_kwh * (30 / 7)
        saving = round(monthly_total_kwh * total_day_kwh / total_kwh * savings_diff, 2)
        if saving >= 0.50:
            recs.append({
                "type": "global_tip",
                "message": (
                    f"Less than 20% of your total usage happens at night — "
                    f"shifting loads could save up to {saving:.2f} BGN/month"
                ),
                "potential_saving_bgn": saving,
                "device_name": None,
            })

    recs.sort(key=lambda r: r["potential_saving_bgn"], reverse=True)
    return recs[:5]


def _billing_cycle_bounds(now, cycle_start_day):
    """
    Return (cycle_start_dt, cycle_end_dt, total_days, days_elapsed) for the
    current billing cycle whose start day-of-month is *cycle_start_day* (1-28).
    """
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    day = cycle_start_day

    if today.day >= day:
        cycle_start = today.replace(day=day)
        # Next occurrence: same day-of-month next calendar month.
        if today.month == 12:
            cycle_end = cycle_start.replace(year=today.year + 1, month=1)
        else:
            cycle_end = cycle_start.replace(month=today.month + 1)
    else:
        # Cycle started last month — clamp to last day of that month.
        first_of_this = today.replace(day=1)
        last_of_prev = first_of_this - timedelta(days=1)
        actual_day = min(day, calendar.monthrange(last_of_prev.year, last_of_prev.month)[1])
        cycle_start = last_of_prev.replace(day=actual_day)
        cycle_end = today.replace(day=day)

    total_days = (cycle_end - cycle_start).days
    days_elapsed = (now - cycle_start).total_seconds() / 86400
    return cycle_start, cycle_end, total_days, days_elapsed


@api_view(["GET"])
def dashboard_data(request):
    """
    Returns all data needed to render the dashboard.
    Called every 30 s by the frontend.
    """
    now = timezone.now()
    hubs = Hub.objects.prefetch_related("emitters").all()

    cycle_start_day = max(1, min(28, int(request.query_params.get("cycle_start", 1))))
    cycle_start_dt, cycle_end_dt, cycle_total_days, cycle_days_elapsed = (
        _billing_cycle_bounds(now, cycle_start_day)
    )

    units = []
    _devices_summary_list = []
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

            cost_today  = _cost_for_period_tou(emitter, today_start)
            cost_week   = _cost_for_period_tou(emitter, week_start)
            cost_month  = _cost_for_period_tou(emitter, month_start)

            # Billing cycle cost + projection
            cost_cycle = _cost_for_period_tou(emitter, cycle_start_dt)
            cycle_projected = (
                round(cost_cycle["cost_bgn"] / cycle_days_elapsed * cycle_total_days, 4)
                if cycle_days_elapsed >= 0.5 else None
            )

            # Session cost (since last ON event)
            last_on = (
                ACEvent.objects.filter(emitter=emitter, power_on=True)
                .order_by("-ts").first()
            )
            cost_session = (
                _cost_for_period_tou(emitter, last_on.ts)
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
                    "session_bgn":         cost_session["cost_bgn"],
                    "session_kwh":         cost_session["kwh"],
                    "today_bgn":           cost_today["cost_bgn"],
                    "today_kwh":           cost_today["kwh"],
                    "today_day_bgn":       cost_today["cost_day_bgn"],
                    "today_night_bgn":     cost_today["cost_night_bgn"],
                    "today_day_kwh":       cost_today["kwh_day"],
                    "today_night_kwh":     cost_today["kwh_night"],
                    "week_bgn":            cost_week["cost_bgn"],
                    "week_day_bgn":        cost_week["cost_day_bgn"],
                    "week_night_bgn":      cost_week["cost_night_bgn"],
                    "week_day_kwh":        cost_week["kwh_day"],
                    "week_night_kwh":      cost_week["kwh_night"],
                    "month_bgn":           cost_month["cost_bgn"],
                    "cycle_bgn":           cost_cycle["cost_bgn"],
                    "cycle_projected_bgn": cycle_projected,
                },
                "daily":  _daily_breakdown_tou(emitter, days=7),
                "hourly": _hourly_breakdown_ac(emitter, hours=24),
            })

            # Accumulate for recommendations
            try:
                sched = emitter.schedule
                has_schedule = sched.enabled
                # preheat start minute of day
                wake_total_min = sched.wake_hour * 60 + sched.wake_minute
                preheat_start_min = wake_total_min - sched.preheat_minutes
                night_end_min = django_settings.NIGHT_END_HOUR * 60
                preheat_starts_in_day = has_schedule and preheat_start_min >= night_end_min
                # Estimate kWh for preheat window
                avg_w = cost_week["avg_watts"] if cost_week["kwh"] > 0 else 300
                preheat_kwh = avg_w * sched.preheat_minutes / 60 / 1000
            except Schedule.DoesNotExist:
                has_schedule = False
                preheat_starts_in_day = False
                preheat_kwh = 0

            _devices_summary_list.append({
                "name":               emitter.name,
                "type":               "ac",
                "kwh_day":            cost_week["kwh_day"],
                "kwh_night":          cost_week["kwh_night"],
                "has_schedule":       has_schedule,
                "preheat_starts_in_day": preheat_starts_in_day,
                "preheat_kwh_per_day":   preheat_kwh,
            })

    outdoor_temp = get_outdoor_temp()

    # Build a per-hub set of addresses from the last network scan (if any).
    hub_scan_addrs = {
        hub.id: set(hub.last_network_scan["addrs"])
        if hub.last_network_scan else None
        for hub in hubs
    }

    plugs = []
    for hub in hubs:
        scan_addrs = hub_scan_addrs[hub.id]
        for plug in hub.plugs.all():
            last_event = (
                SmartPlugEvent.objects.filter(plug=plug).order_by("-ts").first()
            )

            today_start = now.replace(hour=0, minute=0, second=0, microsecond=0)
            week_start  = today_start - timedelta(days=now.weekday())
            month_start = today_start.replace(day=1)

            cost_today  = _plug_cost_for_period_wh(plug, today_start)
            cost_week   = _plug_cost_for_period_wh(plug, week_start)
            cost_month  = _plug_cost_for_period_wh(plug, month_start)

            cost_cycle = _plug_cost_for_period_wh(plug, cycle_start_dt)
            cycle_projected = (
                round(cost_cycle["cost_bgn"] / cycle_days_elapsed * cycle_total_days, 4)
                if cycle_days_elapsed >= 0.5 else None
            )

            last_on = (
                SmartPlugEvent.objects.filter(plug=plug, power_on=True)
                .order_by("-ts").first()
            )
            cost_session = (
                _plug_cost_for_period_wh(plug, last_on.ts)
                if last_on else {"kwh": 0, "cost_bgn": 0}
            )

            plug_online = bool(
                plug.last_seen and (now - plug.last_seen).total_seconds() < 300
            )
            in_last_scan = (
                plug.short_addr in scan_addrs if scan_addrs is not None else None
            )
            plugs.append({
                "id":           plug.id,
                "name":         plug.name,
                "addr":         f"0x{plug.short_addr:04X}",
                "hub":          hub.name,
                "online":       plug_online,
                "power_on":     plug.power_on,
                "last_seen":    plug.last_seen.isoformat() if plug.last_seen else None,
                "in_last_scan": in_last_scan,
                "current": {
                    "watts": last_event.measured_watts if last_event else 0,
                },
                "cost": {
                    "session_bgn":         cost_session["cost_bgn"],
                    "session_kwh":         cost_session["kwh"],
                    "today_bgn":           cost_today["cost_bgn"],
                    "today_kwh":           cost_today["kwh"],
                    "today_day_bgn":       cost_today["cost_day_bgn"],
                    "today_night_bgn":     cost_today["cost_night_bgn"],
                    "today_day_kwh":       cost_today["kwh_day"],
                    "today_night_kwh":     cost_today["kwh_night"],
                    "week_bgn":            cost_week["cost_bgn"],
                    "week_day_bgn":        cost_week["cost_day_bgn"],
                    "week_night_bgn":      cost_week["cost_night_bgn"],
                    "week_day_kwh":        cost_week["kwh_day"],
                    "week_night_kwh":      cost_week["kwh_night"],
                    "month_bgn":           cost_month["cost_bgn"],
                    "cycle_bgn":           cost_cycle["cost_bgn"],
                    "cycle_projected_bgn": cycle_projected,
                },
                "daily":  _plug_daily_breakdown_tou(plug, days=7),
                "hourly": _hourly_breakdown_plug(plug, hours=24),
            })
            _devices_summary_list.append({
                "name":               plug.name,
                "type":               "plug",
                "kwh_day":            cost_week["kwh_day"],
                "kwh_night":          cost_week["kwh_night"],
                "has_schedule":       False,
                "preheat_starts_in_day": False,
                "preheat_kwh_per_day":   0,
            })

    hubs_list = []
    for hub in hubs:
        is_online = hub.last_seen and (now - hub.last_seen).total_seconds() < 300
        scan = hub.last_network_scan  # {ts, addrs} or None
        known_addrs = {p.short_addr for p in hub.plugs.all()}
        unknown = (
            [a for a in scan["addrs"] if a not in known_addrs]
            if scan else []
        )
        hubs_list.append({
            "id":            hub.id,
            "name":          hub.name,
            "identifier":    hub.identifier,
            "online":        bool(is_online),
            "last_seen":     hub.last_seen.isoformat() if hub.last_seen else None,
            "emitter_count": hub.emitters.count(),
            "plug_count":    hub.plugs.count(),
            "last_scan": {
                "ts":      scan["ts"],
                "count":   len(scan["addrs"]),
                "unknown": unknown,
            } if scan else None,
        })

    return Response({
        "outdoor_temp_c": outdoor_temp,
        "hubs":  hubs_list,
        "units": units,
        "plugs": plugs,
        "server_time": now.isoformat(),
        "cycle": {
            "start_day":     cycle_start_day,
            "start_date":    cycle_start_dt.date().isoformat(),
            "end_date":      cycle_end_dt.date().isoformat(),
            "total_days":    cycle_total_days,
            "days_elapsed":  round(cycle_days_elapsed, 1),
        },
        "tariff": {
            "rate_day_bgn":   django_settings.ELECTRICITY_RATE_DAY_BGN,
            "rate_night_bgn": django_settings.ELECTRICITY_RATE_NIGHT_BGN,
            "night_start":    django_settings.NIGHT_START_HOUR,
            "night_end":      django_settings.NIGHT_END_HOUR,
        },
        "recommendations": _recommendations(_devices_summary_list),
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
    payload = {
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
    }
    cmd = PendingCommand.objects.create(
        hub=hub,
        command_type="set_schedule",
        payload=payload,
    )
    mqtt_manager.publish_command(hub.identifier, cmd.id, cmd.command_type, payload)


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

def _delete_emitter(emitter):
    """Clear hub NVS schedule via MQTT then remove the DB record."""
    try:
        emitter.schedule  # noqa: just checking existence
        clear_payload = {"addr": emitter.short_addr, "enabled": False}
        cmd = PendingCommand.objects.create(
            hub=emitter.hub,
            command_type="set_schedule",
            payload=clear_payload,
        )
        mqtt_manager.publish_command(
            emitter.hub.identifier, cmd.id, cmd.command_type, clear_payload
        )
    except Schedule.DoesNotExist:
        pass
    emitter.delete()


@api_view(["PATCH", "DELETE"])
def update_emitter(request, emitter_id):
    try:
        emitter = IREmitter.objects.get(pk=emitter_id)
    except IREmitter.DoesNotExist:
        return Response({"error": "not found"}, status=404)

    if request.method == "DELETE":
        _delete_emitter(emitter)
        return Response(status=status.HTTP_204_NO_CONTENT)

    if "name" in request.data:
        emitter.name = request.data["name"][:128]
        emitter.save(update_fields=["name"])

    return Response({"id": emitter.id, "name": emitter.name})


@api_view(["DELETE"])
def delete_emitter_by_addr(request, hub_id, addr):
    """
    Delete an IR emitter by Zigbee short address (hex like 0x8f87 or decimal).
    If the emitter is not in the DB, still sends a schedule-clear to the hub NVS.
    """
    try:
        short_addr = int(addr, 16) if addr.lower().startswith("0x") else int(addr)
    except ValueError:
        return Response({"error": "invalid addr"}, status=400)

    hub = Hub.objects.filter(identifier=hub_id).first()
    if not hub:
        return Response({"error": "hub not found"}, status=404)

    try:
        emitter = IREmitter.objects.get(hub=hub, short_addr=short_addr)
        _delete_emitter(emitter)
    except IREmitter.DoesNotExist:
        # Not in DB but may still have a schedule in NVS — clear it
        clear_payload = {"addr": short_addr, "enabled": False}
        cmd = PendingCommand.objects.create(
            hub=hub,
            command_type="set_schedule",
            payload=clear_payload,
        )
        mqtt_manager.publish_command(hub.identifier, cmd.id, cmd.command_type, clear_payload)

    return Response(status=status.HTTP_204_NO_CONTENT)


# ---------------------------------------------------------------------------
# Smart plug events — hub POSTs here on every plug state / metering report
# ---------------------------------------------------------------------------

@api_view(["POST"])
def receive_plug_event(request):
    """
    Payload:
    {
        "addr":       2,          // Zigbee short address (int)
        "power":      true,       // plug on/off
        "watts":      120,        // measured power in W (optional, 0 when off)
        "energy_wh":  "1500000",  // cumulative energy in Wh (optional, string)
        "voltage_dv": 2300,       // voltage in 0.1V units (optional)
        "current_ma": 500,        // current in milliamps (optional)
        "ts":         1712345678  // unix timestamp (optional)
    }
    Hub identifier is sent in X-Hub-Id header.
    """
    data   = request.data
    hub_id = request.headers.get("X-Hub-Id", "unknown")
    hub, _ = Hub.objects.get_or_create(
        identifier=hub_id,
        defaults={"name": f"Hub {hub_id[:8]}"},
    )
    hub.last_seen = timezone.now()
    hub.save(update_fields=["last_seen"])

    short_addr = int(data.get("addr", 0))
    plug, _ = SmartPlug.objects.get_or_create(
        hub=hub,
        short_addr=short_addr,
        defaults={"name": f"Plug 0x{short_addr:04X}"},
    )

    power_on       = bool(data.get("power", True))
    measured_watts = int(data.get("watts", 0)) if data.get("watts") is not None else None
    energy_wh      = int(data["energy_wh"]) if data.get("energy_wh") is not None else None
    voltage_dv     = int(data["voltage_dv"]) if data.get("voltage_dv") is not None else None
    current_ma     = int(data["current_ma"]) if data.get("current_ma") is not None else None

    plug.online    = True
    plug.power_on  = power_on
    plug.last_seen = timezone.now()
    plug.save(update_fields=["online", "power_on", "last_seen"])

    ts = timezone.now()
    if data.get("ts"):
        from datetime import datetime
        ts = datetime.utcfromtimestamp(int(data["ts"])).replace(tzinfo=timezone.utc)

    # Merge partial readings from the same poll cycle into one row.
    # The firmware sends each ZCL cluster response as a separate POST, so
    # multiple requests arrive within seconds of each other. Coalesce them
    # into the most recent event within the last 60 s instead of creating
    # a new sparse row every time.
    MERGE_WINDOW_S = 60
    window_start = ts - timezone.timedelta(seconds=MERGE_WINDOW_S)
    existing = (SmartPlugEvent.objects
                .filter(plug=plug, ts__gte=window_start)
                .order_by("-ts")
                .first())

    if existing:
        update_fields = []
        if measured_watts is not None:
            existing.measured_watts = measured_watts
            update_fields.append("measured_watts")
        if energy_wh is not None:
            existing.energy_wh = energy_wh
            update_fields.append("energy_wh")
        if voltage_dv is not None:
            existing.voltage_dv = voltage_dv
            update_fields.append("voltage_dv")
        if current_ma is not None:
            existing.current_ma = current_ma
            update_fields.append("current_ma")
        if update_fields:
            existing.save(update_fields=update_fields)
        event = existing
    else:
        event = SmartPlugEvent.objects.create(
            plug=plug,
            power_on=power_on,
            measured_watts=measured_watts or 0,
            energy_wh=energy_wh,
            voltage_dv=voltage_dv,
            current_ma=current_ma,
            ts=ts,
        )

    logger.info("Plug event from hub=%s addr=0x%04X power=%s %dW energy=%s Wh",
                hub_id, short_addr, power_on, event.measured_watts, event.energy_wh)

    return Response({"ok": True}, status=status.HTTP_201_CREATED)


# ---------------------------------------------------------------------------
# Smart plug control — toggle on/off via pending command queue
# ---------------------------------------------------------------------------

@api_view(["POST"])
def control_plug(request, plug_id):
    """
    Body: {"power": true/false}
    Queues a set_plug command for the hub to pick up.
    """
    try:
        plug = SmartPlug.objects.select_related("hub").get(pk=plug_id)
    except SmartPlug.DoesNotExist:
        return Response({"error": "not found"}, status=404)

    power_on = bool(request.data.get("power", True))
    payload = {"addr": plug.short_addr, "power": power_on}

    cmd = PendingCommand.objects.create(
        hub=plug.hub,
        command_type="set_plug",
        payload=payload,
    )
    mqtt_manager.publish_command(plug.hub.identifier, cmd.id, cmd.command_type, payload)

    return Response({"queued": True, "power": power_on})


# ---------------------------------------------------------------------------
# Hub name update
# ---------------------------------------------------------------------------

@api_view(["PATCH"])
def update_hub(request, hub_pk):
    try:
        hub = Hub.objects.get(pk=hub_pk)
    except Hub.DoesNotExist:
        return Response({"error": "not found"}, status=404)

    if "name" in request.data:
        hub.name = request.data["name"][:128]
        hub.save(update_fields=["name"])

    return Response({"id": hub.id, "name": hub.name})


@api_view(["POST"])
def scan_hub_network(request, hub_pk):
    """Queue a scan_network command for the hub.

    The hub will respond on hub/{hub_id}/network with its active Zigbee
    addresses. mqtt_manager will reconcile SmartPlug records on arrival.
    """
    try:
        hub = Hub.objects.get(pk=hub_pk)
    except Hub.DoesNotExist:
        return Response({"error": "not found"}, status=404)

    cmd = PendingCommand.objects.create(
        hub=hub,
        command_type="scan_network",
        payload={},
    )
    mqtt_manager.publish_command(hub.identifier, cmd.id, cmd.command_type, {})
    return Response({"queued": True})


# ---------------------------------------------------------------------------
# Smart plug name update
# ---------------------------------------------------------------------------

@api_view(["PATCH", "DELETE"])
def update_plug(request, plug_id):
    try:
        plug = SmartPlug.objects.get(pk=plug_id)
    except SmartPlug.DoesNotExist:
        return Response({"error": "not found"}, status=404)

    if request.method == "DELETE":
        plug.delete()
        return Response(status=status.HTTP_204_NO_CONTENT)

    if "name" in request.data:
        plug.name = request.data["name"][:128]
        plug.save(update_fields=["name"])

    return Response({"id": plug.id, "name": plug.name})
