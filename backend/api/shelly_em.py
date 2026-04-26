"""
HTTP client and mDNS discovery for Shelly Gen2/Gen4 EM devices.

Shelly EM Mini Gen4 uses the Shelly Gen2 JSON-RPC API over HTTP:
  GET  http://{ip}/shelly              — device identification
  POST http://{ip}/rpc/EM.GetStatus    — instantaneous readings (body: {"id": 0})
  POST http://{ip}/rpc/EMData.GetStatus — cumulative energy   (body: {"id": 0})
"""

import logging
import socket

import requests

logger = logging.getLogger(__name__)

_TIMEOUT = 5  # seconds


def get_device_info(ip: str) -> dict | None:
    """Return the /shelly identification blob, or None on any error."""
    try:
        r = requests.get(f"http://{ip}/shelly", timeout=_TIMEOUT)
        r.raise_for_status()
        return r.json()
    except Exception as exc:
        logger.warning("get_device_info(%s): %s", ip, exc)
        return None


def get_em_status(ip: str) -> dict | None:
    """Return EM.GetStatus response for channel 0, or None on any error."""
    try:
        r = requests.post(
            f"http://{ip}/rpc/EM.GetStatus",
            json={"id": 0},
            timeout=_TIMEOUT,
        )
        r.raise_for_status()
        return r.json()
    except Exception as exc:
        logger.warning("get_em_status(%s): %s", ip, exc)
        return None


def get_em_data(ip: str) -> dict | None:
    """Return EMData.GetStatus response for channel 0, or None on any error."""
    try:
        r = requests.post(
            f"http://{ip}/rpc/EMData.GetStatus",
            json={"id": 0},
            timeout=_TIMEOUT,
        )
        r.raise_for_status()
        return r.json()
    except Exception as exc:
        logger.warning("get_em_data(%s): %s", ip, exc)
        return None


def _addr_to_ip(raw: bytes) -> str:
    if len(raw) == 4:
        return socket.inet_ntoa(raw)
    return socket.inet_ntop(socket.AF_INET6, raw)


def discover_mdns(timeout_s: float = 4.0) -> list[dict]:
    """
    Browse the local network for Shelly devices via mDNS (_shelly._tcp.local.).

    Returns a list of dicts with keys: mdns_name, ip, hostname.
    Each entry represents a discovered device; call get_device_info(ip) to
    obtain the full device identification before registering.
    """
    try:
        from zeroconf import ServiceBrowser, Zeroconf
    except ImportError:
        logger.error("zeroconf not installed — mDNS discovery unavailable")
        return []

    import time

    found: dict[str, dict] = {}

    class _Listener:
        def add_service(self, zc, service_type, name):
            info = zc.get_service_info(service_type, name)
            if not info:
                return
            ip = _addr_to_ip(info.addresses[0]) if info.addresses else None
            found[name] = {"mdns_name": name, "ip": ip, "hostname": info.server}

        def remove_service(self, zc, service_type, name):
            pass

        def update_service(self, zc, service_type, name):
            self.add_service(zc, service_type, name)

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, "_shelly._tcp.local.", _Listener())
        time.sleep(timeout_s)
    finally:
        zc.close()

    return list(found.values())


def reading_from_status(em_status: dict, em_data: dict | None = None) -> dict:
    """
    Convert raw EM.GetStatus (and optional EMData.GetStatus) dicts into a flat
    dict of keyword arguments suitable for ShellyEMReading(**...).
    """
    def _f(key):
        v = em_status.get(key)
        return float(v) if v is not None else None

    kwargs = {
        "a_current":    _f("a_current"),
        "a_voltage":    _f("a_voltage"),
        "a_act_power":  _f("a_act_power"),
        "a_aprt_power": _f("a_aprt_power"),
        "a_pf":         _f("a_pf"),
        "a_freq":       _f("a_freq"),
        "b_current":    _f("b_current"),
        "b_voltage":    _f("b_voltage"),
        "b_act_power":  _f("b_act_power"),
        "b_aprt_power": _f("b_aprt_power"),
        "b_pf":         _f("b_pf"),
        "b_freq":       _f("b_freq"),
        "total_act_power":  _f("total_act_power"),
        "total_aprt_power": _f("total_aprt_power"),
    }

    if em_data:
        def _fd(key):
            v = em_data.get(key)
            return float(v) if v is not None else None
        kwargs["a_energy_wh"] = _fd("a_total_act_energy")
        kwargs["b_energy_wh"] = _fd("b_total_act_energy")

    return kwargs
