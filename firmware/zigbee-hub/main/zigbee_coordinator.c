#include "zigbee_coordinator.h"
#include "nvs_store.h"

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include <time.h>

static const char *TAG = "zb_coord";

static zb_coordinator_callbacks_t s_cb;
static ir_emitter_t s_emitters[MAX_EMITTERS];
static uint8_t s_emitter_count = 0;

/* ---- forward declaration ------------------------------------------------ */

static void configure_plug_reporting(uint16_t short_addr);

/* ---- helpers ------------------------------------------------------------ */

static ir_emitter_t *find_emitter_by_addr(uint16_t short_addr)
{
    for (int i = 0; i < s_emitter_count; i++) {
        if (s_emitters[i].short_addr == short_addr) {
            return &s_emitters[i];
        }
    }
    return NULL;
}

static ir_emitter_t *find_or_add_emitter(uint16_t short_addr,
                                          const esp_zb_ieee_addr_t ieee_addr)
{
    ir_emitter_t *e = find_emitter_by_addr(short_addr);
    if (e) return e;
    if (s_emitter_count >= MAX_EMITTERS) {
        ESP_LOGW(TAG, "Max emitters reached, ignoring %04x", short_addr);
        return NULL;
    }
    e = &s_emitters[s_emitter_count++];
    e->short_addr = short_addr;
    memcpy(e->ieee_addr, ieee_addr, sizeof(esp_zb_ieee_addr_t));
    e->endpoint = IR_EMITTER_ENDPOINT;
    e->online   = true;
    return e;
}

/* ---- Zigbee signal handler (called from Zigbee task) -------------------- */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t              *p_sg  = signal_struct->p_app_signal;
    esp_err_t              err   = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = (esp_zb_app_signal_type_t)(*p_sg);

    switch (sig) {

    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialised, starting commissioning");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Zigbee start failed: %s", esp_err_to_name(err));
            break;
        }
        ESP_LOGI(TAG, "Forming Zigbee network");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Network formation failed: %s, retrying", esp_err_to_name(err));
            esp_zb_scheduler_alarm((esp_zb_callback_t)esp_zb_bdb_start_top_level_commissioning,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
            break;
        }
        {
            esp_zb_ieee_addr_t epid = {0};
            esp_zb_get_extended_pan_id(epid);
            uint16_t pan_id = esp_zb_get_pan_id();
            uint8_t  chan   = esp_zb_get_current_channel();
            ESP_LOGI(TAG, "Network formed: PAN 0x%04x, channel %d", pan_id, chan);

            if (s_cb.on_network_formed) {
                s_cb.on_network_formed(pan_id, chan);
            }
            /* Immediately open joining for 3 minutes on first boot */
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Network steering done, join window open");
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *ann =
            (esp_zb_zdo_signal_device_annce_params_t *)
            esp_zb_app_signal_get_params(p_sg);
        ESP_LOGI(TAG, "Device announce: short=0x%04x", ann->device_short_addr);

        ir_emitter_t *e = find_or_add_emitter(ann->device_short_addr,
                                               ann->ieee_addr);
        if (e && s_cb.on_device_joined) {
            s_cb.on_device_joined(e);
            nvs_store_save_emitters(s_emitters, s_emitter_count);
        }
        /* Configure metering attribute reports on every joined device.
           Non-metering devices (IR emitters) will simply reject the command. */
        configure_plug_reporting(ann->device_short_addr);
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        /* LEAVE_INDICATION carries the short address of the departing device */
        esp_zb_zdo_signal_leave_indication_params_t *lv =
            (esp_zb_zdo_signal_leave_indication_params_t *)
            esp_zb_app_signal_get_params(p_sg);
        ESP_LOGI(TAG, "Device left: 0x%04x", lv->short_addr);
        ir_emitter_t *e = find_emitter_by_addr(lv->short_addr);
        if (e) {
            e->online = false;
            if (s_cb.on_device_left) {
                s_cb.on_device_left(lv->short_addr);
            }
        }
        break;
    }

    default:
        ESP_LOGD(TAG, "Signal %d, err %s", sig, esp_err_to_name(err));
        break;
    }
}

/* ---- Attribute reporting configuration ---------------------------------- */

