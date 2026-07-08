#include "diag_webserver.h"
#include "log_ring_buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gdo.h"

// Small read-only accessors defined alongside the state they own, so this
// file doesn't need its own copies of "last known" state. See
// gdo-blaq-homekit.cpp and homekit.cpp for the implementations.
extern "C" void gdo_diag_get_last_states(gdo_door_state_t *door,
                                          gdo_light_state_t *light,
                                          gdo_lock_state_t *lock,
                                          gdo_obstruction_state_t *obstruction,
                                          gdo_motion_state_t *motion);
extern "C" int homekit_get_connected_session_count(void);

static const char *TAG = "DIAGWEB";
#define DIAG_WEB_PORT 8080
#define LOG_BUFFER_CAPACITY (64 * 1024)

static const char *reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:   return "Power-on";
        case ESP_RST_EXT:       return "External pin";
        case ESP_RST_SW:        return "Software (esp_restart)";
        case ESP_RST_PANIC:     return "Panic/exception";
        case ESP_RST_INT_WDT:   return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "Task watchdog";
        case ESP_RST_WDT:       return "Other watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT:  return "Brownout";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "Unknown";
    }
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    // No actual icon - just answer with an empty 204 instead of a 404 so
    // the browser's automatic favicon fetch doesn't spam the log on every
    // page load.
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char *page =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>GDO Diagnostics</title>"
        "<style>"
        "body{font-family:-apple-system,sans-serif;background:#111;color:#ddd;margin:0;padding:16px;}"
        "h1{font-size:18px;} h2{font-size:14px;color:#8ab4f8;margin-top:24px;}"
        "table{border-collapse:collapse;} td{padding:2px 12px 2px 0;font-size:13px;}"
        "td.k{color:#9aa0a6;} pre{background:#000;color:#0f0;padding:10px;overflow:auto;"
        "max-height:60vh;font-size:12px;white-space:pre-wrap;word-break:break-all;"
        "border-radius:4px;}"
        "button,a.btn{background:#2a2a2a;color:#ddd;border:1px solid #444;padding:6px 12px;"
        "border-radius:4px;cursor:pointer;text-decoration:none;display:inline-block;"
        "margin-right:8px;font-size:13px;}"
        "label{color:#9aa0a6;font-size:13px;margin-left:4px;}"
        "</style></head><body>"
        "<h1>GDO Diagnostics</h1>"
        "<h2>Status</h2><table id='status'></table>"
        "<h2>Log <span id='loginfo' style='color:#9aa0a6;font-weight:normal;'></span></h2>"
        "<div style='margin-bottom:8px;'>"
        "<button onclick='refresh()'>Refresh now</button>"
        "<a class='btn' href='/logs/download'>Download full log</a>"
        "<label><input type='checkbox' id='auto' checked> auto-refresh (3s)</label>"
        "</div>"
        "<pre id='log'>Loading...</pre>"
        "<script>"
        "async function refresh(){"
        "  try{"
        "    const s = await (await fetch('/status')).json();"
        "    document.getElementById('status').innerHTML = Object.entries(s).map(function(e){"
        "      return \"<tr><td class='k'>\"+e[0]+\"</td><td>\"+e[1]+\"</td></tr>\";"
        "    }).join('');"
        "  }catch(e){}"
        "  try{"
        "    const r = await fetch('/logs');"
        "    const txt = await r.text();"
        "    const pre = document.getElementById('log');"
        "    const wasAtBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 20;"
        "    pre.textContent = txt;"
        "    document.getElementById('loginfo').textContent = '(' + txt.length + ' bytes shown)';"
        "    if (wasAtBottom) pre.scrollTop = pre.scrollHeight;"
        "  }catch(e){}"
        "}"
        "refresh();"
        "setInterval(function(){ if(document.getElementById('auto').checked) refresh(); }, 3000);"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    gdo_door_state_t door;
    gdo_light_state_t light;
    gdo_lock_state_t lock;
    gdo_obstruction_state_t obstruction;
    gdo_motion_state_t motion;
    gdo_diag_get_last_states(&door, &light, &lock, &obstruction, &motion);

    wifi_ap_record_t ap_info;
    bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    int rssi = wifi_ok ? ap_info.rssi : 0;

    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"door\":\"%s\","
        "\"light\":\"%s\","
        "\"lock\":\"%s\","
        "\"obstruction\":\"%s\","
        "\"motion\":\"%s\","
        "\"uptime_s\":%lld,"
        "\"free_heap\":%u,"
        "\"min_free_heap\":%u,"
        "\"wifi_rssi\":%d,"
        "\"wifi_connected\":%s,"
        "\"hap_sessions\":%d,"
        "\"last_reset_reason\":\"%s\","
        "\"log_buffer_used\":%u,"
        "\"log_buffer_capacity\":%u"
        "}",
        gdo_door_state_to_string(door),
        gdo_light_state_to_string(light),
        gdo_lock_state_to_string(lock),
        gdo_obstruction_state_to_string(obstruction),
        gdo_motion_state_to_string(motion),
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        rssi,
        wifi_ok ? "true" : "false",
        homekit_get_connected_session_count(),
        reset_reason_to_string(esp_reset_reason()),
        (unsigned)log_ring_buffer_used(),
        (unsigned)log_ring_buffer_capacity());

    httpd_resp_set_type(req, "application/json");
    if (n > 0 && (size_t)n < sizeof(buf)) {
        return httpd_resp_send(req, buf, n);
    }
    return httpd_resp_send(req, "{}", 2);
}

