from django.db import models
from django.utils import timezone


class Hub(models.Model):
    """One per home. Identified by its MAC / UUID set in firmware."""
    identifier          = models.CharField(max_length=64, unique=True)
    name                = models.CharField(max_length=128, default="Home Hub")
    latitude            = models.FloatField(default=42.6977)
    longitude           = models.FloatField(default=23.3219)
    last_seen           = models.DateTimeField(null=True, blank=True)
    last_network_scan   = models.JSONField(null=True, blank=True)
    created_at          = models.DateTimeField(auto_now_add=True)

    def __str__(self):
        return f"{self.name} ({self.identifier})"


class IREmitter(models.Model):
    """One per AC unit. Identified by its Zigbee short address."""
    hub         = models.ForeignKey(Hub, on_delete=models.CASCADE,
                                    related_name="emitters")
    short_addr  = models.IntegerField()   # Zigbee 16-bit addr (0x0001 … 0xFFF7)
    name        = models.CharField(max_length=128, default="AC Unit")
    online      = models.BooleanField(default=False)
    last_seen   = models.DateTimeField(null=True, blank=True)

    class Meta:
        unique_together = ("hub", "short_addr")

    def __str__(self):
        return f"{self.name} (0x{self.short_addr:04X})"


class ACEvent(models.Model):
    """
    State-change event from an IR emitter.
    Hub POSTs one of these every time it sends a command.
    Consecutive events define a consumption interval:
      energy[i] = event[i].watts × (event[i+1].ts - event[i].ts)
    """
    MODE_CHOICES = [
        ("heat", "Heat"),
        ("cool", "Cool"),
        ("fan",  "Fan"),
        ("auto", "Auto"),
        ("off",  "Off"),
    ]

    emitter         = models.ForeignKey(IREmitter, on_delete=models.CASCADE,
                                         related_name="events")
    setpoint_c      = models.IntegerField(default=21)
    mode            = models.CharField(max_length=16, choices=MODE_CHOICES,
                                       default="heat")
    power_on        = models.BooleanField(default=True)
    outdoor_temp_c  = models.FloatField(null=True, blank=True)
    estimated_watts = models.IntegerField(default=0)
    ts              = models.DateTimeField(default=timezone.now, db_index=True)

    class Meta:
        ordering = ["ts"]

    def __str__(self):
        return (f"{self.emitter} @ {self.ts:%Y-%m-%d %H:%M} "
                f"{'ON' if self.power_on else 'OFF'} {self.setpoint_c}°C "
                f"{self.estimated_watts}W")


class Schedule(models.Model):
    """Night-setback schedule per emitter. Pushed to hub via PendingCommand."""
    MODE_CHOICES = [("heat", "Heat"), ("cool", "Cool")]

    emitter         = models.OneToOneField(IREmitter, on_delete=models.CASCADE,
                                            related_name="schedule")
    mode            = models.CharField(max_length=8, choices=MODE_CHOICES,
                                       default="heat")
    comfort_temp_c  = models.IntegerField(default=21)
    setback_temp_c  = models.IntegerField(default=18)
    sleep_hour      = models.IntegerField(default=23)
    sleep_minute    = models.IntegerField(default=0)
    wake_hour       = models.IntegerField(default=7)
    wake_minute     = models.IntegerField(default=0)
    preheat_minutes = models.IntegerField(default=45)
    enabled         = models.BooleanField(default=True)
    updated_at      = models.DateTimeField(auto_now=True)

    def __str__(self):
        return (f"{self.emitter} sleep={self.sleep_hour:02d}:{self.sleep_minute:02d} "
                f"→ {self.setback_temp_c}°C, "
                f"wake={self.wake_hour:02d}:{self.wake_minute:02d} "
                f"→ {self.comfort_temp_c}°C")


class SmartPlug(models.Model):
    """One per smart plug. Identified by its Zigbee short address."""
    hub         = models.ForeignKey(Hub, on_delete=models.CASCADE,
                                    related_name="plugs")
    short_addr  = models.IntegerField()   # Zigbee 16-bit addr
    name        = models.CharField(max_length=128, default="Smart Plug")
    online      = models.BooleanField(default=False)
    power_on    = models.BooleanField(default=False)
    last_seen   = models.DateTimeField(null=True, blank=True)

    class Meta:
        unique_together = ("hub", "short_addr")

    def __str__(self):
        return f"{self.name} (0x{self.short_addr:04X})"


