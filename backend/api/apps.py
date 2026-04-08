import os
from django.apps import AppConfig


class ApiConfig(AppConfig):
    name = "api"
    default_auto_field = "django.db.models.BigAutoField"

    def ready(self):
        # Django's dev server forks a reloader process; only start the MQTT
        # client in the actual worker process to avoid duplicate connections.
        if os.environ.get("RUN_MAIN") != "true" and os.environ.get("DJANGO_SETTINGS_MODULE"):
            # Running under the reloader's parent process — skip.
            # (Under gunicorn / production there is no RUN_MAIN; the guard is
            # only relevant for `manage.py runserver`.)
            import sys
            if "runserver" in sys.argv:
                return

        from . import mqtt_manager
        mqtt_manager.start()
