import os
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent.parent

SECRET_KEY = os.environ.get("SECRET_KEY", "dev-secret-change-in-production")
DEBUG = os.environ.get("DEBUG", "true").lower() == "true"
ALLOWED_HOSTS = os.environ.get("ALLOWED_HOSTS", "*").split(",")

INSTALLED_APPS = [
    "django.contrib.admin",
    "django.contrib.auth",
    "django.contrib.contenttypes",
    "django.contrib.sessions",
    "django.contrib.messages",
    "django.contrib.staticfiles",
    "rest_framework",
    "corsheaders",
    "drf_spectacular",
    "api.apps.ApiConfig",
]

MIDDLEWARE = [
    "corsheaders.middleware.CorsMiddleware",
    "django.middleware.common.CommonMiddleware",
    "django.contrib.sessions.middleware.SessionMiddleware",
    "django.middleware.csrf.CsrfViewMiddleware",
    "django.contrib.auth.middleware.AuthenticationMiddleware",
    "django.contrib.messages.middleware.MessageMiddleware",
]

ROOT_URLCONF = "config.urls"
WSGI_APPLICATION = "config.wsgi.application"

TEMPLATES = [
    {
        "BACKEND": "django.template.backends.django.DjangoTemplates",
        "DIRS": [],
        "APP_DIRS": True,
        "OPTIONS": {
            "context_processors": [
                "django.template.context_processors.request",
                "django.contrib.auth.context_processors.auth",
                "django.contrib.messages.context_processors.messages",
            ]
        },
    }
]

DATABASES = {
    "default": {
        "ENGINE": "django.db.backends.sqlite3",
        "NAME": BASE_DIR / "db.sqlite3",
    }
}

STATIC_URL = "/static/"
DEFAULT_AUTO_FIELD = "django.db.models.BigAutoField"
USE_TZ = True
TIME_ZONE = "Europe/Sofia"

REST_FRAMEWORK = {
    "DEFAULT_RENDERER_CLASSES": ["rest_framework.renderers.JSONRenderer"],
    "DEFAULT_SCHEMA_CLASS": "drf_spectacular.openapi.AutoSchema",
}

SPECTACULAR_SETTINGS = {
    "TITLE": "AC Home Assist API",
    "VERSION": "1.0.0",
    "SERVE_INCLUDE_SCHEMA": False,
}

CORS_ALLOW_ALL_ORIGINS = DEBUG

# --- MQTT settings ------------------------------------------------------------

MQTT_BROKER_HOST = os.environ.get("MQTT_BROKER_HOST", "localhost")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))

# --- Project settings ---------------------------------------------------------

# Sofia, Bulgaria (used for weather lookup when hub doesn't report location)
DEFAULT_LATITUDE  = float(os.environ.get("LATITUDE",  "42.6977"))
DEFAULT_LONGITUDE = float(os.environ.get("LONGITUDE", "23.3219"))

# BGN per kWh — Bulgarian residential peak tariff (Electrohold, 2024)
ELECTRICITY_RATE_BGN = float(os.environ.get("ELECTRICITY_RATE_BGN", "0.2529"))

# Time-of-use tariff (Bulgarian EVN/Electrohold two-zone schedule)
ELECTRICITY_RATE_DAY_BGN   = float(os.environ.get("ELECTRICITY_RATE_DAY_BGN",   "0.28"))
ELECTRICITY_RATE_NIGHT_BGN = float(os.environ.get("ELECTRICITY_RATE_NIGHT_BGN", "0.18"))
NIGHT_START_HOUR = int(os.environ.get("NIGHT_START_HOUR", "22"))  # 22:00 local
NIGHT_END_HOUR   = int(os.environ.get("NIGHT_END_HOUR",   "6"))   # 06:00 local
