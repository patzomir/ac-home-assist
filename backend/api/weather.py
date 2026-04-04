"""
Outdoor temperature via Open-Meteo (free, no API key).
Cached in-process for 10 minutes to avoid hammering the API.
"""

import time
import logging
import requests
from django.conf import settings

logger = logging.getLogger(__name__)

_OPEN_METEO_URL = "https://api.open-meteo.com/v1/forecast"
_TIMEOUT_S = 5
_CACHE_TTL  = 600  # seconds

_cache: dict = {"ts": 0, "temp_c": None}


def get_outdoor_temp(lat: float = None,
                     lon: float = None) -> float:
    lat = lat or settings.DEFAULT_LATITUDE
    lon = lon or settings.DEFAULT_LONGITUDE

    now = time.monotonic()
    if now - _cache["ts"] < _CACHE_TTL and _cache["temp_c"] is not None:
        return _cache["temp_c"]

    try:
        resp = requests.get(
            _OPEN_METEO_URL,
            params={
                "latitude":        lat,
                "longitude":       lon,
                "current_weather": "true",
            },
            timeout=_TIMEOUT_S,
        )
        resp.raise_for_status()
        temp = resp.json()["current_weather"]["temperature"]
        _cache.update({"ts": now, "temp_c": float(temp)})
        logger.debug("Outdoor temp: %.1f°C", temp)
        return float(temp)
    except Exception as exc:
        logger.warning("Weather fetch failed: %s", exc)
        return _cache.get("temp_c")  # stale value beats None
