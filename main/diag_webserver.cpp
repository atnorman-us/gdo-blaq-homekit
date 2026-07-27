#include "diag_webserver.h"
#include "log_ring_buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "gdo.h"
#include "gdo_settings.h"
#include "diagnostics_counters.h"

// Small read-only accessors defined alongside the state they own, so this
// file doesn't need its own copies of "last known" state. See
// gdo-blaq-homekit.cpp and homekit.cpp for the implementations.
extern "C" void gdo_diag_get_last_states(gdo_door_state_t *door,
                                          gdo_light_state_t *light,
                                          gdo_lock_state_t *lock,
                                          gdo_obstruction_state_t *obstruction,
                                          gdo_motion_state_t *motion);
extern "C" int homekit_get_connected_session_count(void);

// Auto-close accessors now come from gdo_settings.h above, instead of a
// second hand-maintained copy of the same declarations here - see that
// header for why.

static const char *TAG = "DIAGWEB";
#define DIAG_WEB_PORT 8080
#define LOG_BUFFER_CAPACITY (64 * 1024)
#define DIAG_MDNS_HOSTNAME "gdo-blaq"

// Persistent so diag_webserver_restart() can stop and recreate it after a
// WiFi bounce - the underlying httpd socket doesn't reliably survive an
// interface going down and coming back up.
static httpd_handle_t s_server = nullptr;

// Reused across every /logs and /logs/download request instead of
// malloc()/free() per request. The dashboard auto-refreshes /logs every 3s
// while open - repeatedly allocating and freeing a 64KB block that often
// can fragment the heap over a long uptime, occasionally failing an
// allocation and surfacing as a spurious 500 error. Allocating once at
// startup avoids that entirely.
static char *s_log_scratch = nullptr;
static SemaphoreHandle_t s_log_scratch_mutex = nullptr;

static void ensure_log_scratch_buffer(size_t capacity)
{
    if (!s_log_scratch) {
        s_log_scratch = (char *)malloc(capacity);
        if (!s_log_scratch) {
            ESP_LOGE(TAG, "Failed to allocate %u byte log scratch buffer", (unsigned)capacity);
        }
    }
    if (!s_log_scratch_mutex) {
        s_log_scratch_mutex = xSemaphoreCreateMutex();
    }
}

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

