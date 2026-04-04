from django.urls import path
from . import views

urlpatterns = [
    # Hub → Backend
    path("events/",                          views.receive_event,   name="receive-event"),
    path("hubs/<str:hub_id>/heartbeat/",     views.hub_heartbeat,   name="hub-heartbeat"),
    path("hubs/<str:hub_id>/commands/",      views.poll_commands,   name="poll-commands"),

    # Dashboard data
    path("dashboard/",                       views.dashboard_data,  name="dashboard-data"),

    # Schedules
    path("emitters/<int:emitter_id>/schedule/", views.schedules,    name="schedules"),

    # Emitter management
    path("emitters/<int:emitter_id>/",       views.update_emitter,  name="update-emitter"),
]
