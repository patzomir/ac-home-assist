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

from django.conf import settings

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


def cost_bgn(kwh: float) -> float:
    return round(kwh * settings.ELECTRICITY_RATE_BGN, 4)


def compute_interval_cost(events) -> dict:
    """
    Given a QuerySet (or list) of ACEvent ordered by ts, compute total
    energy and cost over the period covered by the events.

    Returns:
        {"kwh": float, "cost_bgn": float, "avg_watts": float}
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
        "cost_bgn": cost_bgn(total_kwh),
        "avg_watts": round(avg_watts, 1),
    }
