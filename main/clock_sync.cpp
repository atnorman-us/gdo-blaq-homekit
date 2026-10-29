#include "clock_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "close_schedule.h"

static const char *TAG = "clock";

extern "C" void clock_sync_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SNTP started");
}

extern "C" esp_err_t clock_sync_apply_timezone(const char *posix_tz)
{
    if (!close_schedule_tz_valid(posix_tz)) return ESP_ERR_INVALID_ARG;
    if (setenv("TZ", posix_tz, 1) != 0) return ESP_FAIL;
    tzset();
    return ESP_OK;
}

extern "C" bool clock_sync_is_synced(void)
{
    return time(nullptr) >= CLOSE_SCHEDULE_MIN_VALID_EPOCH;
}

extern "C" bool clock_sync_local_now(uint16_t *minute_of_day, int32_t *day_number)
{
    time_t now = time(nullptr);
    if (now < CLOSE_SCHEDULE_MIN_VALID_EPOCH) return false;
    struct tm local;
    localtime_r(&now, &local);
    *minute_of_day = (uint16_t)(local.tm_hour * 60 + local.tm_min);
    *day_number = close_schedule_days_from_civil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return true;
}

extern "C" void clock_sync_format_local(char *out, size_t out_len)
{
    time_t now = time(nullptr);
    if (now < CLOSE_SCHEDULE_MIN_VALID_EPOCH) {
        snprintf(out, out_len, "unsynced");
        return;
    }
    struct tm local;
    localtime_r(&now, &local);
    strftime(out, out_len, "%Y-%m-%d %H:%M", &local);
}
