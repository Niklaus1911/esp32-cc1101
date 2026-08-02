#include "rf_activity_led_private.hpp"

#include <atomic>
#include <cstdint>
#include <iterator>
#include <limits>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "rf_activity_led_policy.hpp"
#include "sdkconfig.h"

namespace rfbridge {
namespace {

constexpr char kTag[] = "rf_led";

#if CONFIG_RF_ACTIVITY_LED_ENABLE

#if CONFIG_RF_ACTIVITY_LED_ACTIVE_HIGH
constexpr bool kActiveHigh = true;
#else
constexpr bool kActiveHigh = false;
#endif

constexpr RfActivityLedConfig kConfig{
    .enabled = true,
    .gpio = CONFIG_RF_ACTIVITY_LED_GPIO,
    .active_high = kActiveHigh,
    .pulse_ms = CONFIG_RF_ACTIVITY_LED_PULSE_MS,
};
constexpr int kUnavailableGpios[] = {
    CONFIG_CC1101_SPI_SCLK_GPIO,
    CONFIG_CC1101_SPI_MISO_GPIO,
    CONFIG_CC1101_SPI_MOSI_GPIO,
    CONFIG_CC1101_SPI_CS_GPIO,
    CONFIG_CC1101_GDO0_GPIO,
    CONFIG_CC1101_GDO2_GPIO,
};

std::atomic<bool> s_initialized{false};
portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
int64_t s_deadline_us = 0;
esp_timer_handle_t s_timer = nullptr;

void set_inactive()
{
    gpio_set_level(static_cast<gpio_num_t>(kConfig.gpio), rf_activity_led_inactive_level(kConfig));
}

esp_err_t arm_timer(uint64_t timeout_us)
{
    esp_err_t error = esp_timer_restart(s_timer, timeout_us);
    if (error == ESP_ERR_INVALID_STATE) {
        error = esp_timer_start_once(s_timer, timeout_us);
    }
    if (error == ESP_ERR_INVALID_STATE && esp_timer_is_active(s_timer)) {
        return ESP_OK;
    }
    return error;
}

void fail_dark_if_deadline_is_current(int64_t deadline_us)
{
    portENTER_CRITICAL(&s_state_mux);
    if (s_deadline_us == deadline_us) {
        s_deadline_us = 0;
        set_inactive();
    }
    portEXIT_CRITICAL(&s_state_mux);
}

void timer_callback(void *)
{
    const int64_t now_us = esp_timer_get_time();
    int64_t deadline_us = 0;
    RfActivityLedDeadlineDecision decision{};
    portENTER_CRITICAL(&s_state_mux);
    deadline_us = s_deadline_us;
    decision = rf_activity_led_deadline_decision(now_us, deadline_us);
    if (decision.turn_off && deadline_us != 0) {
        s_deadline_us = 0;
        set_inactive();
    }
    portEXIT_CRITICAL(&s_state_mux);
    if (decision.turn_off) {
        return;
    }

    const esp_err_t error = esp_timer_start_once(s_timer, decision.rearm_us);
    if (error != ESP_OK &&
        !(error == ESP_ERR_INVALID_STATE && esp_timer_is_active(s_timer))) {
        fail_dark_if_deadline_is_current(deadline_us);
    }
}

#endif

}  // namespace

esp_err_t initialize_rf_activity_led()
{
#if !CONFIG_RF_ACTIVITY_LED_ENABLE
    return ESP_OK;
#else
    if (s_initialized.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    if (!rf_activity_led_config_is_valid(kConfig, kUnavailableGpios,
                                         std::size(kUnavailableGpios)) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(kConfig.gpio)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_timer_create_args_t timer_config{
        .callback = timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "rf_rx_led",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t timer = nullptr;
    esp_err_t error = esp_timer_create(&timer_config, &timer);
    if (error != ESP_OK) {
        return error;
    }

    const gpio_config_t gpio_settings{
        .pin_bit_mask = 1ULL << static_cast<unsigned>(kConfig.gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&gpio_settings);
    if (error == ESP_OK) {
        error = gpio_set_level(static_cast<gpio_num_t>(kConfig.gpio),
                               rf_activity_led_inactive_level(kConfig));
    }
    if (error != ESP_OK) {
        gpio_reset_pin(static_cast<gpio_num_t>(kConfig.gpio));
        esp_timer_delete(timer);
        return error;
    }

    s_timer = timer;
    s_deadline_us = 0;
    s_initialized.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "RX activity LED GPIO%d active-%s pulse=%lu ms", kConfig.gpio,
             kConfig.active_high ? "high" : "low",
             static_cast<unsigned long>(kConfig.pulse_ms));
    return ESP_OK;
#endif
}

void notify_rf_activity_led()
{
#if CONFIG_RF_ACTIVITY_LED_ENABLE
    if (!s_initialized.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t pulse_us = rf_activity_led_pulse_us(kConfig);
    const int64_t now_us = esp_timer_get_time();
    const int64_t deadline_us =
        now_us > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(pulse_us)
            ? std::numeric_limits<int64_t>::max()
            : now_us + static_cast<int64_t>(pulse_us);
    portENTER_CRITICAL(&s_state_mux);
    s_deadline_us = deadline_us;
    const esp_err_t level_error =
        gpio_set_level(static_cast<gpio_num_t>(kConfig.gpio),
                       rf_activity_led_active_level(kConfig));
    portEXIT_CRITICAL(&s_state_mux);

    const esp_err_t timer_error = arm_timer(pulse_us);
    if (level_error != ESP_OK || timer_error != ESP_OK) {
        fail_dark_if_deadline_is_current(deadline_us);
    }
#endif
}

}  // namespace rfbridge
