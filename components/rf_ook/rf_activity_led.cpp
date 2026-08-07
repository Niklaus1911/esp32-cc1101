#include "rf_activity_led_private.hpp"

#include <atomic>
#include <cstdint>
#include <iterator>
#include <limits>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "platform_board.hpp"
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
enum class LedMode : uint8_t {
    kIdle,
    kStartup,
    kStartupGap,
    kActivity,
};

std::atomic<bool> s_initialized{false};
portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
int64_t s_deadline_us = 0;
esp_timer_handle_t s_timer = nullptr;
LedMode s_mode = LedMode::kIdle;
uint8_t s_startup_phase = 0;
bool s_pending_activity = false;

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
    if (s_mode == LedMode::kActivity && s_deadline_us == deadline_us) {
        s_mode = LedMode::kIdle;
        s_deadline_us = 0;
        set_inactive();
    }
    portEXIT_CRITICAL(&s_state_mux);
}

void fail_dark_if_mode_is_current(LedMode mode, int64_t deadline_us = 0)
{
    portENTER_CRITICAL(&s_state_mux);
    if (s_mode == mode && (mode != LedMode::kActivity || s_deadline_us == deadline_us)) {
        s_mode = LedMode::kIdle;
        s_deadline_us = 0;
        s_pending_activity = false;
        set_inactive();
    }
    portEXIT_CRITICAL(&s_state_mux);
}

int64_t activity_deadline(int64_t now_us, uint64_t pulse_us)
{
    return now_us > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(pulse_us)
               ? std::numeric_limits<int64_t>::max()
               : now_us + static_cast<int64_t>(pulse_us);
}

void timer_callback(void *)
{
    const int64_t now_us = esp_timer_get_time();
    const uint64_t pulse_us = rf_activity_led_pulse_us(kConfig);
    uint64_t rearm_us = 0;
    LedMode scheduled_mode = LedMode::kIdle;
    int64_t scheduled_deadline_us = 0;
    portENTER_CRITICAL(&s_state_mux);
    if (s_mode == LedMode::kStartup) {
        const RfActivityLedStartupStep step =
            rf_activity_led_startup_step(kConfig, ++s_startup_phase);
        gpio_set_level(static_cast<gpio_num_t>(kConfig.gpio), step.level);
        if (step.complete) {
            s_mode = LedMode::kStartupGap;
        }
        rearm_us = step.duration_us;
    } else if (s_mode == LedMode::kStartupGap) {
        if (s_pending_activity) {
            s_pending_activity = false;
            s_mode = LedMode::kActivity;
            s_deadline_us = activity_deadline(now_us, pulse_us);
            gpio_set_level(static_cast<gpio_num_t>(kConfig.gpio),
                           rf_activity_led_active_level(kConfig));
            rearm_us = pulse_us;
        } else {
            s_mode = LedMode::kIdle;
        }
    } else if (s_mode == LedMode::kActivity) {
        const RfActivityLedDeadlineDecision decision =
            rf_activity_led_deadline_decision(now_us, s_deadline_us);
        if (decision.turn_off) {
            s_mode = LedMode::kIdle;
            s_deadline_us = 0;
            set_inactive();
        } else {
            rearm_us = decision.rearm_us;
        }
    }
    scheduled_mode = s_mode;
    scheduled_deadline_us = s_deadline_us;
    portEXIT_CRITICAL(&s_state_mux);
    if (rearm_us == 0) {
        return;
    }

    const esp_err_t error = arm_timer(rearm_us);
    if (error != ESP_OK) {
        fail_dark_if_mode_is_current(scheduled_mode, scheduled_deadline_us);
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
    const BoardInfo &board = current_board_info();
    const int unavailable_gpios[] = {
        CONFIG_CC1101_SPI_SCLK_GPIO, CONFIG_CC1101_SPI_MISO_GPIO,
        CONFIG_CC1101_SPI_MOSI_GPIO, CONFIG_CC1101_SPI_CS_GPIO,
        CONFIG_CC1101_GDO0_GPIO,     CONFIG_CC1101_GDO2_GPIO,
        board.cc1101.generic_tx,     board.cc1101.generic_rx,
    };
    if (!rf_activity_led_config_is_valid(configured_board_profile(), kConfig,
                                         unavailable_gpios,
                                         std::size(unavailable_gpios)) ||
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

    const RfActivityLedStartupStep startup = rf_activity_led_startup_step(kConfig, 0);
    s_timer = timer;
    s_mode = LedMode::kStartup;
    s_startup_phase = 0;
    s_pending_activity = false;
    s_deadline_us = 0;
    error = gpio_set_level(static_cast<gpio_num_t>(kConfig.gpio), startup.level);
    if (error == ESP_OK) {
        error = esp_timer_start_once(timer, startup.duration_us);
    }
    if (error != ESP_OK) {
        set_inactive();
        s_timer = nullptr;
        s_mode = LedMode::kIdle;
        gpio_reset_pin(static_cast<gpio_num_t>(kConfig.gpio));
        esp_timer_delete(timer);
        return error;
    }
    s_initialized.store(true, std::memory_order_release);
    ESP_LOGI(kTag,
             "RX activity LED GPIO%d active-%s pulse=%lu ms startup=%ux%lu ms gap=%lu ms",
             kConfig.gpio, kConfig.active_high ? "high" : "low",
             static_cast<unsigned long>(kConfig.pulse_ms),
             static_cast<unsigned>(kRfActivityLedStartupPulseCount),
             static_cast<unsigned long>(kConfig.pulse_ms),
             static_cast<unsigned long>(kRfActivityLedStartupGapMs));
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
    const int64_t deadline_us = activity_deadline(now_us, pulse_us);
    portENTER_CRITICAL(&s_state_mux);
    if (s_mode == LedMode::kStartup || s_mode == LedMode::kStartupGap) {
        s_pending_activity = true;
        portEXIT_CRITICAL(&s_state_mux);
        return;
    }
    s_mode = LedMode::kActivity;
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