class SmartPlugEvent(models.Model):
    """
    State-change or metering report from a smart plug.
    Consecutive events define a consumption interval:
      energy[i] = event[i].measured_watts × (event[i+1].ts - event[i].ts)
    """
    plug            = models.ForeignKey(SmartPlug, on_delete=models.CASCADE,
                                        related_name="events")
    power_on        = models.BooleanField(default=True)
    measured_watts  = models.IntegerField(default=0)
    energy_wh       = models.BigIntegerField(null=True, blank=True)  # cumulative Wh
    voltage_dv      = models.IntegerField(null=True, blank=True)     # 0.1V units
    current_ma      = models.IntegerField(null=True, blank=True)     # milliamps
    ts              = models.DateTimeField(default=timezone.now, db_index=True)

    class Meta:
        ordering = ["ts"]

    def __str__(self):
        return (f"{self.plug} @ {self.ts:%Y-%m-%d %H:%M} "
                f"{'ON' if self.power_on else 'OFF'} {self.measured_watts}W")


class ShellyEMDevice(models.Model):
    """A Shelly EM Mini Gen4 energy monitor on the local WiFi network."""
    device_id   = models.CharField(max_length=128, unique=True)   # e.g. shellyemmini-AABBCCDDEEFF
    mac         = models.CharField(max_length=32, unique=True)
    name        = models.CharField(max_length=128, default="Shelly EM Mini Gen4")
    ip_address  = models.GenericIPAddressField(protocol="IPv4", null=True, blank=True)
    online      = models.BooleanField(default=False)
    last_seen   = models.DateTimeField(null=True, blank=True)
    fw_version  = models.CharField(max_length=64, blank=True)
    created_at  = models.DateTimeField(auto_now_add=True)

    def __str__(self):
        return f"{self.name} ({self.device_id})"


class ShellyEMReading(models.Model):
    """
    Instantaneous EM reading from a ShellyEMDevice.
    Sourced either by HTTP poll or MQTT push from the device.
    Channel B fields are null for single-clamp devices.
    """
    device = models.ForeignKey(ShellyEMDevice, on_delete=models.CASCADE,
                               related_name="readings")

    # Channel A
    a_current    = models.FloatField(null=True, blank=True)   # A
    a_voltage    = models.FloatField(null=True, blank=True)   # V
    a_act_power  = models.FloatField(null=True, blank=True)   # W
    a_aprt_power = models.FloatField(null=True, blank=True)   # VA
    a_pf         = models.FloatField(null=True, blank=True)   # dimensionless
    a_freq       = models.FloatField(null=True, blank=True)   # Hz
    a_energy_wh  = models.FloatField(null=True, blank=True)   # cumulative Wh

    # Channel B (optional — only populated when a second CT clamp is fitted)
    b_current    = models.FloatField(null=True, blank=True)
    b_voltage    = models.FloatField(null=True, blank=True)
    b_act_power  = models.FloatField(null=True, blank=True)
    b_aprt_power = models.FloatField(null=True, blank=True)
    b_pf         = models.FloatField(null=True, blank=True)
    b_freq       = models.FloatField(null=True, blank=True)
    b_energy_wh  = models.FloatField(null=True, blank=True)

    total_act_power  = models.FloatField(null=True, blank=True)  # W
    total_aprt_power = models.FloatField(null=True, blank=True)  # VA

    ts = models.DateTimeField(default=timezone.now, db_index=True)

    class Meta:
        ordering = ["ts"]

    def __str__(self):
        return (f"{self.device} @ {self.ts:%Y-%m-%d %H:%M} "
                f"{self.total_act_power}W")


class PendingCommand(models.Model):
    """
    Backend-to-hub command queue.
    Hub polls GET /api/commands/{hub_id}/ and marks commands delivered.
    """
    TYPE_CHOICES = [
        ("set_schedule",   "Set Schedule"),
        ("set_ac",         "Set AC State"),
        ("set_plug",       "Set Plug State"),
        ("scan_network",   "Scan Zigbee Network"),
    ]

    hub          = models.ForeignKey(Hub, on_delete=models.CASCADE,
                                     related_name="commands")
    command_type = models.CharField(max_length=32, choices=TYPE_CHOICES)
    payload      = models.JSONField()
    created_at   = models.DateTimeField(auto_now_add=True)
    delivered    = models.BooleanField(default=False)
    delivered_at = models.DateTimeField(null=True, blank=True)

    class Meta:
        ordering = ["created_at"]

    def __str__(self):
        return f"{self.command_type} → {self.hub} (delivered={self.delivered})"
