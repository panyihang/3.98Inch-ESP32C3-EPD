/*
 * 3.98" four-colour e-paper bring-up / serial diagnostics on ESP32-C3.
 *
 * Power-on only initializes the panel and starts the serial console. No test
 * pattern or refresh is issued automatically. The console lets you trigger
 * any diagnostic step without reflashing:
 *
 *   1..4  solid white / black / yellow / red
 *   5     info page (colour bars, grid, orientation markers)
 *   6     horizontal colour bands
 *   7     8 px checkerboard
 *   8     stripe ruler, 24 px stripes with row labels
 *   9     vertical stripe ruler
 *   0     alternate info page
 *   A     band probe A: 48 px blocks inside rows 96..336
 *   B     band probe B: blank white page
 *   x     cycle data framing: chunk -> line -> byte -> continuous CS
 *   w     toggle the 0x83 full-panel window before each frame
 *   d     framebuffer + stage diagnostics
 *   h     framebuffer checksum and row samples
 *   rotation <0|90|180|270>  persist software UI rotation and redraw
 *   s     deep sleep the panel
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "epd_panel.h"
#include "test_patterns.h"
#include "app_state.h"
#include "refresh_policy.h"
#include "refresh_queue.h"
#include "nvs_store.h"
#include "photo_store.h"
#include "font_store.h"
#include "ntp_manager.h"
#include "ui_app.h"
#include "wifi_manager.h"
#include "weather_client.h"
#include "ble_commands.h"
#include "ble_service.h"

static const char *TAG = "main";
static app_state_t s_app_state;
static wifi_manager_t s_wifi_manager;
static ntp_manager_t s_ntp_manager;
static ble_commands_t s_ble_commands;
static weather_client_t s_weather_client;
static refresh_queue_t *s_refresh_queue;
static QueueHandle_t s_ble_frame_queue;
static QueueHandle_t s_weather_request_queue;
static bool s_ntp_refresh_sent;
static bool s_wifi_started;
static TaskHandle_t s_weather_task_handle;
static SemaphoreHandle_t s_display_mutex;
static SemaphoreHandle_t s_config_mutex;
static bool s_usb_console_input;

typedef struct {
    uint8_t type;
    uint16_t sequence;
    uint16_t payload_len;
    uint8_t payload[BLE_PROTO_MAX_PAYLOAD];
} ble_queued_frame_t;

/* BLE writes can carry a full photo chunk.  Keep the command task's working
 * buffers out of its call stack: the command path also uses NVS and upload
 * helpers with non-trivial call frames, so putting these objects on a 3 KB
 * task stack leaves too little guard margin.  This workspace is owned by the
 * single ble_commands task and is never accessed from the GATT callback. */
typedef struct {
    ble_queued_frame_t queued;
    ble_command_reply_t reply;
    uint8_t response_payload[BLE_COMMAND_RESPONSE_DATA_MAX + 1u];
    uint8_t encoded[BLE_COMMAND_ENCODED_RESPONSE_MAX];
} ble_command_workspace_t;

static ble_command_workspace_t s_ble_command_workspace;
static ble_queued_frame_t s_ble_rx_workspace;

typedef struct {
    char body[1024];
    size_t length;
} weather_http_body_t;

static esp_err_t network_init(void);
static void refresh_task(void *arg);
static void ntp_monitor_task(void *arg);
static bool submit_refresh(uint32_t reason, bool force);
static void ble_config_changed(void *ctx);
static void ble_rx_enqueue_frame(const ble_frame_view_t *frame, void *ctx);
static void ble_command_task(void *arg);
static void weather_task(void *arg);
static bool start_weather_task(void);
static int weather_request_enqueue(void *ctx);
static int ble_runtime_info(void *ctx, ble_runtime_info_t *info);
static int ble_request_refresh(void *ctx, uint32_t reason, bool force);
static int ble_apply_wifi_credentials(void *ctx);
static esp_err_t weather_http_event(esp_http_client_event_t *event);

