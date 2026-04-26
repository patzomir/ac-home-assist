from django.db import migrations, models
import django.db.models.deletion
import django.utils.timezone


class Migration(migrations.Migration):

    dependencies = [
        ("api", "0004_hub_last_network_scan"),
    ]

    operations = [
        migrations.CreateModel(
            name="ShellyEMDevice",
            fields=[
                ("id", models.BigAutoField(auto_created=True, primary_key=True,
                                           serialize=False, verbose_name="ID")),
                ("device_id",  models.CharField(max_length=128, unique=True)),
                ("mac",        models.CharField(max_length=32, unique=True)),
                ("name",       models.CharField(default="Shelly EM Mini Gen4", max_length=128)),
                ("ip_address", models.GenericIPAddressField(blank=True, null=True,
                                                             protocol="IPv4")),
                ("online",     models.BooleanField(default=False)),
                ("last_seen",  models.DateTimeField(blank=True, null=True)),
                ("fw_version", models.CharField(blank=True, max_length=64)),
                ("created_at", models.DateTimeField(auto_now_add=True)),
            ],
        ),
        migrations.CreateModel(
            name="ShellyEMReading",
            fields=[
                ("id", models.BigAutoField(auto_created=True, primary_key=True,
                                           serialize=False, verbose_name="ID")),
                ("device", models.ForeignKey(
                    on_delete=django.db.models.deletion.CASCADE,
                    related_name="readings",
                    to="api.shellyemdevice",
                )),
                ("a_current",    models.FloatField(blank=True, null=True)),
                ("a_voltage",    models.FloatField(blank=True, null=True)),
                ("a_act_power",  models.FloatField(blank=True, null=True)),
                ("a_aprt_power", models.FloatField(blank=True, null=True)),
                ("a_pf",         models.FloatField(blank=True, null=True)),
                ("a_freq",       models.FloatField(blank=True, null=True)),
                ("a_energy_wh",  models.FloatField(blank=True, null=True)),
                ("b_current",    models.FloatField(blank=True, null=True)),
                ("b_voltage",    models.FloatField(blank=True, null=True)),
                ("b_act_power",  models.FloatField(blank=True, null=True)),
                ("b_aprt_power", models.FloatField(blank=True, null=True)),
                ("b_pf",         models.FloatField(blank=True, null=True)),
                ("b_freq",       models.FloatField(blank=True, null=True)),
                ("b_energy_wh",  models.FloatField(blank=True, null=True)),
                ("total_act_power",  models.FloatField(blank=True, null=True)),
                ("total_aprt_power", models.FloatField(blank=True, null=True)),
                ("ts", models.DateTimeField(db_index=True,
                                            default=django.utils.timezone.now)),
            ],
            options={
                "ordering": ["ts"],
            },
        ),
    ]
