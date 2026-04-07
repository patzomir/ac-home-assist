from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ("api", "0001_initial"),
    ]

    operations = [
        migrations.AddField(
            model_name="smartplugevent",
            name="energy_wh",
            field=models.BigIntegerField(blank=True, null=True),
        ),
        migrations.AddField(
            model_name="smartplugevent",
            name="voltage_dv",
            field=models.IntegerField(blank=True, null=True),
        ),
        migrations.AddField(
            model_name="smartplugevent",
            name="current_ma",
            field=models.IntegerField(blank=True, null=True),
        ),
    ]