static bool app_config_lock(void *ctx)
{
    SemaphoreHandle_t mutex = (SemaphoreHandle_t)ctx;
    return mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

static void app_config_unlock(void *ctx)
{
    SemaphoreHandle_t mutex = (SemaphoreHandle_t)ctx;
    if (mutex) xSemaphoreGive(mutex);
}

static bool app_has_wifi_credentials(void)
{
    bool has_credentials = false;
    if (app_config_lock(s_config_mutex)) {
        has_credentials = wifi_manager_has_credentials(&s_app_state.config);
        app_config_unlock(s_config_mutex);
    }
    return has_credentials;
}

static bool handle_console_command(const char *command, size_t length)
{
    (void)length;
    if (strncmp(command, "rotation ", 9) != 0) {
        return false;
    }
    const long degrees = strtol(command + 9, NULL, 10);
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
        ESP_LOGW(TAG, "rotation must be 0, 90, 180 or 270 degrees");
        return true;
    }
    if (!app_config_lock(s_config_mutex)) {
        ESP_LOGW(TAG, "configuration lock unavailable");
        return true;
    }
    s_app_state.config.rotation = (uint8_t)(degrees / 90);
    const esp_err_t save_err = nvs_store_save(&s_app_state.config);
    app_config_unlock(s_config_mutex);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "could not persist rotation=%ld", degrees);
    }
    ESP_LOGI(TAG, "manual UI rotation set to %ld degrees; redraw queued", degrees);
    submit_refresh(REFRESH_REASON_CONFIG_CHANGE, true);
    return true;
}

#define CONSOLE_UART    UART_NUM_0
#define CONSOLE_BAUD    115200

static void banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " 3.98in 4-colour e-paper bring-up (JD79665)");
    ESP_LOGI(TAG, " chip    : ESP32-C3 rev %d, %d core(s)", chip.revision, chip.cores);
    ESP_LOGI(TAG, " flash   : %" PRIu32 " MB", flash_size / (1024 * 1024));
    ESP_LOGI(TAG, " panel   : controller %dx%d, image %dx%d, 4 colours, 2bpp",
             EPD_PHYS_WIDTH, EPD_PHYS_HEIGHT, EPD_WIDTH, EPD_HEIGHT);
    ESP_LOGI(TAG, " pins    : DIN=IO%d SCK=IO%d CS=IO%d DC=IO%d RST=IO%d BUSY=IO%d",
             PIN_EPD_DIN, PIN_EPD_SCK, PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST,
             PIN_EPD_BUSY);
    ESP_LOGI(TAG, " ref     : github.com/krstc/openepaperlinkforCN "
                  "(EPD_3in98g / JD79665)");
    ESP_LOGI(TAG, "==================================================");
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[64];
    size_t len = 0;
    uint8_t buf[32];

    while (1) {
        int n = s_usb_console_input
                    ? usb_serial_jtag_read_bytes(buf, sizeof(buf),
                                                 pdMS_TO_TICKS(100))
                    : uart_read_bytes(CONSOLE_UART, buf, sizeof(buf),
                                      pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (len == 0) {
                    continue;
                }
                line[len] = '\0';
                if (s_display_mutex) {
                    xSemaphoreTake(s_display_mutex, portMAX_DELAY);
                }
                if (!handle_console_command(line, len) &&
                    !test_console_command(line, len)) {
                    ESP_LOGW(TAG, "unknown command '%s'", line);
                }
                if (s_display_mutex) {
                    xSemaphoreGive(s_display_mutex);
                }
                len = 0;
                continue;
            }
            /* Keep spaces inside a command so arguments such as
             * "rotation 90" and "clock 1000000" reach their parser. */
            if (c == ' ' && len == 0) {
                continue;
            }
            if (len + 1 < sizeof(line)) {
                line[len++] = c;
            } else {
                len = 0; /* overlong token: drop it */
            }
        }
    }
}

