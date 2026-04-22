"""
Power estimation for inverter air conditioners.

Model:
  - Heat mode: 25 W per °C of temperature difference (setpoint – outdoor).
    Calibrated against use-case data:
      setpoint=22°C, outdoor=-2°C → 24°C diff → 600W average  ✓
      setpoint=18°C, outdoor=-2°C → 20°C diff → 500W (conservative; actual
      lower due to thermal mass cycling, but a safe upper bound for cost estimates)
  - Cool mode: 30 W per °C (slightly higher COP degradation in reverse).
  - Fan/auto (no temp control): flat 150W estimate.
  - Off / power_on=False: 0W.

Clamped to [50, 3000] W to avoid nonsense values at extreme outdoor temps.
"""

from datetime import timedelta

from django.conf import settings
from django.utils import timezone as tz

_W_PER_DELTA_HEAT = 25.0
_W_PER_DELTA_COOL = 30.0
_FAN_WATTS        = 150
_MIN_WATTS        = 50
_MAX_WATTS        = 3000


def estimate_watts(mode: str, setpoint_c: int,
                   outdoor_temp_c: float, power_on: bool) -> int:
    if not power_on or mode == "off":
        return 0

    if mode == "heat":
        delta = max(0.0, setpoint_c - outdoor_temp_c)
        return max(_MIN_WATTS, min(_MAX_WATTS, int(_W_PER_DELTA_HEAT * delta)))

    if mode == "cool":
        delta = max(0.0, outdoor_temp_c - setpoint_c)
        return max(_MIN_WATTS, min(_MAX_WATTS, int(_W_PER_DELTA_COOL * delta)))

    return _FAN_WATTS  # fan / auto


def kwh_from_watt_seconds(watts: int, seconds: float) -> float:
    return watts * seconds / 3_600_000.0


def cost_eur(kwh: float) -> float:
    return round(kwh * settings.ELECTRICITY_RATE_EUR, 4)


def is_night_hour(hour: int) -> bool:
    """Return True if the given local hour falls in the cheaper night zone."""
    start = settings.NIGHT_START_HOUR  # e.g. 22
    end   = settings.NIGHT_END_HOUR    # e.g. 6
    if start > end:              # wraps midnight (the normal case)
        return hour >= start or hour < end
    return start <= hour < end   # unusual same-day window


def _next_boundary(t_local):
    """Return the next day/night tariff boundary at or after t_local."""
    boundaries = sorted({settings.NIGHT_START_HOUR, settings.NIGHT_END_HOUR})
    for b in boundaries:
        if b > t_local.hour:
            return t_local.replace(hour=b, minute=0, second=0, microsecond=0)
    # All boundaries are earlier today — wrap to first boundary tomorrow.
    return (t_local + timedelta(days=1)).replace(
        hour=boundaries[0], minute=0, second=0, microsecond=0
    )


def split_interval_by_period(t_start, t_end, watts: int) -> dict:
    """
    Split a constant-power interval [t_start, t_end) across day/night tariff
    zones.  Handles arbitrary lengths (multi-day spans fine).

    Returns:
        {
          "kwh":            float,
          "kwh_day":        float,
          "kwh_night":      float,
          "cost_eur":       float,
          "cost_day_eur":   float,
          "cost_night_eur": float,
        }
    """
    zeros = {"kwh": 0.0, "kwh_day": 0.0, "kwh_night": 0.0,
             "cost_eur": 0.0, "cost_day_eur": 0.0, "cost_night_eur": 0.0}
    if t_start >= t_end:
        return zeros

    kwh_day = 0.0
    kwh_night = 0.0

    # Work in local time so tariff hours are respected for the user's timezone.
    cursor = tz.localtime(t_start)
    t_end_local = tz.localtime(t_end)

    while cursor < t_end_local:
        boundary = _next_boundary(cursor)
        slice_end = min(boundary, t_end_local)
        duration_s = (slice_end - cursor).total_seconds()
        kwh_slice = watts * duration_s / 3_600_000.0
        if is_night_hour(cursor.hour):
            kwh_night += kwh_slice
        else:
            kwh_day += kwh_slice
        cursor = slice_end

    total_kwh   = kwh_day + kwh_night
    cost_day    = round(kwh_day   * settings.ELECTRICITY_RATE_DAY_EUR,   4)
    cost_night  = round(kwh_night * settings.ELECTRICITY_RATE_NIGHT_EUR, 4)
    return {
        "kwh":            round(total_kwh, 4),
        "kwh_day":        round(kwh_day,   4),
        "kwh_night":      round(kwh_night, 4),
        "cost_eur":       round(cost_day + cost_night, 4),
        "cost_day_eur":   cost_day,
        "cost_night_eur": cost_night,
    }


def compute_interval_cost_tou(events) -> dict:
    """
    Like compute_interval_cost() but returns ToU-split figures using
    split_interval_by_period() for each consecutive event pair.

    Returns:
        {
          "kwh": float, "kwh_day": float, "kwh_night": float,
          "cost_eur": float, "cost_day_eur": float, "cost_night_eur": float,
          "avg_watts": float,
        }
    """
    events = list(events)
    totals = {"kwh": 0.0, "kwh_day": 0.0, "kwh_night": 0.0,
              "cost_eur": 0.0, "cost_day_eur": 0.0, "cost_night_eur": 0.0}
    total_seconds = 0.0

    for i in range(len(events) - 1):
        e_now  = events[i]
        e_next = events[i + 1]
        duration_s = (e_next.ts - e_now.ts).total_seconds()
        if duration_s <= 0:
            continue
        split = split_interval_by_period(e_now.ts, e_next.ts, e_now.estimated_watts)
        for k in totals:
            totals[k] += split[k]
        total_seconds += duration_s

    avg_watts = (
        (totals["kwh"] * 3_600_000.0 / total_seconds)
        if total_seconds > 0 else 0.0
    )
    return {
        "kwh":            round(totals["kwh"],            4),
        "kwh_day":        round(totals["kwh_day"],        4),
        "kwh_night":      round(totals["kwh_night"],      4),
        "cost_eur":       round(totals["cost_eur"],       4),
        "cost_day_eur":   round(totals["cost_day_eur"],   4),
        "cost_night_eur": round(totals["cost_night_eur"], 4),
        "avg_watts":      round(avg_watts, 1),
    }


def compute_interval_cost(events) -> dict:
    """
    Given a QuerySet (or list) of ACEvent ordered by ts, compute total
    energy and cost over the period covered by the events.

    Returns:
        {"kwh": float, "cost_eur": float, "avg_watts": float}
    """
    events = list(events)
    total_wh     = 0.0
    total_seconds = 0.0

    for i in range(len(events) - 1):
        e_now  = events[i]
        e_next = events[i + 1]
        duration_s = (e_next.ts - e_now.ts).total_seconds()
        if duration_s <= 0:
            continue
        total_wh      += e_now.estimated_watts * duration_s / 3600.0
        total_seconds += duration_s

    total_kwh   = total_wh / 1000.0
    avg_watts   = (total_wh / (total_seconds / 3600.0)) if total_seconds > 0 else 0.0

    return {
        "kwh":      round(total_kwh, 4),
        "cost_eur": cost_eur(total_kwh),
        "avg_watts": round(avg_watts, 1),
    }
