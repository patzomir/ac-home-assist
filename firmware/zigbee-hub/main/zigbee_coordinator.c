#include "zigbee_coordinator.h"
#include "nvs_store.h"

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "zb_coord";

static zb_coordinator_callbacks_t s_cb;
static ir_emitter_t s_emitters[MAX_EMITTERS];
static uint8_t s_emitter_count = 0;

/* ---- forward declarations ----------------------------------------------- */

static void configure_plug_reporting(uint16_t short_addr, uint8_t endpoint);

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
    /* Exact short-address match */
    ir_emitter_t *e = find_emitter_by_addr(short_addr);
    if (e) return e;

    /* IEEE address match — device rejoined with a new short address */
    for (int i = 0; i < s_emitter_count; i++) {
        if (memcmp(s_emitters[i].ieee_addr, ieee_addr,
                   sizeof(esp_zb_ieee_addr_t)) == 0) {
            ESP_LOGI(TAG, "Device rejoined: 0x%04x → 0x%04x (same IEEE)",
                     s_emitters[i].short_addr, short_addr);
            s_emitters[i].short_addr = short_addr;
            return &s_emitters[i];
        }
    }

    if (s_emitter_count >= MAX_EMITTERS) {
        ESP_LOGW(TAG, "Max emitters reached, ignoring %04x", short_addr);
        return NULL;
    }
    e = &s_emitters[s_emitter_count++];
    e->short_addr  = short_addr;
    memcpy(e->ieee_addr, ieee_addr, sizeof(esp_zb_ieee_addr_t));
    e->endpoint    = 0;
    e->online      = true;
    e->device_type = DEVICE_UNKNOWN;
    return e;
}

/* ---- ZDO discovery ------------------------------------------------------ */

/* Carries short addr + IEEE addr across the two async ZDO round-trips */
typedef struct {
    uint16_t           short_addr;
    esp_zb_ieee_addr_t ieee_addr;
} pending_join_t;

static void on_simple_desc_resp(esp_zb_zdp_status_t status,
                                 esp_zb_af_simple_desc_1_1_t *simple_desc,
                                 void *user_ctx)
{
    pending_join_t *pj = (pending_join_t *)user_ctx;
    device_type_t dtype = DEVICE_IR_EMITTER;  /* safe default */
    uint8_t ep = IR_EMITTER_ENDPOINT;

    if (status == ESP_ZB_ZDP_STATUS_SUCCESS && simple_desc) {
        ep = simple_desc->endpoint;

        /* Scan server (input) clusters: Thermostat → IR emitter */
        bool has_thermostat = false;
        for (int i = 0; i < simple_desc->app_input_cluster_count; i++) {
            if (simple_desc->app_cluster_list[i] ==
                    ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT) {
                has_thermostat = true;
                break;
            }
        }
        dtype = has_thermostat ? DEVICE_IR_EMITTER : DEVICE_SMART_PLUG;

        ESP_LOGI(TAG,
                 "Device 0x%04x ep=%d device_id=0x%04x → %s",
                 pj->short_addr, ep, simple_desc->app_device_id,
                 dtype == DEVICE_IR_EMITTER ? "IR emitter" : "smart plug");
    } else {
        ESP_LOGW(TAG,
                 "Simple desc failed for 0x%04x (status=%d), treating as IR emitter",
                 pj->short_addr, status);
    }

    ir_emitter_t *e = find_or_add_emitter(pj->short_addr, pj->ieee_addr);
    if (e) {
        e->device_type = dtype;
        e->endpoint    = ep;
        e->online      = true;

        if (dtype == DEVICE_SMART_PLUG) {
            configure_plug_reporting(pj->short_addr, ep);
            if (s_cb.on_plug_joined) s_cb.on_plug_joined(e);
        } else {
            if (s_cb.on_device_joined) s_cb.on_device_joined(e);
        }
        nvs_store_save_emitters(s_emitters, s_emitter_count);
    }

    free(pj);
}