void app_main(void)
{
    /* ESP-IDF v6 no longer installs a UART driver for the console, but
     * uart_read_bytes() needs one. Without this the console task spins on
     * "uart driver error" and floods the log. */
    const uart_config_t console_cfg = {
        .baud_rate = CONSOLE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONSOLE_UART, &console_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CONSOLE_UART, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    /* The board is normally connected through the ESP32-C3 USB
     * Serial/JTAG bridge.  UART0 remains a fallback for an external USB-UART,
     * but UART0 does not receive bytes sent to COM19 on this board. */
    usb_serial_jtag_driver_config_t usb_cfg =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_is_driver_installed() ||
        usb_serial_jtag_driver_install(&usb_cfg) == ESP_OK) {
        s_usb_console_input = true;
        ESP_LOGI(TAG, "serial diagnostics input: USB Serial/JTAG");
    } else {
        ESP_LOGI(TAG, "serial diagnostics input: UART0");
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = photo_store_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "photo store unavailable: %s", esp_err_to_name(err));
    }

    /* Flash font is optional: a missing/corrupt library only degrades missing
     * glyphs to blanks and never blocks boot. */
    font_store_init();

    /* Load persistent configuration before starting the diagnostic console. */
    app_state_init(&s_app_state, 0);
    (void)nvs_store_load(&s_app_state.config);
    weather_client_init(&s_weather_client,
                        s_app_state.config.latitude_e7,
                        s_app_state.config.longitude_e7);
    if (s_app_state.config.weather_cache_unix != 0) {
        s_weather_client.last.updated_unix =
            s_app_state.config.weather_cache_unix;
        s_weather_client.last.stale = true;
    }
    s_config_mutex = xSemaphoreCreateMutex();
    ble_commands_init(&s_ble_commands, &s_app_state.config, nvs_store_save,
                      app_config_lock, app_config_unlock,
                      ble_config_changed, s_config_mutex);
    const ble_commands_ops_t ble_ops = {
        .get_runtime_info = ble_runtime_info,
        .request_refresh = ble_request_refresh,
        .apply_wifi_credentials = ble_apply_wifi_credentials,
        .request_weather_refresh = weather_request_enqueue,
    };
    ble_commands_set_ops(&s_ble_commands, &ble_ops);
    (void)app_state_set_refresh_interval(&s_app_state,
                                         s_app_state.config.auto_refresh_interval_sec);
    ESP_LOGI(TAG, "app defaults: mode=%u rotation=%u auto_refresh=%" PRIu32
             " sec, Wi-Fi max TX requested=%d qdBm effective=%d qdBm",
             s_app_state.config.home_mode, s_app_state.config.rotation,
             s_app_state.config.auto_refresh_interval_sec,
             WIFI_MANAGER_MAX_TX_POWER_QDBM,
             wifi_manager_effective_tx_power_qdbm());
    ntp_manager_init(&s_ntp_manager, s_app_state.config.timezone);

    s_refresh_queue = refresh_queue_create();
    if (!s_refresh_queue) {
        ESP_LOGE(TAG, "refresh queue allocation failed");
        xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
        return;
    }
    s_ble_frame_queue = xQueueCreate(4, sizeof(ble_queued_frame_t));
    s_weather_request_queue = xQueueCreate(1, sizeof(uint8_t));
    if (!s_ble_frame_queue || !s_weather_request_queue) {
        ESP_LOGE(TAG, "BLE/weather work queue allocation failed");
        xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
        return;
    }
    s_display_mutex = xSemaphoreCreateMutex();
    if (!s_display_mutex) {
        ESP_LOGE(TAG, "display mutex allocation failed");
        xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
        return;
    }
    /* Bring up BLE before allocating the large EPD framebuffer.  NimBLE and
     * the controller require internal DRAM; allocating the 106 KB panel
     * buffer first can make VHCI buffer creation fail on the C3.  The BLE
     * service is self-contained and has already got its RX queue above, so
     * starting it synchronously here also gives us a deterministic failure
     * point before any panel task is launched. */
    const int ble_result = ble_service_start(ble_rx_enqueue_frame, NULL);
    if (ble_result != 0) {
        ESP_LOGE(TAG, "BLE service start failed: %d (BLE disabled)", ble_result);
    }

    banner();

    err = epd_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI/GPIO bring-up failed: %s", esp_err_to_name(err));
        test_report();
        return;
    }

    err = epd_fb_alloc();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "framebuffer allocation failed: %s", esp_err_to_name(err));
        test_report();
        return;
    }

    /* Wi-Fi owns substantially more internal RAM than the idle network
     * manager. Start it after the EPD chunks have been reserved to keep the
     * display framebuffer available while BLE provisioning remains active. */
    err = network_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network initialization failed: %s", esp_err_to_name(err));
    }

    err = epd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        test_report();
        ESP_LOGE(TAG, "the SPI bus is up and the framebuffer exists, so this is "
                      "a panel/PCB side problem, not a software one");
        xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
        return;
    }

    /* Keep the low-level diagnostic controls available, but do not run any
     * panel self-test automatically at boot. */
    epd_set_frame_mode(EPD_FRAME_SWEEP);
    epd_set_rowmap(EPD_MAP_LINEAR);
    epd_set_column_major(false);
    epd_set_init_variant(false);
    epd_set_x_offset(0);
    epd_set_partial_window_mode(true);
    epd_set_data_stop(true);
    epd_set_xfer_mode(EPD_XFER_CHUNK);
    epd_set_physical_frame(true);

    ui_app_init();

    ESP_LOGI(TAG, "heap before background tasks: %u bytes",
             (unsigned)esp_get_free_heap_size());
    /* Panel transfers contain long CPU-bound stages. Run at idle priority so
     * the CPU0 idle task still gets watchdog-reset time slices throughout a
     * refresh. All initial and subsequent frames use this one serialized path. */
    const BaseType_t refresh_created = xTaskCreate(refresh_task, "epd_refresh", 3072, NULL, 0, NULL);
    const BaseType_t ntp_created = xTaskCreate(ntp_monitor_task, "ntp_monitor", 2048, NULL, 4, NULL);
    /* Large protocol buffers live in s_ble_command_workspace.  The remaining
     * command path is small enough for 2 KiB words (8 KiB on ESP32-C3), which
     * keeps this task within the tight heap budget alongside the EPD worker. */
    const BaseType_t ble_created = xTaskCreate(ble_command_task, "ble_commands", 2048, NULL, 4, NULL);
    if (refresh_created != pdPASS || ntp_created != pdPASS || ble_created != pdPASS) {
        ESP_LOGE(TAG, "background task creation failed (refresh=%d ntp=%d ble=%d, heap=%u)",
                 (int)refresh_created, (int)ntp_created, (int)ble_created,
                 (unsigned)esp_get_free_heap_size());
        ESP_LOGE(TAG, "background task creation failed");
        xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
        return;
    }
    submit_refresh(REFRESH_REASON_BOOT, true);
    xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
}

