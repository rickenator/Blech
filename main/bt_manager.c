#include "bt_manager.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "sdkconfig.h"
#include "chat_service.h"

static const char *TAG = "ble";
static uint8_t own_addr_type;
static uint16_t response_handle;
static uint16_t subscribed_connection = BLE_HS_CONN_HANDLE_NONE;

static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t prompt_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t response_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

typedef struct {
    uint16_t connection;
    char prompt[CONFIG_CHAT_MAX_PROMPT_BYTES + 1];
} ble_job_t;

static void notify_text(uint16_t connection, const char *text)
{
    size_t length = strlen(text);
    for (size_t offset = 0; offset < length; offset += 180) {
        size_t chunk = length - offset;
        if (chunk > 180) {
            chunk = 180;
        }
        struct os_mbuf *packet = ble_hs_mbuf_from_flat(text + offset, chunk);
        if (!packet ||
            ble_gatts_notify_custom(connection, response_handle, packet) != 0) {
            ESP_LOGW(TAG, "BLE notification failed");
            break;
        }
    }
}

static void inference_task(void *arg)
{
    ble_job_t *job = arg;
    char *response = calloc(1, CONFIG_CHAT_MAX_RESPONSE_BYTES);
    if (response) {
        chat_service_generate(job->prompt, response,
                              CONFIG_CHAT_MAX_RESPONSE_BYTES);
        notify_text(job->connection, response);
        free(response);
    }
    free(job);
    vTaskDelete(NULL);
}

static int prompt_access(uint16_t connection, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length == 0 || length > CONFIG_CHAT_MAX_PROMPT_BYTES) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    ble_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    job->connection = connection;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, job->prompt, length, NULL);
    if (rc != 0) {
        free(job);
        return BLE_ATT_ERR_UNLIKELY;
    }
    job->prompt[length] = '\0';
    if (xTaskCreate(inference_task, "ble_chat", 8192, job, 4, NULL)
        != pdPASS) {
        free(job);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int response_access(uint16_t connection, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)connection;
    (void)attr_handle;
    (void)arg;
    const char *status = chat_service_model_status();
    return os_mbuf_append(ctxt->om, status, strlen(status)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &prompt_uuid.u,
                .access_cb = prompt_access,
                .flags = BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &response_uuid.u,
                .access_cb = response_access,
                .val_handle = &response_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_LINK_ESTAB:
        if (event->link_estab.status != 0) {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (subscribed_connection == event->disconnect.conn.conn_handle) {
            subscribed_connection = BLE_HS_CONN_HANDLE_NONE;
        }
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == response_handle &&
            event->subscribe.cur_notify) {
            subscribed_connection = event->subscribe.conn_handle;
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertisement data failed: %d", rc);
        return;
    }

    /*
     * A 128-bit UUID and the complete device name do not both fit in the
     * 31-byte legacy advertising payload. Keep the UUID in the advertisement
     * and publish the human-readable name in the scan response.
     */
    struct ble_hs_adv_fields response = {0};
    const char *name = ble_svc_gap_device_name();
    response.name = (uint8_t *)name;
    response.name_len = strlen(name);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response data failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as '%s'", name);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    assert(rc == 0);
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t bt_manager_start(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(CONFIG_CHAT_BLE_NAME);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_gatts_count_cfg(services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return ESP_FAIL;
    }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE service '%s' started", CONFIG_CHAT_BLE_NAME);
    return ESP_OK;
}
