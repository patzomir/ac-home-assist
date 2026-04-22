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
static bool s_poll_active = false;

#define PLUG_POLL_INTERVAL_MS    30000
#define PLUG_MAX_MISSED_POLLS    3     /* mark offline after this many unanswered polls */

/* ---- forward declarations ----------------------------------------------- */

static void poll_plugs(uint8_t param);
static void bind_plug_cluster(uint16_t short_addr,
                               const esp_zb_ieee_addr_t plug_ieee,
                               uint8_t endpoint,
                               uint16_t cluster_id);

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
            /* Bind standard measurement clusters so the plug knows its data
               destination; plugs push attribute reports at their own interval
               (~5 min factory default for TS011F) which is sufficient for now.
               Polling is disabled but kept for future use with other hardware. */
            bind_plug_cluster(pj->short_addr, pj->ieee_addr, ep,
                              ELEC_MEAS_CLUSTER_ID);
            bind_plug_cluster(pj->short_addr, pj->ieee_addr, ep,
                              ZCL_METERING_CLUSTER_ID);
            if (s_cb.on_plug_joined) s_cb.on_plug_joined(e);
            /* Polling disabled — relying on device-pushed attribute reports.
            if (!s_poll_active) {
                s_poll_active = true;
                esp_zb_scheduler_alarm(poll_plugs, 0, PLUG_POLL_INTERVAL_MS);
            } */
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

            /* Smart plugs that survived a hub reboot stay associated and keep
               sending bound attribute reports at their own interval — no need
               to poll. Polling disabled; kept for future hardware compatibility. */
            /* if (!s_poll_active) {
                for (int i = 0; i < s_emitter_count; i++) {
                    if (s_emitters[i].device_type == DEVICE_SMART_PLUG) {
                        s_poll_active = true;
                        esp_zb_scheduler_alarm(poll_plugs, 0, 10000);
                        break;
                    }
                }
            } */
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

/* ---- ZDO binding -------------------------------------------------------- */

typedef struct {
    uint16_t short_addr;
    uint8_t  endpoint;
} bind_ctx_t;

static void on_bind_resp(esp_zb_zdp_status_t status, void *user_ctx)
{
    bind_ctx_t *ctx = (bind_ctx_t *)user_ctx;
    if (status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Bind OK for 0x%04x", ctx->short_addr);
    } else {
        ESP_LOGW(TAG, "Bind failed for 0x%04x (status=%d)", ctx->short_addr, status);
    }
    free(ctx);
}

static void bind_plug_cluster(uint16_t short_addr,
                               const esp_zb_ieee_addr_t plug_ieee,
                               uint8_t endpoint,
                               uint16_t cluster_id)
{
    bind_ctx_t *ctx = malloc(sizeof(bind_ctx_t));
    if (!ctx) {
        ESP_LOGE(TAG, "OOM in bind_plug_cluster");
        return;
    }
    ctx->short_addr = short_addr;
    ctx->endpoint   = endpoint;

    esp_zb_ieee_addr_t hub_ieee;
    esp_zb_get_long_address(hub_ieee);

    esp_zb_zdo_bind_req_param_t req = {
        .req_dst_addr  = short_addr,
        .src_endp      = endpoint,
        .cluster_id    = cluster_id,
        .dst_addr_mode = 0x03, /* 64-bit IEEE + endpoint */
        .dst_endp      = HUB_ENDPOINT,
    };
    memcpy(req.src_address, plug_ieee, sizeof(esp_zb_ieee_addr_t));
    memcpy(req.dst_address_u.addr_long, hub_ieee, sizeof(esp_zb_ieee_addr_t));

    ESP_LOGI(TAG, "Binding cluster 0x%04x on 0x%04x → hub", cluster_id, short_addr);
    esp_zb_zdo_device_bind_req(&req, on_bind_resp, ctx);
}

/* ---- Standard ZCL attribute decoders ------------------------------------ */

/* Decodes one haElectricalMeasurement (0x0B04) attribute.
 * Scale factors for Nous A7Z / TS011F:
 *   rmsVoltage  (0x0505): raw = 0.1 V  (acVoltageDivisor=10)
 *   rmsCurrent  (0x0508): raw = mA     (acCurrentDivisor=1000)
 *   activePower (0x050B): raw = 0.1 W  (acPowerDivisor=10)
 */
static bool decode_elec_meas_attr(uint16_t attr_id, const void *value,
                                   plug_metering_t *m)
{
    switch (attr_id) {
    case ELEC_MEAS_ATTR_POWER_ID: {
        int16_t raw = *(const int16_t *)value;
        m->has_power      = true;
        m->active_power_w = (int16_t)(raw / 10); /* 0.1 W → W */
        return true;
    }
    case ELEC_MEAS_ATTR_VOLTAGE_ID: {
        uint16_t raw = *(const uint16_t *)value;
        m->has_voltage = true;
        m->voltage_dv  = raw; /* raw already in 0.1 V */
        return true;
    }
    case ELEC_MEAS_ATTR_CURRENT_ID: {
        uint16_t raw = *(const uint16_t *)value;
        m->has_current = true;
        m->current_ma  = raw; /* raw already in mA */
        return true;
    }
    default:
        return false;
    }
}

/* Decodes one seMetering (0x0702) attribute.
 * currentSummDelivered (0x0000): uint48, divisor=100 → raw/100=kWh → raw*10=Wh
 */
static bool decode_metering_attr(uint16_t attr_id, const void *value,
                                  plug_metering_t *m)
{
    if (attr_id != ZCL_METERING_ATTR_ENERGY_ID) return false;

    /* uint48 stored little-endian in ZCL (6 bytes) */
    const uint8_t *b = (const uint8_t *)value;
    uint64_t raw = 0;
    for (int i = 5; i >= 0; i--) raw = (raw << 8) | b[i];

    m->has_energy = true;
    m->energy_wh  = raw * 10; /* raw/100=kWh → raw*10=Wh */
    return true;
}