static bool submit_refresh(uint32_t reason, bool force)
{
    if (!s_refresh_queue) return false;
    const refresh_queue_item_t item = {
        .reason_mask = reason,
        .force = force,
        .requested_ms = (uint64_t)(esp_timer_get_time() / 1000),
    };
    if (!refresh_queue_submit(s_refresh_queue, &item)) {
        ESP_LOGW(TAG, "refresh request queue full (reason=0x%" PRIx32 ")", reason);
        return false;
    }
    return true;
}

static void ble_config_changed(void *ctx)
{
    (void)ctx;
    nvs_store_config_t config;
    if (app_config_lock(s_config_mutex)) {
        config = s_app_state.config;
        app_config_unlock(s_config_mutex);
        (void)ntp_manager_set_timezone(&s_ntp_manager, config.timezone);
        (void)app_state_set_refresh_interval(&s_app_state,
                                             config.auto_refresh_interval_sec);
        weather_client_set_location(&s_weather_client, config.latitude_e7,
                                     config.longitude_e7);
    }
    submit_refresh(REFRESH_REASON_CONFIG_CHANGE, true);
}

static void ble_rx_enqueue_frame(const ble_frame_view_t *frame, void *ctx)
{
    (void)ctx;
    if (!frame || !s_ble_frame_queue ||
        frame->payload_len > BLE_PROTO_MAX_PAYLOAD) {
        return;
    }
    /* NimBLE invokes this callback from its host task.  Keep the 1 KiB
     * payload staging object out of that task's stack as well; xQueueSend
     * copies it before this callback returns. */
    ble_queued_frame_t *queued = &s_ble_rx_workspace;
    *queued = (ble_queued_frame_t){
        .type = frame->type,
        .sequence = frame->sequence,
        .payload_len = frame->payload_len,
    };
    if (frame->payload_len) {
        memcpy(queued->payload, frame->payload, frame->payload_len);
    }
    if (xQueueSend(s_ble_frame_queue, queued, 0) != pdTRUE) {
        ESP_LOGW(TAG, "BLE command queue full; dropped type=0x%02x seq=%u",
                 frame->type, frame->sequence);
    }
}