static void delayed_restart_task(void *arg)
{
    // Give the HTTP response time to actually flush over the socket before
    // rebooting - esp_restart() doesn't wait for pending sends.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t restart_post_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Restart requested via diagnostics web server");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Restarting...", HTTPD_RESP_USE_STRLEN);

    xTaskCreate(delayed_restart_task, "diag_restart", 2048, NULL,
                tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
}

// Only one upload can be in flight at a time - guards against a second
// concurrent POST racing the same esp_ota_handle_t. The httpd task processes
// requests one at a time by default (no worker pool is configured here), so
// this is belt-and-suspenders rather than a load-bearing lock.
static bool s_ota_upload_in_progress = false;

#define OTA_UPLOAD_CHUNK_SIZE 4096

// Two-step upload: /firmware/update validates and writes the image but
// stops short of committing it as the boot target, so the browser can show
// the detected version and let the user confirm before the device reboots
// into it. The image sits harmlessly on flash either way - a second upload
// (or a fresh confirm/cancel cycle) just overwrites the same inactive
// partition, so there's nothing to explicitly "undo" on cancel/expiry.
static const esp_partition_t *s_ota_pending_partition = nullptr;
static int64_t s_ota_pending_since_ms = 0;
static char s_ota_pending_version[32] = {0};
#define OTA_CONFIRM_EXPIRY_MS (10 * 60 * 1000)

static void ota_clear_pending(void)
{
    s_ota_pending_partition = nullptr;
    s_ota_pending_since_ms = 0;
    s_ota_pending_version[0] = '\0';
}

static esp_err_t firmware_update_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED, "Content-Length required");
        return ESP_FAIL;
    }

    if (s_ota_upload_in_progress) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "An upload is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "No OTA update partition available");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if ((size_t)req->content_len > target->size) {
        ESP_LOGW(TAG, "Firmware upload (%d bytes) too large for OTA partition (%u bytes)",
                 req->content_len, (unsigned)target->size);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Image larger than OTA partition");
        return ESP_FAIL;
    }

    char *chunk = (char *)malloc(OTA_UPLOAD_CHUNK_SIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "Failed to allocate %u byte OTA upload chunk buffer", OTA_UPLOAD_CHUNK_SIZE);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    s_ota_upload_in_progress = true;
    ota_clear_pending();

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin() failed: %s", esp_err_to_name(err));
        free(chunk);
        s_ota_upload_in_progress = false;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Firmware upload starting: %d bytes -> partition '%s'",
             req->content_len, target->label);

    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int to_read = remaining < OTA_UPLOAD_CHUNK_SIZE ? remaining : OTA_UPLOAD_CHUNK_SIZE;
        int received = httpd_req_recv(req, chunk, to_read);
        if (received <= 0) {
            ESP_LOGE(TAG, "Firmware upload: recv failed/closed early (%d bytes remaining)", remaining);
            ok = false;
            break;
        }

        err = esp_ota_write(ota_handle, chunk, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write() failed: %s", esp_err_to_name(err));
            ok = false;
            break;
        }

        remaining -= received;
    }

    free(chunk);

    if (!ok) {
        esp_ota_abort(ota_handle);
        s_ota_upload_in_progress = false;
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    s_ota_upload_in_progress = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end() failed (invalid image?): %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image");
        return ESP_FAIL;
    }

    esp_app_desc_t app_desc = {};
    esp_err_t desc_err = esp_ota_get_partition_description(target, &app_desc);
    const char *version = (desc_err == ESP_OK) ? app_desc.version : "unknown";

    s_ota_pending_partition = target;
    s_ota_pending_since_ms = esp_timer_get_time() / 1000;
    snprintf(s_ota_pending_version, sizeof(s_ota_pending_version), "%s", version);

    ESP_LOGI(TAG, "Firmware validated: version '%s' written to partition '%s', awaiting confirm",
             version, target->label);

    char resp[128];
    int n = snprintf(resp, sizeof(resp), "{\"partition\":\"%s\",\"version\":\"%s\",\"size\":%d}",
                      target->label, version, req->content_len);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, (n > 0 && (size_t)n < sizeof(resp)) ? n : HTTPD_RESP_USE_STRLEN);
}

static esp_err_t firmware_confirm_post_handler(httpd_req_t *req)
{
    if (!s_ota_pending_partition) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No pending upload to confirm");
        return ESP_FAIL;
    }

    int64_t age_ms = (esp_timer_get_time() / 1000) - s_ota_pending_since_ms;
    if (age_ms > OTA_CONFIRM_EXPIRY_MS) {
        ESP_LOGW(TAG, "Pending firmware confirm expired (%lld ms old) - re-upload required",
                 (long long)age_ms);
        ota_clear_pending();
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Pending upload expired - upload again");
        return ESP_FAIL;
    }

    const esp_partition_t *target = s_ota_pending_partition;
    esp_err_t err = esp_ota_set_boot_partition(target);
    ota_clear_pending();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition() failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Firmware confirmed, booting '%s' next - restarting", target->label);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK, restarting...", HTTPD_RESP_USE_STRLEN);

    xTaskCreate(delayed_restart_task, "diag_restart", 2048, NULL,
                tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
}

