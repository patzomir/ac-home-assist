from django.urls import path
from django.shortcuts import render


def dashboard(request):
    return render(request, "dashboard.html")


urlpatterns = [
    path("", dashboard, name="dashboard"),
]