static void ble_command_task(void *arg)
{
    (void)arg;
    ble_command_workspace_t *workspace = &s_ble_command_workspace;
    for (;;) {
        if (xQueueReceive(s_ble_frame_queue, &workspace->queued, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const ble_frame_view_t request = {
            .version = BLE_PROTO_VERSION,
            .type = workspace->queued.type,
            .sequence = workspace->queued.sequence,
            .payload_len = workspace->queued.payload_len,
            .payload = workspace->queued.payload,
        };
        memset(&workspace->reply, 0, sizeof(workspace->reply));
        const ble_command_result_t command_status = ble_commands_process_ex(
            &s_ble_commands, &request, &workspace->reply);

        workspace->response_payload[0] = (uint8_t)command_status;
        if (workspace->reply.length) {
            memcpy(workspace->response_payload + 1, workspace->reply.data,
                   workspace->reply.length);
        }
        const ble_frame_view_t response = {
            .version = BLE_PROTO_VERSION,
            .type = (uint8_t)(0x80u | workspace->queued.type),
            .sequence = workspace->queued.sequence,
            .payload_len = (uint16_t)(workspace->reply.length + 1u),
            .payload = workspace->response_payload,
        };
        size_t encoded_len = 0;
        if (ble_proto_encode(&response, workspace->encoded,
                             sizeof(workspace->encoded), &encoded_len) ==
            BLE_PROTO_OK) {
            const int notify_err = ble_service_notify(workspace->encoded, encoded_len);
            if (notify_err != 0) {
                ESP_LOGD(TAG, "BLE response not delivered (type=0x%02x seq=%u err=%d)",
                         workspace->queued.type, workspace->queued.sequence, notify_err);
            }
        }
        const UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
        if (stack_free < 256u) {
            ESP_LOGW(TAG, "BLE command task stack low: %u words free",
                     (unsigned)stack_free);
        }
    }
}

static int ble_runtime_info(void *ctx, ble_runtime_info_t *info)
{
    (void)ctx;
    if (!info) return -1;
    if (!app_config_lock(s_config_mutex)) return -1;
    const bool wifi_credentials_present =
        wifi_manager_has_credentials(&s_app_state.config);
    app_config_unlock(s_config_mutex);
    *info = (ble_runtime_info_t){
        .wifi_connected = s_app_state.wifi_connected,
        .wifi_credentials_present = wifi_credentials_present,
        .ntp_synced = ntp_manager_is_synced(&s_ntp_manager),
        .weather_valid = s_weather_client.last.valid,
        .weather_stale = s_weather_client.last.stale,
        .wifi_tx_power_qdbm = wifi_manager_effective_tx_power_qdbm(),
        .ble_tx_power_dbm = 6,
        .weather_temperature_c10 = s_weather_client.last.temperature_c10,
        .weather_updated_unix = s_weather_client.last.updated_unix,
    };
    return 0;
}

static int ble_request_refresh(void *ctx, uint32_t reason, bool force)
{
    (void)ctx;
    return submit_refresh(reason, force) ? 0 : -1;
}

static int ble_apply_wifi_credentials(void *ctx)
{
    (void)ctx;
    if (!s_wifi_started) return -1;

    nvs_store_config_t config;
    if (!app_config_lock(s_config_mutex)) return -1;
    config = s_app_state.config;
    app_config_unlock(s_config_mutex);
    if (!wifi_manager_has_credentials(&config)) {
        (void)esp_wifi_disconnect();
        s_app_state.wifi_connected = false;
        s_wifi_manager.credentials_present = false;
        s_wifi_manager.retries = 0;
        s_wifi_manager.state = WIFI_MANAGER_DISCONNECTED;
        ESP_LOGI(TAG, "saved Wi-Fi credentials cleared via BLE");
        return 0;
    }

    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, config.wifi_ssid,
           strnlen(config.wifi_ssid, sizeof(sta.sta.ssid)));
    memcpy(sta.sta.password, config.wifi_password,
           strnlen(config.wifi_password, sizeof(sta.sta.password)));
    (void)esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BLE Wi-Fi credentials saved but STA config failed: %s",
                 esp_err_to_name(err));
        return (int)err;
    }
    (void)wifi_manager_apply_tx_limit();
    s_app_state.wifi_connected = false;
    s_wifi_manager.credentials_present = true;
    s_wifi_manager.retries = 0;
    s_wifi_manager.state = WIFI_MANAGER_CONNECTING;
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "BLE Wi-Fi credentials saved; connect request failed: %s",
                 esp_err_to_name(err));
        return (int)err;
    }
    ESP_LOGI(TAG, "Wi-Fi credentials received over BLE and saved; STA reconnect requested");
    return 0;
}