static esp_err_t firmware_cancel_post_handler(httpd_req_t *req)
{
    // Best-effort: the uploaded image is left in place on flash (it'll just
    // get overwritten by the next upload) - this only clears the
    // server-side "waiting to be confirmed" state so a stray/late confirm
    // can't accidentally commit an upload the user backed out of.
    ota_clear_pending();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t firmware_rollback_post_handler(httpd_req_t *req)
{
    if (s_ota_upload_in_progress) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "An upload is in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t app_desc = {};
    if (!other || esp_ota_get_partition_description(other, &app_desc) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "Other OTA slot has no valid image to roll back to");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition() failed during rollback: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Manual rollback requested, booting '%s' (version '%s') next - restarting",
             other->label, app_desc.version);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK, restarting...", HTTPD_RESP_USE_STRLEN);

    xTaskCreate(delayed_restart_task, "diag_restart", 2048, NULL,
                tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
}

// Body: application/x-www-form-urlencoded, e.g. "enabled=1&minutes=45".
// Neither value can contain characters needing percent-decoding (a
// checkbox and a plain integer), so a simple substring/strtoul parse is
// enough - no need to pull in a URL-decoding helper for this.
static esp_err_t auto_close_settings_post_handler(httpd_req_t *req)
{
    char buf[64] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request body");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    bool enabled = (strstr(buf, "enabled=1") != nullptr);

    uint32_t minutes = 60;
    const char *m = strstr(buf, "minutes=");
    if (m) {
        minutes = (uint32_t)strtoul(m + strlen("minutes="), nullptr, 10);
    }
    if (minutes == 0) {
        // Guard against a 0-minute timeout closing the door essentially
        // instantly after every open - not a real use case, just a bad
        // value to silently accept.
        minutes = 1;
    }

    gdo_set_auto_close_enabled(enabled);
    gdo_set_auto_close_timeout_ms(minutes * 60u * 1000u);

    ESP_LOGI(TAG, "Auto-close settings updated via web: enabled=%s, minutes=%u",
             enabled ? "true" : "false", (unsigned)minutes);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
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
        "button.danger{background:#3a1f1f;border-color:#663;color:#f88;}"
        "label{color:#9aa0a6;font-size:13px;margin-left:4px;}"
        "</style></head><body>"
        "<h1>GDO Diagnostics</h1>"
        "<h2>Status</h2><table id='status'></table>"
        "<h2>Auto-Close</h2>"
        "<div style='margin-bottom:16px;'>"
        "<label><input type='checkbox' id='acEnabled'> Enabled</label>"
        "<label style='margin-left:16px;'>Close after "
        "<input type='number' id='acMinutes' min='1' value='60' "
        "style='width:60px;background:#000;color:#ddd;border:1px solid #444;"
        "border-radius:4px;padding:2px 6px;'> minutes open</label>"
        "<button style='margin-left:16px;' onclick='saveAutoClose()'>Save</button>"
        "<button style='margin-left:8px;' onclick='refreshAutoCloseFields()'>Refresh</button>"
        "<span id='acStatus' style='margin-left:8px;color:#9aa0a6;font-size:13px;'></span>"
        "<div id='acCountdown' style='margin-top:8px;font-size:13px;color:#8ab4f8;'></div>"
        "</div>"
        "<h2>Firmware</h2>"
        "<div style='margin-bottom:16px;'>"
        "<div style='color:#9aa0a6;font-size:13px;margin-bottom:8px;'>"
        "Running: <span id='fwPartition'>?</span> / <span id='fwVersion'>?</span></div>"
        "<div id='fwUploadRow'>"
        "<input type='file' id='fwFile' accept='.bin'>"
        "<button style='margin-left:8px;' onclick='uploadFirmware()'>Upload</button>"
        "</div>"
        "<div id='fwProgressWrap' style='display:none;margin-top:8px;width:260px;"
        "background:#000;border:1px solid #444;border-radius:4px;overflow:hidden;'>"
        "<div id='fwProgressBar' style='height:8px;width:0%;background:#8ab4f8;'></div>"
        "</div>"
        "<div id='fwConfirmRow' style='display:none;margin-top:8px;'>"
        "<button onclick='confirmFirmware()'>Confirm &amp; Reboot</button>"
        "<button style='margin-left:8px;' onclick='cancelFirmware()'>Cancel</button>"
        "</div>"
        "<div id='fwStatus' style='margin-top:6px;font-size:13px;color:#9aa0a6;'></div>"
        "<div style='margin-top:16px;border-top:1px solid #333;padding-top:12px;'>"
        "<div style='color:#9aa0a6;font-size:13px;margin-bottom:8px;'>"
        "Other slot: <span id='fwOtherPartition'>?</span> / <span id='fwOtherVersion'>?</span></div>"
        "<button class='danger' onclick='rollbackFirmware()'>Roll back to other slot</button>"
        "<div id='fwRollbackStatus' style='margin-top:6px;font-size:13px;color:#9aa0a6;'></div>"
        "</div>"
        "</div>"
        "<h2>Log <span id='loginfo' style='color:#9aa0a6;font-weight:normal;'></span></h2>"
        "<div style='margin-bottom:8px;'>"
        "<button onclick='refresh()'>Refresh now</button>"
        "<a class='btn' href='/logs/download'>Download full log</a>"
        "<button class='danger' onclick='restartDevice()'>Restart device</button>"
        "<label><input type='checkbox' id='auto' checked> auto-refresh (3s)</label>"
        "</div>"
        "<pre id='log'>Loading...</pre>"
        "<script>"
        "async function restartDevice(){"
        "  if(!confirm('Restart the device now? HomeKit and the door link will be briefly unavailable.')) return;"
        "  try{"
        "    await fetch('/restart', {method:'POST'});"
        "    document.getElementById('log').textContent = 'Restarting... page will reload automatically in a few seconds.';"
        "    document.getElementById('auto').checked = false;"
        "    setTimeout(()=>{ location.reload(); }, 8000);"
        "  }catch(e){"
        "    document.getElementById('log').textContent = 'Restarting... (connection dropped, as expected). Reload the page in a few seconds.';"
        "  }"
        "}"
        "async function refresh(){"
        "  try{"
        "    const s = await (await fetch('/status')).json();"
        "    document.getElementById('status').innerHTML = Object.entries(s).map(function(e){"
        "      return \"<tr><td class='k'>\"+e[0]+\"</td><td>\"+e[1]+\"</td></tr>\";"
        "    }).join('');"
        "    if (!acLoaded) {"
        "      document.getElementById('acEnabled').checked = !!s.auto_close_enabled;"
        "      document.getElementById('acMinutes').value = s.auto_close_minutes;"
        "      acLoaded = true;"
        "    }"
        "    acRemainingS = (typeof s.auto_close_remaining_s === 'number') ? s.auto_close_remaining_s : null;"
        "    renderCountdown();"
        "    document.getElementById('fwPartition').textContent = s.running_partition || '?';"
        "    document.getElementById('fwVersion').textContent = s.fw_version || '?';"
        "    document.getElementById('fwOtherPartition').textContent = s.other_partition || '?';"
        "    document.getElementById('fwOtherVersion').textContent = s.other_fw_version || '?';"
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
        "let acLoaded = false;"
        "async function refreshAutoCloseFields(){"
        "  const statusEl = document.getElementById('acStatus');"
        "  statusEl.textContent = 'Refreshing...';"
        "  try{"
        "    const s = await (await fetch('/status')).json();"
        "    document.getElementById('acEnabled').checked = !!s.auto_close_enabled;"
        "    document.getElementById('acMinutes').value = s.auto_close_minutes;"
        "    statusEl.textContent = 'Refreshed.';"
        "    setTimeout(function(){ statusEl.textContent = ''; }, 2000);"
        "  }catch(e){"
        "    statusEl.textContent = 'Refresh failed - check connection.';"
        "  }"
        "}"
        "let acRemainingS = null;"
        "function renderCountdown(){"
        "  const el = document.getElementById('acCountdown');"
        "  if (acRemainingS === null || acRemainingS < 0) { el.textContent = ''; return; }"
        "  const m = Math.floor(acRemainingS / 60);"
        "  const s = acRemainingS % 60;"
        "  el.textContent = 'Auto-closing in ' + m + ':' + String(s).padStart(2, '0');"
        "}"
        "setInterval(function(){"
        "  if (acRemainingS !== null && acRemainingS > 0) {"
        "    acRemainingS--;"
        "    renderCountdown();"
        "  }"
        "}, 1000);"
        "async function saveAutoClose(){"
        "  const enabled = document.getElementById('acEnabled').checked ? 1 : 0;"
        "  const minutes = parseInt(document.getElementById('acMinutes').value, 10) || 60;"
        "  const statusEl = document.getElementById('acStatus');"
        "  statusEl.textContent = 'Saving...';"
        "  try{"
        "    await fetch('/settings/auto-close', {"
        "      method: 'POST',"
        "      headers: {'Content-Type': 'application/x-www-form-urlencoded'},"
        "      body: 'enabled=' + enabled + '&minutes=' + minutes"
        "    });"
        "    statusEl.textContent = 'Saved.';"
        "    refresh();"
        "    setTimeout(function(){ statusEl.textContent = ''; }, 3000);"
        "  }catch(e){"
        "    statusEl.textContent = 'Save failed - check connection.';"
        "  }"
        "}"
        "function uploadFirmware(){"
        "  const fileInput = document.getElementById('fwFile');"
        "  const statusEl = document.getElementById('fwStatus');"
        "  const wrap = document.getElementById('fwProgressWrap');"
        "  const bar = document.getElementById('fwProgressBar');"
        "  const file = fileInput.files[0];"
        "  if(!file){ statusEl.textContent = 'Choose a .bin file first.'; return; }"
        "  const xhr = new XMLHttpRequest();"
        "  xhr.open('POST', '/firmware/update');"
        "  document.getElementById('fwUploadRow').style.display = 'none';"
        "  wrap.style.display = 'block';"
        "  bar.style.width = '0%';"
        "  statusEl.textContent = 'Uploading...';"
        "  xhr.upload.onprogress = function(e){"
        "    if(e.lengthComputable){"
        "      const pct = Math.round((e.loaded / e.total) * 100);"
        "      bar.style.width = pct + '%';"
        "      statusEl.textContent = 'Uploading... ' + pct + '%';"
        "    }"
        "  };"
        "  xhr.onload = function(){"
        "    if(xhr.status === 200){"
        "      let info = {};"
        "      try{ info = JSON.parse(xhr.responseText); }catch(e){}"
        "      statusEl.textContent = 'Validated: version ' + (info.version || '?')"
        "        + ' on partition ' + (info.partition || '?') + '. Review before rebooting.';"
        "      document.getElementById('fwConfirmRow').style.display = 'block';"
        "    }else{"
        "      statusEl.textContent = 'Upload failed (' + xhr.status + '): ' + xhr.responseText;"
        "      document.getElementById('fwUploadRow').style.display = 'block';"
        "    }"
        "  };"
        "  xhr.onerror = function(){"
        "    statusEl.textContent = 'Upload failed - check connection.';"
        "    document.getElementById('fwUploadRow').style.display = 'block';"
        "  };"
        "  xhr.send(file);"
        "}"
        "async function confirmFirmware(){"
        "  const statusEl = document.getElementById('fwStatus');"
        "  if(!confirm('Reboot into the uploaded firmware now? '"
        "    + 'HomeKit and the door link will be briefly unavailable.')) return;"
        "  document.getElementById('fwConfirmRow').style.display = 'none';"
        "  try{"
        "    await fetch('/firmware/confirm', {method:'POST'});"
        "    statusEl.textContent = 'Rebooting... page will reload automatically in a few seconds.';"
        "    document.getElementById('auto').checked = false;"
        "    setTimeout(function(){ location.reload(); }, 8000);"
        "  }catch(e){"
        "    statusEl.textContent = 'Confirm failed - check connection.';"
        "  }"
        "}"
        "async function cancelFirmware(){"
        "  const statusEl = document.getElementById('fwStatus');"
        "  try{ await fetch('/firmware/cancel', {method:'POST'}); }catch(e){}"
        "  document.getElementById('fwConfirmRow').style.display = 'none';"
        "  document.getElementById('fwUploadRow').style.display = 'block';"
        "  document.getElementById('fwProgressWrap').style.display = 'none';"
        "  statusEl.textContent = 'Cancelled - upload was not applied.';"
        "}"
        "async function rollbackFirmware(){"
        "  const statusEl = document.getElementById('fwRollbackStatus');"
        "  const other = document.getElementById('fwOtherPartition').textContent;"
        "  const otherVer = document.getElementById('fwOtherVersion').textContent;"
        "  if(otherVer === 'none' || otherVer === '?'){ statusEl.textContent = 'No valid image on the other slot.'; return; }"
        "  if(!confirm('Roll back to ' + other + ' (' + otherVer + ') and reboot now? '"
        "    + 'HomeKit and the door link will be briefly unavailable.')) return;"
        "  try{"
        "    const r = await fetch('/firmware/rollback', {method:'POST'});"
        "    if(r.ok){"
        "      statusEl.textContent = 'Rolling back and rebooting... page will reload automatically in a few seconds.';"
        "      document.getElementById('auto').checked = false;"
        "      setTimeout(function(){ location.reload(); }, 8000);"
        "    }else{"
        "      statusEl.textContent = 'Rollback failed (' + r.status + '): ' + await r.text();"
        "    }"
        "  }catch(e){"
        "    statusEl.textContent = 'Rollback failed - check connection.';"
        "  }"
        "}"
        "refresh();"
        "setInterval(function(){ if(document.getElementById('auto').checked) refresh(); }, 3000);"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

// gdolib's *_to_string() functions are outside this codebase - if any of
// them return NULL for an enum value they don't have a case for (e.g. an
// uninitialized/default-zero protocol value before sync ever completes),
// passing that straight into a %s format crashes with a NULL-pointer read.
// Confirmed via a real crash backtrace landing inside snprintf() in
// status_get_handler(). Wrap every such call so a NULL is survivable.
static inline const char *safe_str(const char *s)
{
    return s ? s : "null";
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    gdo_door_state_t door;
    gdo_light_state_t light;
    gdo_lock_state_t lock;
    gdo_obstruction_state_t obstruction;
    gdo_motion_state_t motion;
    gdo_diag_get_last_states(&door, &light, &lock, &obstruction, &motion);

    // Pulled fresh rather than added to gdo_diag_get_last_states() - the
    // detected protocol has turned out to be non-deterministic across
    // boots on some Sec+2.0 hardware (gdolib's own auto-detection can
    // settle on the wrong protocol), so this is worth surfacing directly
    // rather than only being visible by reading a full serial log.
    gdo_status_t full_status;
    gdo_get_status(&full_status);

    wifi_ap_record_t ap_info;
    bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    int rssi = wifi_ok ? ap_info.rssi : 0;

    // -1 ms / 1000 truncates to 0 in C, not -1 - compute explicitly so the
    // "not counting down" sentinel survives the ms->s conversion intact.
    int64_t auto_close_remaining_ms = gdo_get_auto_close_remaining_ms();
    long long auto_close_remaining_s = (auto_close_remaining_ms < 0)
                                            ? -1
                                            : (long long)(auto_close_remaining_ms / 1000);

    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_app_desc_t *app_desc = esp_app_get_description();

    // The other OTA slot only has a meaningful version once something has
    // actually been written there (e.g. a prior update) - esp_ota_get_
    // partition_description() fails cleanly on a blank/never-written
    // partition, which just means "nothing to roll back to yet".
    const esp_partition_t *other_partition = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t other_app_desc = {};
    bool have_other_version = other_partition &&
        (esp_ota_get_partition_description(other_partition, &other_app_desc) == ESP_OK);

    char buf[1088];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"door\":\"%s\","
        "\"light\":\"%s\","
        "\"lock\":\"%s\","
        "\"obstruction\":\"%s\","
        "\"motion\":\"%s\","
        "\"protocol\":\"%s\","
        "\"gdo_synced\":%s,"
        "\"uptime_s\":%lld,"
        "\"free_heap\":%u,"
        "\"min_free_heap\":%u,"
        "\"wifi_rssi\":%d,"
        "\"wifi_connected\":%s,"
        "\"hap_sessions\":%d,"
        "\"last_reset_reason\":\"%s\","
        "\"log_buffer_used\":%u,"
        "\"log_buffer_capacity\":%u,"
        "\"auto_close_enabled\":%s,"
        "\"auto_close_minutes\":%u,"
        "\"auto_close_remaining_s\":%lld,"
        "\"rx_error_count\":%u,"
        "\"long_obstruction_count\":%u,"
        "\"running_partition\":\"%s\","
        "\"fw_version\":\"%s\","
        "\"other_partition\":\"%s\","
        "\"other_fw_version\":\"%s\""
        "}",
        safe_str(gdo_door_state_to_string(door)),
        safe_str(gdo_light_state_to_string(light)),
        safe_str(gdo_lock_state_to_string(lock)),
        safe_str(gdo_obstruction_state_to_string(obstruction)),
        safe_str(gdo_motion_state_to_string(motion)),
        safe_str(gdo_protocol_type_to_string(full_status.protocol)),
        full_status.synced ? "true" : "false",
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        rssi,
        wifi_ok ? "true" : "false",
        homekit_get_connected_session_count(),
        safe_str(reset_reason_to_string(esp_reset_reason())),
        (unsigned)log_ring_buffer_used(),
        (unsigned)log_ring_buffer_capacity(),
        gdo_get_auto_close_enabled() ? "true" : "false",
        (unsigned)(gdo_get_auto_close_timeout_ms() / 60000),
        auto_close_remaining_s,
        (unsigned)diag_rx_error_count_peek(),
        (unsigned)diag_long_obstruction_count(),
        running_partition ? running_partition->label : "unknown",
        app_desc ? app_desc->version : "unknown",
        have_other_version ? other_partition->label : "none",
        have_other_version ? other_app_desc.version : "none");

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
    if (capacity == 0 || !s_log_scratch || !s_log_scratch_mutex) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "Log buffer not initialized.", HTTPD_RESP_USE_STRLEN);
    }

    if (xSemaphoreTake(s_log_scratch_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for log scratch buffer (concurrent request?)");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t n = log_ring_buffer_read(s_log_scratch, capacity);

    httpd_resp_set_type(req, "text/plain");
    if (as_attachment) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"gdo-log.txt\"");
    }
    esp_err_t err = httpd_resp_send(req, s_log_scratch, n);

    xSemaphoreGive(s_log_scratch_mutex);
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
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    // Flash-write paths in the firmware upload handler want more headroom
    // than the 4KB default, and a ~1.7MB upload over a slow WiFi link
    // shouldn't time out mid-chunk on the default 5s recv timeout.
    config.stack_size = 6144;
    config.recv_wait_timeout = 15;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start diagnostics web server on port %d", DIAG_WEB_PORT);
        s_server = nullptr;
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

    httpd_uri_t restart_uri = {};
    restart_uri.uri = "/restart";
    restart_uri.method = HTTP_POST;
    restart_uri.handler = restart_post_handler;

    httpd_uri_t auto_close_uri = {};
    auto_close_uri.uri = "/settings/auto-close";
    auto_close_uri.method = HTTP_POST;
    auto_close_uri.handler = auto_close_settings_post_handler;

    httpd_uri_t firmware_update_uri = {};
    firmware_update_uri.uri = "/firmware/update";
    firmware_update_uri.method = HTTP_POST;
    firmware_update_uri.handler = firmware_update_post_handler;

    httpd_uri_t firmware_confirm_uri = {};
    firmware_confirm_uri.uri = "/firmware/confirm";
    firmware_confirm_uri.method = HTTP_POST;
    firmware_confirm_uri.handler = firmware_confirm_post_handler;

    httpd_uri_t firmware_cancel_uri = {};
    firmware_cancel_uri.uri = "/firmware/cancel";
    firmware_cancel_uri.method = HTTP_POST;
    firmware_cancel_uri.handler = firmware_cancel_post_handler;

    httpd_uri_t firmware_rollback_uri = {};
    firmware_rollback_uri.uri = "/firmware/rollback";
    firmware_rollback_uri.method = HTTP_POST;
    firmware_rollback_uri.handler = firmware_rollback_post_handler;

    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &logs_uri);
    httpd_register_uri_handler(s_server, &logs_dl_uri);
    httpd_register_uri_handler(s_server, &favicon_uri);
    httpd_register_uri_handler(s_server, &restart_uri);
    httpd_register_uri_handler(s_server, &auto_close_uri);
    httpd_register_uri_handler(s_server, &firmware_update_uri);
    httpd_register_uri_handler(s_server, &firmware_confirm_uri);
    httpd_register_uri_handler(s_server, &firmware_cancel_uri);
    httpd_register_uri_handler(s_server, &firmware_rollback_uri);

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

    // Advertise http://gdo-blaq.local:8080/ so the diagnostics page doesn't
    // require hunting for the device's IP. mdns_init() is safe to call even
    // though esp_hap_core (HomeKit) already calls it during hap_start() -
    // the real implementation just returns ESP_OK if already initialized,
    // regardless of which task got there first. Overriding the hostname
    // afterward (HomeKit itself leaves it at the generic default "MyHost")
    // is a global mDNS responder setting, not per-service, so it also
    // renames the existing HAP service's host reference - harmless, since
    // HomeKit pairing identity lives in the HAP service's TXT/instance
    // records, not the hostname.
    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_hostname_set(DIAG_MDNS_HOSTNAME);
        mdns_instance_name_set("GDO Diagnostics");
        mdns_service_add("GDO Diagnostics", "_http", "_tcp", DIAG_WEB_PORT, NULL, 0);
        ESP_LOGI(TAG, "mDNS advertised as http://%s.local:%d/", DIAG_MDNS_HOSTNAME, DIAG_WEB_PORT);
    } else {
        ESP_LOGW(TAG, "mdns_init() failed: %s - diagnostics page will only be reachable by IP",
                 esp_err_to_name(mdns_err));
    }

    // Reaching here proves this image can bring up WiFi and the diagnostics
    // server, which is as good a "boot succeeded" signal as this app has.
    // If a newly-OTA'd image never gets this far (crash loop, can't join
    // WiFi), CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE means the bootloader
    // reverts to the previous image on the next reset instead of leaving
    // the device stuck - this call is what cancels that pending rollback.
    // Safe to call unconditionally; it's a no-op if no rollback is pending.
    esp_err_t mark_valid_err = esp_ota_mark_app_valid_cancel_rollback();
    if (mark_valid_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback() returned %s",
                 esp_err_to_name(mark_valid_err));
    }

    vTaskDelete(NULL);
}

// Call after a WiFi reconnect (interface went down and came back up). Only
// acts if the server was already running - if it's null, either it was
// never started yet or wait_for_network_and_start_task is still in the
// process of starting it for the first time, and this avoids racing that.
extern "C" void diag_webserver_restart(void)
{
    if (!s_server) {
        return;
    }

    ESP_LOGI(TAG, "Restarting diagnostics web server after network change");
    httpd_stop(s_server);
    s_server = nullptr;
    start_httpd_server();
}

void diag_webserver_start(void)
{
    ESP_LOGI(TAG, "diag_webserver_start() called");

    // In-memory only, by design - see log_ring_buffer.h. 64KB holds roughly
    // several minutes to tens of minutes of typical activity depending on
    // how chatty the door is; tune if you want more/less history.
    log_ring_buffer_init(LOG_BUFFER_CAPACITY);
    ensure_log_scratch_buffer(LOG_BUFFER_CAPACITY);

    // Safe to call this immediately from app_main() - the actual httpd
    // startup is deferred until the network is confirmed ready (see
    // wait_for_network_and_start_task above).
    xTaskCreate(wait_for_network_and_start_task, "diag_web_wait", 3072, NULL,
                tskIDLE_PRIORITY + 1, NULL);
}