/* ---- Plug polling (runs inside the Zigbee task via scheduler_alarm) ----- */

static uint16_t s_elec_poll_attrs[] = {
    ELEC_MEAS_ATTR_VOLTAGE_ID,
    ELEC_MEAS_ATTR_CURRENT_ID,
    ELEC_MEAS_ATTR_POWER_ID,
};
static uint16_t s_metering_poll_attrs[] = {
    ZCL_METERING_ATTR_ENERGY_ID,
};

static void poll_plugs(uint8_t param)
{
    (void)param;
    for (int i = 0; i < s_emitter_count; i++) {
        ir_emitter_t *e = &s_emitters[i];
        if (e->device_type != DEVICE_SMART_PLUG) continue;

        /* Fix 2: skip devices already known to be offline. */
        if (!e->online) continue;

        /* Fix 3: if the previous poll went unanswered, count it.
           After PLUG_MAX_MISSED_POLLS consecutive misses, mark offline
           so we stop queuing requests and burning ZBOSS buffers. */
        if (e->missed_polls >= PLUG_MAX_MISSED_POLLS) {
            ESP_LOGW(TAG, "0x%04x no response to %d consecutive polls — marking offline",
                     e->short_addr, e->missed_polls);
            e->online       = false;
            e->missed_polls = 0;
            if (s_cb.on_device_left) s_cb.on_device_left(e->short_addr);
            continue;
        }
        e->missed_polls++;  /* reset to 0 in CMD_READ_ATTR_RESP_CB_ID on response */

        /* Poll haElectricalMeasurement for voltage, current, power */
        esp_zb_zcl_read_attr_cmd_t elec_cmd = {
            .zcl_basic_cmd = {
                .dst_addr_u.addr_short = e->short_addr,
                .dst_endpoint          = e->endpoint,
                .src_endpoint          = HUB_ENDPOINT,
            },
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .clusterID    = ELEC_MEAS_CLUSTER_ID,
            .attr_number  = 3,
            .attr_field   = s_elec_poll_attrs,
        };
        esp_zb_zcl_read_attr_cmd_req(&elec_cmd);

        /* Poll seMetering for cumulative energy */
        esp_zb_zcl_read_attr_cmd_t meter_cmd = {
            .zcl_basic_cmd = {
                .dst_addr_u.addr_short = e->short_addr,
                .dst_endpoint          = e->endpoint,
                .src_endpoint          = HUB_ENDPOINT,
            },
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .clusterID    = ZCL_METERING_CLUSTER_ID,
            .attr_number  = 1,
            .attr_field   = s_metering_poll_attrs,
        };
        esp_zb_zcl_read_attr_cmd_req(&meter_cmd);

        ESP_LOGD(TAG, "Polling plug 0x%04x", e->short_addr);
    }
    esp_zb_scheduler_alarm(poll_plugs, 0, PLUG_POLL_INTERVAL_MS);
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
        plug_metering_t m = { .short_addr = addr, .unix_ts = (uint32_t)time(NULL) };
        bool decoded = false;

        if (msg->cluster == ELEC_MEAS_CLUSTER_ID) {
            decoded = decode_elec_meas_attr(msg->attribute.id,
                                            msg->attribute.data.value, &m);
        } else if (msg->cluster == ZCL_METERING_CLUSTER_ID) {
            decoded = decode_metering_attr(msg->attribute.id,
                                           msg->attribute.data.value, &m);
        } else {
            const uint8_t *b = (const uint8_t *)msg->attribute.data.value;
            ESP_LOGD(TAG, "REPORT 0x%04x cluster=0x%04x attr=0x%04x type=0x%02x "
                          "raw=[%02x %02x %02x %02x]",
                     addr, msg->cluster, msg->attribute.id, msg->attribute.data.type,
                     b[0], b[1], b[2], b[3]);
        }
        if (decoded && s_cb.on_plug_metering) s_cb.on_plug_metering(&m);
        break;
    }

    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
        const esp_zb_zcl_cmd_read_attr_resp_message_t *msg = message;
        uint16_t cluster = msg->info.cluster;
        if (cluster != ELEC_MEAS_CLUSTER_ID && cluster != ZCL_METERING_CLUSTER_ID) break;

        uint16_t addr = msg->info.src_address.u.short_addr;

        /* A poll response means the device is reachable — reset the miss counter
           and mark online so the scheduler can send it setpoints even if it never
           sent a device-announce. */
        ir_emitter_t *resp_e = find_emitter_by_addr(addr);
        if (resp_e) {
            resp_e->missed_polls = 0;
            if (!resp_e->online) {
                ESP_LOGI(TAG, "0x%04x responded to poll — marking online", addr);
                resp_e->online = true;
            }
        }

        plug_metering_t m = { .short_addr = addr, .unix_ts = (uint32_t)time(NULL) };
        bool any_decoded = false;
        for (const esp_zb_zcl_read_attr_resp_variable_t *v = msg->variables;
             v; v = v->next) {
            if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS) continue;
            bool decoded = false;
            if (cluster == ELEC_MEAS_CLUSTER_ID) {
                decoded = decode_elec_meas_attr(v->attribute.id,
                                                v->attribute.data.value, &m);
            } else {
                decoded = decode_metering_attr(v->attribute.id,
                                               v->attribute.data.value, &m);
            }
            if (decoded) any_decoded = true;
        }
        if (any_decoded && s_cb.on_plug_metering) s_cb.on_plug_metering(&m);
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