static int weather_request_enqueue(void *ctx)
{
    (void)ctx;
    if (!s_weather_request_queue) return -1;
    const uint8_t request = 1;
    return xQueueSend(s_weather_request_queue, &request, 0) == pdTRUE ? 0 : -1;
}

static void refresh_task(void *arg)
{
    (void)arg;
    refresh_queue_item_t item;
    while (true) {
        if (!refresh_queue_receive(s_refresh_queue, &item, 1000)) continue;
        refresh_queue_item_t extra;
        while (refresh_queue_receive(s_refresh_queue, &extra, 0)) {
            item.reason_mask |= extra.reason_mask;
            item.force = item.force || extra.force;
        }
        nvs_store_config_t ui_config;
        if (app_config_lock(s_config_mutex)) {
            ui_config = s_app_state.config;
            app_config_unlock(s_config_mutex);
        } else {
            nvs_store_config_defaults(&ui_config);
        }
        ui_app_state_t ui_state = {
            .mode = (ui_mode_t)ui_config.home_mode,
            .photo_slot = ui_config.current_photo_slot,
            .rotation = ui_config.rotation,
            .wifi_connected = s_app_state.wifi_connected,
            .ntp_synced = ntp_manager_is_synced(&s_ntp_manager),
            .weather_stale = s_app_state.weather_stale,
            .temperature_c10 = s_app_state.weather_temperature_c10,
        };
        if (s_weather_client.last.valid) {
            strlcpy(ui_state.weather_summary, s_weather_client.last.summary,
                    sizeof(ui_state.weather_summary));
        }
        strlcpy(ui_state.timezone, ui_config.timezone,
                sizeof(ui_state.timezone));
        strlcpy(ui_state.today_plan, ui_config.today_plan,
                sizeof(ui_state.today_plan));
        xSemaphoreTake(s_display_mutex, portMAX_DELAY);
        ui_app_set_state(&ui_state);
        int result = ui_app_render();   /* 内部已包含条带流式 epd_display */
        xSemaphoreGive(s_display_mutex);
        if (result != 0) ESP_LOGE(TAG, "queued EPD refresh failed: %d", result);
    }
}

