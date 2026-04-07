from django.contrib import admin

from .models import ACEvent, Hub, IREmitter, PendingCommand, Schedule, SmartPlug, SmartPlugEvent


@admin.register(Hub)
class HubAdmin(admin.ModelAdmin):
    list_display = ("name", "identifier", "last_seen", "created_at")
    search_fields = ("name", "identifier")


@admin.register(IREmitter)
class IREmitterAdmin(admin.ModelAdmin):
    list_display = ("name", "hub", "short_addr", "online", "last_seen")
    list_filter = ("hub", "online")
    search_fields = ("name",)


@admin.register(ACEvent)
class ACEventAdmin(admin.ModelAdmin):
    list_display = ("emitter", "ts", "power_on", "mode", "setpoint_c", "estimated_watts", "outdoor_temp_c")
    list_filter = ("emitter", "mode", "power_on")
    date_hierarchy = "ts"


@admin.register(Schedule)
class ScheduleAdmin(admin.ModelAdmin):
    list_display = ("emitter", "mode", "comfort_temp_c", "setback_temp_c", "sleep_hour", "wake_hour", "enabled")
    list_filter = ("mode", "enabled")


@admin.register(SmartPlug)
class SmartPlugAdmin(admin.ModelAdmin):
    list_display = ("name", "hub", "short_addr", "online", "power_on", "last_seen")
    list_filter = ("hub", "online", "power_on")
    search_fields = ("name",)


@admin.register(SmartPlugEvent)
class SmartPlugEventAdmin(admin.ModelAdmin):
    list_display = ("plug", "ts", "power_on", "measured_watts", "energy_wh", "voltage_dv", "current_ma")
    list_filter = ("plug", "power_on")
    date_hierarchy = "ts"


@admin.register(PendingCommand)
class PendingCommandAdmin(admin.ModelAdmin):
    list_display = ("command_type", "hub", "created_at", "delivered", "delivered_at")
    list_filter = ("command_type", "delivered", "hub")
