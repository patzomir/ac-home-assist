#!/bin/bash
cd "$(dirname "$0")"
exec .venv/bin/python manage.py runserver 0.0.0.0:8765