static void network_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            if (app_has_wifi_credentials()) {
                (void)esp_wifi_connect();
            } else {
                s_wifi_manager.state = WIFI_MANAGER_DISCONNECTED;
                ESP_LOGI(TAG, "Wi-Fi waiting for credentials over BLE");
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_app_state.wifi_connected = false;
            if (s_wifi_manager.retries < UINT8_MAX) s_wifi_manager.retries++;
            if (s_wifi_manager.retries >= 3) {
                s_wifi_manager.state = WIFI_MANAGER_DISCONNECTED;
                ESP_LOGW(TAG, "Wi-Fi retry limit reached; waiting for new BLE credentials");
            } else if (app_has_wifi_credentials()) {
                ESP_LOGW(TAG, "Wi-Fi disconnected; reconnect attempt %u",
                         s_wifi_manager.retries);
                (void)esp_wifi_connect();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_app_state.wifi_connected = true;
        s_wifi_manager.state = WIFI_MANAGER_CONNECTED;
        s_wifi_manager.retries = 0;
        ESP_LOGI(TAG, "Wi-Fi connected; starting SNTP");
        if (!start_weather_task()) {
            ESP_LOGW(TAG, "weather task deferred: insufficient heap");
        }
        if (!s_ntp_manager.started && ntp_manager_start(&s_ntp_manager) != 0) {
            ESP_LOGE(TAG, "SNTP initialization failed");
        }
    }
}