// Shared by the inline log view and the download route - only the response
// headers differ (Content-Disposition on the download variant).
static esp_err_t send_log_buffer(httpd_req_t *req, bool as_attachment)
{
    size_t capacity = log_ring_buffer_capacity();
    if (capacity == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "Log buffer not initialized.", HTTPD_RESP_USE_STRLEN);
    }

    char *tmp = (char *)malloc(capacity);
    if (!tmp) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t n = log_ring_buffer_read(tmp, capacity);

    httpd_resp_set_type(req, "text/plain");
    if (as_attachment) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"gdo-log.txt\"");
    }
    esp_err_t err = httpd_resp_send(req, tmp, n);
    free(tmp);
    return err;
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    return send_log_buffer(req, false);
}

static esp_err_t logs_download_get_handler(httpd_req_t *req)
{
    return send_log_buffer(req, true);
}

static void start_httpd_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = DIAG_WEB_PORT;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start diagnostics web server on port %d", DIAG_WEB_PORT);
        return;
    }

    httpd_uri_t root_uri = {};
    root_uri.uri = "/";
    root_uri.method = HTTP_GET;
    root_uri.handler = root_get_handler;

    httpd_uri_t status_uri = {};
    status_uri.uri = "/status";
    status_uri.method = HTTP_GET;
    status_uri.handler = status_get_handler;

    httpd_uri_t logs_uri = {};
    logs_uri.uri = "/logs";
    logs_uri.method = HTTP_GET;
    logs_uri.handler = logs_get_handler;

    httpd_uri_t logs_dl_uri = {};
    logs_dl_uri.uri = "/logs/download";
    logs_dl_uri.method = HTTP_GET;
    logs_dl_uri.handler = logs_download_get_handler;

    httpd_uri_t favicon_uri = {};
    favicon_uri.uri = "/favicon.ico";
    favicon_uri.method = HTTP_GET;
    favicon_uri.handler = favicon_get_handler;

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &logs_uri);
    httpd_register_uri_handler(server, &logs_dl_uri);
    httpd_register_uri_handler(server, &favicon_uri);

    ESP_LOGI(TAG, "Diagnostics web server started on port %d", DIAG_WEB_PORT);
}

// httpd_start() needs LWIP's core TCP/IP task already running, which
// esp_netif_init() (called from WiFi setup, in a different task than
// app_main()) is responsible for starting. Calling httpd_start() before
// that's happened crashes with "assert failed: tcpip_send_msg_wait_sem
// ... Invalid mbox" - and since app_main() has no guaranteed ordering
// relative to when WiFi setup actually runs, we can't just call
// start_httpd_server() directly from diag_webserver_start(). Poll for an
// assigned IP instead, which can only happen after the network stack is
// genuinely ready, then start the server from this dedicated task.
static void wait_for_network_and_start_task(void *arg)
{
    ESP_LOGI(TAG, "Waiting for network before starting diagnostics web server...");

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    int attempts = 0;

    while (true) {
        bool have_netif = (netif != nullptr);
        bool have_ip = have_netif &&
                        esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
                        ip_info.ip.addr != 0;

        if (have_ip) {
            break;
        }

        attempts++;
        if (attempts % 10 == 0) {
            // Log roughly every 5s so a stuck wait is visible instead of silent.
            ESP_LOGW(TAG, "Still waiting for network (netif=%s, attempt %d)...",
                     have_netif ? "found" : "NULL - ifkey WIFI_STA_DEF not registered yet", attempts);
            // If the netif key never resolves, retry the lookup - it may not
            // have been registered yet when this task first ran.
            if (!have_netif) {
                netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "Network ready, starting diagnostics web server...");
    start_httpd_server();
    vTaskDelete(NULL);
}

void diag_webserver_start(void)
{
    ESP_LOGI(TAG, "diag_webserver_start() called");

    // In-memory only, by design - see log_ring_buffer.h. 64KB holds roughly
    // several minutes to tens of minutes of typical activity depending on
    // how chatty the door is; tune if you want more/less history.
    log_ring_buffer_init(LOG_BUFFER_CAPACITY);

    // Safe to call this immediately from app_main() - the actual httpd
    // startup is deferred until the network is confirmed ready (see
    // wait_for_network_and_start_task above).
    xTaskCreate(wait_for_network_and_start_task, "diag_web_wait", 3072, NULL,
                tskIDLE_PRIORITY + 1, NULL);
}