static void configure_plug_reporting(uint16_t short_addr)
{
    /* Electrical Measurement — ActivePower: report every 10-60 s or on ≥5 W change */
    static int16_t em_change = 5;
    esp_zb_zcl_config_report_record_t em_record = {
        .direction        = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID      = ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID,
        .attrType         = ESP_ZB_ZCL_ATTR_TYPE_S16,
        .min_interval     = 10,
        .max_interval     = 60,
        .reportable_change = &em_change,
    };
    esp_zb_zcl_config_report_cmd_t em_cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = A1Z_PLUG_ENDPOINT,
            .src_endpoint = HUB_ENDPOINT,
        },
        .address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID     = ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
        .record_number = 1,
        .record_field  = &em_record,
    };
    esp_zb_zcl_config_report_cmd_req(&em_cmd);

    /* Metering — CurrentSummationDelivered: report every 30-300 s or on ≥1 Wh change */
    static uint64_t mt_change = 1;
    esp_zb_zcl_config_report_record_t mt_record = {
        .direction        = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID      = ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        .attrType         = ESP_ZB_ZCL_ATTR_TYPE_U48,
        .min_interval     = 30,
        .max_interval     = 300,
        .reportable_change = &mt_change,
    };
    esp_zb_zcl_config_report_cmd_t mt_cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = A1Z_PLUG_ENDPOINT,
            .src_endpoint = HUB_ENDPOINT,
        },
        .address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID     = ESP_ZB_ZCL_CLUSTER_ID_METERING,
        .record_number = 1,
        .record_field  = &mt_record,
    };
    esp_zb_zcl_config_report_cmd_req(&mt_cmd);

    ESP_LOGI(TAG, "Configured attribute reporting for 0x%04x", short_addr);
}

/* ---- ZCL core action handler (command acks + attribute reports) --------- */

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message)
{
    switch (callback_id) {

    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID: {
        const esp_zb_zcl_cmd_default_resp_message_t *p = message;
        bool ok = (p->status_code == ESP_ZB_ZCL_STATUS_SUCCESS);
        uint16_t addr = p->info.src_address.u.short_addr;
        ESP_LOGD(TAG, "Cmd ack addr=0x%04x ok=%d", addr, ok);
        if (s_cb.on_cmd_ack) {
            s_cb.on_cmd_ack(addr, ok);
        }
        break;
    }

    case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
        const esp_zb_zcl_report_attr_message_t *msg = message;
        uint16_t addr = msg->src_address.u.short_addr;

        if (msg->cluster == ESP_ZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT &&
            msg->attribute.id == ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID) {
            int16_t watts = *(int16_t *)msg->attribute.data.value;
            ESP_LOGD(TAG, "ActivePower from 0x%04x: %d W", addr, watts);
            if (s_cb.on_plug_metering) {
                plug_metering_t m = {
                    .short_addr    = addr,
                    .has_power     = true,
                    .active_power_w = watts,
                    .has_summation = false,
                    .summation_wh  = 0,
                    .unix_ts       = (uint32_t)time(NULL),
                };
                s_cb.on_plug_metering(&m);
            }

        } else if (msg->cluster == ESP_ZB_ZCL_CLUSTER_ID_METERING &&
                   msg->attribute.id == ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID) {
            uint64_t wh = 0;
            memcpy(&wh, msg->attribute.data.value, 6); /* uint48 */
            ESP_LOGD(TAG, "Summation from 0x%04x: %llu Wh", addr, (unsigned long long)wh);
            if (s_cb.on_plug_metering) {
                plug_metering_t m = {
                    .short_addr    = addr,
                    .has_power     = false,
                    .active_power_w = 0,
                    .has_summation = true,
                    .summation_wh  = wh,
                    .unix_ts       = (uint32_t)time(NULL),
                };
                s_cb.on_plug_metering(&m);
            }
        }
        break;
    }

    default:
        break;
    }
    return ESP_OK;
}

/* ---- Zigbee task -------------------------------------------------------- */

