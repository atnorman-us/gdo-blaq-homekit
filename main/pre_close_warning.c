// pre_close_warning.c
//
// See pre_close_warning.h for usage/integration notes.

#include "pre_close_warning.h"

#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "pre_close_warning";

#define BUZZER_LEDC_TIMER      LEDC_TIMER_1
#define BUZZER_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_CHANNEL    LEDC_CHANNEL_1
#define BUZZER_LEDC_DUTY_RES   LEDC_TIMER_10_BIT
#define BUZZER_TONE_HZ         2700   // typical piezo resonant tone; tune to your buzzer
#define BUZZER_DUTY_ON         512    // ~50% duty at 10-bit resolution
#define BUZZER_DUTY_OFF        0

void pre_close_warning_init(void)
{
    // --- Buzzer: LEDC PWM channel ---
    ledc_timer_config_t timer_conf = {
        .speed_mode       = BUZZER_LEDC_MODE,
        .duty_resolution  = BUZZER_LEDC_DUTY_RES,
        .timer_num        = BUZZER_LEDC_TIMER,
        .freq_hz          = BUZZER_TONE_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t channel_conf = {
        .gpio_num       = PRE_CLOSE_WARNING_BUZZER_GPIO,
        .speed_mode     = BUZZER_LEDC_MODE,
        .channel        = BUZZER_LEDC_CHANNEL,
        .timer_sel      = BUZZER_LEDC_TIMER,
        .duty           = BUZZER_DUTY_OFF,
        .hpoint         = 0,
    };
    ledc_channel_config(&channel_conf);

    // --- LED: plain GPIO strobe ---
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << PRE_CLOSE_WARNING_LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    gpio_set_level(PRE_CLOSE_WARNING_LED_GPIO, 0);

    ESP_LOGI(TAG, "pre-close warning peripherals initialized (buzzer GPIO%d, LED GPIO%d)",
             PRE_CLOSE_WARNING_BUZZER_GPIO, PRE_CLOSE_WARNING_LED_GPIO);
}

static inline void buzzer_set(bool on)
{
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, on ? BUZZER_DUTY_ON : BUZZER_DUTY_OFF);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

static inline void led_set(bool on)
{
    gpio_set_level(PRE_CLOSE_WARNING_LED_GPIO, on ? 1 : 0);
}

void pre_close_warning_run(uint32_t duration_ms)
{
    ESP_LOGI(TAG, "pre-close warning: running for %u ms", (unsigned)duration_ms);

    uint32_t elapsed = 0;
    const uint32_t cycle_ms = PRE_CLOSE_WARNING_BEEP_ON_MS + PRE_CLOSE_WARNING_BEEP_OFF_MS;

    while (elapsed < duration_ms) {
        buzzer_set(true);
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(PRE_CLOSE_WARNING_BEEP_ON_MS));

        buzzer_set(false);
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(PRE_CLOSE_WARNING_BEEP_OFF_MS));

        elapsed += cycle_ms;
    }

    // ensure both are off when we return
    buzzer_set(false);
    led_set(false);
    ESP_LOGI(TAG, "pre-close warning: complete");
}

typedef struct {
    uint32_t duration_ms;
    void (*on_complete)(void);
} warning_task_args_t;

static void warning_task(void *pvArgs)
{
    warning_task_args_t *args = (warning_task_args_t *)pvArgs;
    pre_close_warning_run(args->duration_ms);
    void (*cb)(void) = args->on_complete;
    vPortFree(args);
    if (cb) {
        cb();
    }
    vTaskDelete(NULL);
}

void pre_close_warning_run_async(uint32_t duration_ms, void (*on_complete)(void))
{
    warning_task_args_t *args = pvPortMalloc(sizeof(warning_task_args_t));
    if (!args) {
        ESP_LOGE(TAG, "failed to allocate warning task args");
        if (on_complete) on_complete();
        return;
    }
    args->duration_ms = duration_ms;
    args->on_complete = on_complete;

    // 2048 was too tight in practice: this task's on_complete callback can
    // go on to call HomeKit SDK notification functions (e.g.
    // notify_homekit_target_door_state_change() in an auto-close
    // completion callback), which may touch encrypted-session crypto for
    // connected HAP controllers - confirmed via a real device panic/reset
    // instead of closing, correlated with auto-close (the only caller of
    // this async variant) firing for the first time on real hardware.
    xTaskCreate(warning_task, "pre_close_warn", 8192, args, tskIDLE_PRIORITY + 1, NULL);
}