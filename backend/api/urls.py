from django.urls import path
from . import views

urlpatterns = [
    # Hub → Backend
    path("events/",                          views.receive_event,   name="receive-event"),
    path("hubs/<str:hub_id>/heartbeat/",     views.hub_heartbeat,   name="hub-heartbeat"),
    path("hubs/<str:hub_id>/commands/",      views.poll_commands,   name="poll-commands"),

    # Hub management
    path("hubs/pk/<int:hub_pk>/",            views.update_hub,         name="update-hub"),
    path("hubs/pk/<int:hub_pk>/scan/",       views.scan_hub_network,   name="scan-hub-network"),
    path("hubs/pk/<int:hub_pk>/reconnect/", views.reconnect_hub,      name="reconnect-hub"),

    # Dashboard data
    path("dashboard/",                       views.dashboard_data,  name="dashboard-data"),

    # Schedules
    path("emitters/<int:emitter_id>/schedule/", views.schedules,    name="schedules"),

    # Emitter management
    path("emitters/<int:emitter_id>/",       views.update_emitter,        name="update-emitter"),
    path("hubs/<str:hub_id>/emitters/addr/<str:addr>/", views.delete_emitter_by_addr, name="delete-emitter-by-addr"),

    # Smart plugs — hub → backend events
    path("plug-events/",                     views.receive_plug_event, name="receive-plug-event"),

    # Smart plug management
    path("plugs/<int:plug_id>/",             views.update_plug,     name="update-plug"),
    path("plugs/<int:plug_id>/control/",     views.control_plug,    name="control-plug"),

    # Shelly EM Mini Gen4
    path("shelly-em/",                           views.shelly_em_list,     name="shelly-em-list"),
    path("shelly-em/discover/",                  views.shelly_em_discover, name="shelly-em-discover"),
    path("shelly-em/<int:device_id>/",           views.shelly_em_detail,   name="shelly-em-detail"),
    path("shelly-em/<int:device_id>/poll/",      views.shelly_em_poll,     name="shelly-em-poll"),
    path("shelly-em/<int:device_id>/readings/",  views.shelly_em_readings, name="shelly-em-readings"),
]