static void zb_task(void *arg)
{
    esp_zb_cfg_t cfg = {
        .esp_zb_role          = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy  = false,
        .nwk_cfg.zczr_cfg.max_children = 10,
    };
    esp_zb_init(&cfg);

    /* --- Build coordinator endpoint descriptor --- */
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    /* Basic cluster (server) */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version   = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source  = 0x01, /* mains */
    };
    esp_zb_attribute_list_t *basic_attrs = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic_attrs,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
        (void *)"ac-home-assist");
    esp_zb_basic_cluster_add_attr(basic_attrs,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
        (void *)"hub-esp32c6");
    esp_zb_cluster_list_add_basic_cluster(cl, basic_attrs,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Identify cluster (server) — required by HA */
    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(cl,
        esp_zb_identify_cluster_create(&id_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Thermostat cluster (CLIENT) — hub sends setpoints */
    esp_zb_cluster_list_add_thermostat_cluster(cl,
        esp_zb_thermostat_cluster_create(NULL),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* On/Off cluster (CLIENT) — hub sends on/off */
    esp_zb_cluster_list_add_on_off_cluster(cl,
        esp_zb_on_off_cluster_create(NULL),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* Metering cluster (CLIENT) — hub reads energy summation from smart plugs */
    esp_zb_cluster_list_add_metering_cluster(cl,
        esp_zb_metering_cluster_create(NULL),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* Electrical Measurement cluster (CLIENT) — hub reads instantaneous power */
    esp_zb_cluster_list_add_electrical_meas_cluster(cl,
        esp_zb_electrical_meas_cluster_create(NULL),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* Time cluster (SERVER) — hub is time master for emitters */
    esp_zb_time_cluster_cfg_t time_cfg = { 0 };
    esp_zb_cluster_list_add_time_cluster(cl,
        esp_zb_time_cluster_create(&time_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Register endpoint */
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint        = HUB_ENDPOINT,
        .app_profile_id  = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id   = ESP_ZB_HA_HOME_GATEWAY_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cl, ep_cfg);
    esp_zb_device_register(ep_list);

    /* Use all channels; let the stack pick the clearest one */
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    /* Register core action handler — handles command acks and attribute reports */
    esp_zb_core_action_handler_register(zb_action_handler);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop(); /* never returns */
}

/* ---- Public API --------------------------------------------------------- */

esp_err_t zb_coordinator_init(const zb_coordinator_callbacks_t *callbacks)
{
    if (callbacks) {
        s_cb = *callbacks;
    }

    /* Restore previously paired emitters from NVS */
    s_emitter_count = MAX_EMITTERS;
    nvs_store_load_emitters(s_emitters, &s_emitter_count);
    /* Mark restored emitters as offline until they announce */
    for (int i = 0; i < s_emitter_count; i++) {
        s_emitters[i].online = false;
    }

    xTaskCreate(zb_task, "zb_task", 4096, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t zb_coordinator_permit_join(uint8_t duration_s)
{
    ESP_LOGI(TAG, "Opening join window for %d s", duration_s);
    esp_zb_zdo_permit_joining_req_param_t req = {
        .dst_nwk_addr    = 0xFFFC, /* all routers + coordinator */
        .permit_duration = duration_s,
        .tc_significance = 1,
    };
    esp_zb_zdo_permit_joining_req(&req, NULL, NULL);
    return ESP_OK;
}

esp_err_t zb_coordinator_send_setpoint(uint16_t short_addr,
                                        int8_t setpoint_c,
                                        ac_mode_t mode)
{
    ir_emitter_t *e = find_emitter_by_addr(short_addr);
    if (!e) {
        ESP_LOGW(TAG, "send_setpoint: unknown addr 0x%04x", short_addr);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Sending setpoint %d°C mode=%d to 0x%04x",
             setpoint_c, mode, short_addr);

    /* Write occupied-heating-setpoint attribute on the emitter */
    int16_t sp_zb = ZB_TEMP(setpoint_c);
    esp_zb_zcl_write_attr_cmd_t write_cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = e->endpoint,
            .src_endpoint = HUB_ENDPOINT,
        },
        .address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID     = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
        .attr_number   = 2,
        .attr_field    = (esp_zb_zcl_attribute_t[]){
            {
                .id   = ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID,
                .data = { .type = ESP_ZB_ZCL_ATTR_TYPE_S16, .value = &sp_zb },
            },
            {
                .id   = ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID,
                .data = { .type = ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, .value = &mode },
            },
        },
    };
    esp_zb_zcl_write_attr_cmd_req(&write_cmd);
    return ESP_OK;
}

esp_err_t zb_coordinator_send_power(uint16_t short_addr, bool on)
{
    ir_emitter_t *e = find_emitter_by_addr(short_addr);
    if (!e) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Sending power %s to 0x%04x", on ? "ON" : "OFF", short_addr);

    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = e->endpoint,
            .src_endpoint = HUB_ENDPOINT,
        },
        .address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
                           : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID,
    };
    esp_zb_zcl_on_off_cmd_req(&cmd);
    return ESP_OK;
}

const ir_emitter_t *zb_coordinator_get_emitters(uint8_t *count_out)
{
    if (count_out) *count_out = s_emitter_count;
    return s_emitters;
}