static void ntp_monitor_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!s_ntp_refresh_sent && ntp_manager_is_synced(&s_ntp_manager)) {
            s_ntp_refresh_sent = true;
            s_app_state.ntp_synced = true;
            ESP_LOGI(TAG, "first NTP sync complete");
            submit_refresh(REFRESH_REASON_NTP_SYNC, true);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t weather_http_event(esp_http_client_event_t *event)
{
    if (!event || event->event_id != HTTP_EVENT_ON_DATA || !event->data ||
        event->data_len <= 0 || !event->user_data) {
        return ESP_OK;
    }
    weather_http_body_t *body = event->user_data;
    const size_t length = (size_t)event->data_len;
    if (length >= sizeof(body->body) - body->length) return ESP_FAIL;
    memcpy(body->body + body->length, event->data, length);
    body->length += length;
    body->body[body->length] = '\0';
    return ESP_OK;
}

static bool weather_json_number(const char *json, const char *key, double *out)
{
    if (!json || !key || !out) return false;
    char quoted_key[40];
    const int key_len = snprintf(quoted_key, sizeof(quoted_key), "\"%s\"", key);
    if (key_len <= 0 || (size_t)key_len >= sizeof(quoted_key)) return false;

    /* open-meteo 在同一响应里对同名键可能出现两次：current_units 里是字符串
     * （如 "temperature_2m":"°C"），current 里才是数值。strstr 会先命中
     * 前面的字符串项导致 strtod 失败。因此遇到非数值匹配要继续往后找，
     * 直到真正的数值项。 */
    const char *cursor = json;
    while ((cursor = strstr(cursor, quoted_key)) != NULL) {
        const char *p = cursor + key_len;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (*p != ':') { cursor += 1; continue; }
        ++p;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        char *end = NULL;
        const double value = strtod(p, &end);
        if (end != p) { /* 解析出了数值 */
            if (value != value || value > 1000000.0 || value < -1000000.0) {
                return false; /* 数值确实非法 */
            }
            *out = value;
            return true;
        }
        /* 非数值（如 "°C"），继续找下一处同名键。 */
        cursor += 1;
    }
    return false;
}

static const char *weather_summary_for_code(int code)
{
    if (code == 0) return "晴";
    if (code == 1 || code == 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "小雨";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "降雪";
    if (code >= 95 && code <= 99) return "雷雨";
    return "天气";
}

static bool weather_fetch_once(void)
{
    if (!s_app_state.wifi_connected) return false;
    nvs_store_config_t config;
    if (!app_config_lock(s_config_mutex)) return false;
    config = s_app_state.config;
    app_config_unlock(s_config_mutex);
    if (config.latitude_e7 == 0 && config.longitude_e7 == 0) {
        ESP_LOGI(TAG, "weather skipped: no location configured");
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.7f&longitude=%.7f&current=temperature_2m,weather_code&timezone=auto",
             (double)config.latitude_e7 / 10000000.0,
             (double)config.longitude_e7 / 10000000.0);
    weather_http_body_t body = {0};
    const esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 12000,
        .event_handler = weather_http_event,
        .user_data = &body,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) return false;
    const esp_err_t request_err = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    double temperature = 0.0;
    double weather_code = -1.0;
    const bool parsed = body.length != 0 &&
        weather_json_number(body.body, "temperature_2m", &temperature) &&
        weather_json_number(body.body, "weather_code", &weather_code);
    if (request_err != ESP_OK || status_code != 200 || !parsed ||
        temperature < -100.0 || temperature > 100.0 ||
        weather_code < 0 || weather_code > 255) {
        weather_client_mark_failure(&s_weather_client,
                                    (uint64_t)time(NULL));
        s_app_state.weather_stale = s_weather_client.last.valid;
        ESP_LOGW(TAG, "weather update failed (err=%s http=%d)",
                 esp_err_to_name(request_err), status_code);
        if (s_weather_client.last.valid) {
            submit_refresh(REFRESH_REASON_WEATHER, false);
        }
        return false;
    }

    const time_t now = time(NULL);
    weather_data_t data = {
        .latitude_e7 = config.latitude_e7,
        .longitude_e7 = config.longitude_e7,
        .temperature_c10 = (int16_t)(temperature * 10.0 +
                                     (temperature < 0 ? -0.5 : 0.5)),
        .updated_unix = now > 0 ? (uint64_t)now : 0,
        .valid = true,
        .stale = false,
    };
    strlcpy(data.summary, weather_summary_for_code((int)weather_code),
            sizeof(data.summary));
    weather_client_mark_success(&s_weather_client, &data);
    s_app_state.weather_valid = true;
    s_app_state.weather_stale = false;
    s_app_state.weather_temperature_c10 = data.temperature_c10;
    s_app_state.weather_updated_unix = data.updated_unix;
    if (app_config_lock(s_config_mutex)) {
        s_app_state.config.weather_cache_unix = data.updated_unix;
        (void)nvs_store_save(&s_app_state.config);
        app_config_unlock(s_config_mutex);
    }
    ESP_LOGI(TAG, "weather updated: %s %.1f C",
             data.summary, temperature);
    submit_refresh(REFRESH_REASON_WEATHER, false);
    return true;
}

static void weather_task(void *arg)
{
    (void)arg;
    uint8_t request;
    for (;;) {
        const bool explicit_request = xQueueReceive(
            s_weather_request_queue, &request, pdMS_TO_TICKS(60000)) == pdTRUE;
        const time_t now = time(NULL);
        const bool scheduled = now >= 1000000000 &&
            weather_client_due(&s_weather_client, (uint64_t)now);
        if ((!explicit_request && !scheduled) || !s_app_state.wifi_connected) {
            continue;
        }
        s_weather_client.request_in_flight = true;
        (void)weather_fetch_once();
        s_weather_client.request_in_flight = false;
    }
}

static bool start_weather_task(void)
{
    if (s_weather_task_handle) return true;
    const BaseType_t result = xTaskCreate(weather_task, "weather", 12288,
                                          NULL, 3, &s_weather_task_handle);
    if (result != pdPASS) {
        s_weather_task_handle = NULL;
        return false;
    }
    ESP_LOGI(TAG, "weather task started; heap=%u bytes",
             (unsigned)esp_get_free_heap_size());
    return true;
}

static esp_err_t network_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     network_event_handler, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     network_event_handler, NULL);
    if (err != ESP_OK) return err;
    wifi_manager_init(&s_wifi_manager, &s_app_state.config);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    wifi_config_t sta = {0};
    if (wifi_manager_has_credentials(&s_app_state.config)) {
        memcpy(sta.sta.ssid, s_app_state.config.wifi_ssid,
               strnlen(s_app_state.config.wifi_ssid, sizeof(sta.sta.ssid)));
        memcpy(sta.sta.password, s_app_state.config.wifi_password,
               strnlen(s_app_state.config.wifi_password,
                       sizeof(sta.sta.password)));
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err == ESP_OK) {
        /* Apply before start so the first STA frame is capped;
         * repeat after start because some controller revisions only accept
         * the setter once the driver is running. */
        (void)wifi_manager_apply_tx_limit();
        s_wifi_started = true;
        err = esp_wifi_start();
        if (err != ESP_OK) s_wifi_started = false;
    }
    if (err == ESP_OK) {
        (void)wifi_manager_apply_tx_limit();
    }
    return err;
}