static void on_active_ep_resp(esp_zb_zdp_status_t status,
                               uint8_t ep_count, uint8_t *ep_id_list,
                               void *user_ctx)
{
    pending_join_t *pj = (pending_join_t *)user_ctx;

    if (status != ESP_ZB_ZDP_STATUS_SUCCESS || ep_count == 0 || !ep_id_list) {
        ESP_LOGW(TAG,
                 "Active EP req failed for 0x%04x (status=%d), treating as IR emitter",
                 pj->short_addr, status);
        ir_emitter_t *e = find_or_add_emitter(pj->short_addr, pj->ieee_addr);
        if (e) {
            e->device_type = DEVICE_IR_EMITTER;
            e->endpoint    = IR_EMITTER_ENDPOINT;
            e->online      = true;
            if (s_cb.on_device_joined) s_cb.on_device_joined(e);
            nvs_store_save_emitters(s_emitters, s_emitter_count);
        }
        free(pj);
        return;
    }

    /* Query simple descriptor for the first reported endpoint */
    esp_zb_zdo_simple_desc_req_param_t req = {
        .addr_of_interest = pj->short_addr,
        .endpoint         = ep_id_list[0],
    };
    esp_zb_zdo_simple_desc_req(&req, on_simple_desc_resp, pj);
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
            ESP_LOGI(TAG, "Network steering done — opening permit join for 180 s");
            esp_zb_bdb_open_network(180);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *ann =
            (esp_zb_zdo_signal_device_annce_params_t *)
            esp_zb_app_signal_get_params(p_sg);
        ESP_LOGI(TAG, "Device announce: 0x%04x — querying descriptor",
                 ann->device_short_addr);

        pending_join_t *pj = malloc(sizeof(pending_join_t));
        if (!pj) {
            ESP_LOGE(TAG, "OOM handling device announce");
            break;
        }
        pj->short_addr = ann->device_short_addr;
        memcpy(pj->ieee_addr, ann->ieee_addr, sizeof(esp_zb_ieee_addr_t));

        esp_zb_zdo_active_ep_req_param_t req = {
            .addr_of_interest = ann->device_short_addr,
        };
        esp_zb_zdo_active_ep_req(&req, on_active_ep_resp, pj);
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
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

static void configure_plug_reporting(uint16_t short_addr, uint8_t endpoint)
{
    /* Tuya 0xe001 — power (0xd002, uint32, 0.1 W): report every 10-60 s or on ≥1 unit change */
    static uint32_t power_change = 1;
    esp_zb_zcl_config_report_record_t power_record = {
        .direction         = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID       = TUYA_ATTR_POWER_ID,
        .attrType          = ESP_ZB_ZCL_ATTR_TYPE_U32,
        .min_interval      = 10,
        .max_interval      = 60,
        .reportable_change = &power_change,
    };
    esp_zb_zcl_config_report_cmd_t power_cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint          = endpoint,
            .src_endpoint          = HUB_ENDPOINT,
        },
        .address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID     = TUYA_METERING_CLUSTER_ID,
        .record_number = 1,
        .record_field  = &power_record,
    };
    esp_zb_zcl_config_report_cmd_req(&power_cmd);

    /* Tuya 0xe001 — energy (0xd001, uint48, Wh): report every 30-300 s or on ≥1 Wh change */
    static uint64_t energy_change = 1;
    esp_zb_zcl_config_report_record_t energy_record = {
        .direction         = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID       = TUYA_ATTR_ENERGY_ID,
        .attrType          = ESP_ZB_ZCL_ATTR_TYPE_U48,
        .min_interval      = 30,
        .max_interval      = 300,
        .reportable_change = &energy_change,
    };
    esp_zb_zcl_config_report_cmd_t energy_cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint          = endpoint,
            .src_endpoint          = HUB_ENDPOINT,
        },
        .address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID     = TUYA_METERING_CLUSTER_ID,
        .record_number = 1,
        .record_field  = &energy_record,
    };
    esp_zb_zcl_config_report_cmd_req(&energy_cmd);

    ESP_LOGI(TAG, "Configured attribute reporting for plug 0x%04x ep=%d",
             short_addr, endpoint);
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

        if (msg->cluster != TUYA_METERING_CLUSTER_ID) break;

        plug_metering_t m = {
            .short_addr = addr,
            .unix_ts    = (uint32_t)time(NULL),
        };

        switch (msg->attribute.id) {
        case TUYA_ATTR_POWER_ID: {
            uint32_t raw = *(uint32_t *)msg->attribute.data.value;
            m.has_power     = true;
            m.active_power_w = (int16_t)(raw / 10); /* 0.1 W → W */
            ESP_LOGI(TAG, "Power from 0x%04x: %d W (raw %lu)",
                     addr, m.active_power_w, (unsigned long)raw);
            break;
        }
        case TUYA_ATTR_ENERGY_ID: {
            uint64_t raw = 0;
            memcpy(&raw, msg->attribute.data.value, 6); /* uint48 */
            m.has_energy = true;
            m.energy_wh  = raw;
            ESP_LOGI(TAG, "Energy from 0x%04x: %llu Wh",
                     addr, (unsigned long long)raw);
            break;
        }
        case TUYA_ATTR_VOLTAGE_ID: {
            uint32_t raw = *(uint32_t *)msg->attribute.data.value;
            m.has_voltage  = true;
            m.voltage_dv   = raw;
            ESP_LOGI(TAG, "Voltage from 0x%04x: %lu.%lu V",
                     addr, (unsigned long)(raw / 10), (unsigned long)(raw % 10));
            break;
        }
        case TUYA_ATTR_CURRENT_ID: {
            uint32_t raw = *(uint32_t *)msg->attribute.data.value;
            m.has_current = true;
            m.current_ma  = raw;
            ESP_LOGI(TAG, "Current from 0x%04x: %lu mA", addr, (unsigned long)raw);
            break;
        }
        default:
            break;
        }

        if (s_cb.on_plug_metering && (m.has_power || m.has_energy ||
                                       m.has_voltage || m.has_current)) {
            s_cb.on_plug_metering(&m);
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
    esp_log_level_set("ESP_ZIGBEE_CORE", ESP_LOG_VERBOSE);
    esp_log_level_set("ESP_ZIGBEE_API_ZDO", ESP_LOG_VERBOSE);
    esp_log_level_set("ESP_ZB_ZCL", ESP_LOG_VERBOSE);

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

    /* Thermostat cluster (CLIENT) — hub sends setpoints to IR emitters */
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

    /* Restore previously paired devices from NVS.
       If the blob is stale (e.g. struct grew), erase it so it doesn't linger. */
    s_emitter_count = MAX_EMITTERS;
    esp_err_t load_ret = nvs_store_load_emitters(s_emitters, &s_emitter_count);
    if (load_ret != ESP_OK) {
        if (load_ret != ESP_ERR_NOT_FOUND) {
            /* Blob exists but is unreadable (e.g. stale struct size) — erase it */
            ESP_LOGW(TAG, "Emitter blob stale (%s) — clearing", esp_err_to_name(load_ret));
            nvs_store_clear_emitters();
        }
        s_emitter_count = 0;
    }
    /* Mark restored devices as offline until they announce */
    for (int i = 0; i < s_emitter_count; i++) {
        s_emitters[i].online = false;
    }

    xTaskCreate(zb_task, "zb_task", 4096, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t zb_coordinator_permit_join(uint8_t duration_s)
{
    ESP_LOGI(TAG, "Opening join window for %d s", duration_s);
    return esp_zb_bdb_open_network(duration_s);
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

esp_err_t zb_coordinator_forget_all(void)
{
    s_emitter_count = 0;
    memset(s_emitters, 0, sizeof(s_emitters));
    esp_err_t ret = nvs_store_clear_emitters();
    ESP_LOGI(TAG, "All devices forgotten");
    return ret;
}
