#include "rf_activity_led_policy.hpp"

namespace rfbridge {
namespace {

bool classic_esp32_output_gpio_is_valid(int gpio)
{
    return (gpio >= 0 && gpio <= 19) || (gpio >= 21 && gpio <= 23) ||
           (gpio >= 25 && gpio <= 27) || (gpio >= 32 && gpio <= 33);
}

bool gpio_is_reserved_by_platform(int gpio)
{
    const bool unsupported_strapping_pin = gpio == 0 || gpio == 5 || gpio == 12 || gpio == 15;
    return unsupported_strapping_pin || gpio == 1 || gpio == 3 ||
           (gpio >= 6 && gpio <= 11) || gpio == 16 || gpio == 17;
}

}  // namespace

bool rf_activity_led_config_is_valid(const RfActivityLedConfig &config,
                                     const int *unavailable_gpios,
                                     std::size_t unavailable_gpio_count)
{
    if (!config.enabled) {
        return true;
    }
    if (unavailable_gpio_count != 0 && unavailable_gpios == nullptr) {
        return false;
    }
    if (!classic_esp32_output_gpio_is_valid(config.gpio) ||
        gpio_is_reserved_by_platform(config.gpio) ||
        config.pulse_ms < kRfActivityLedMinimumPulseMs ||
        config.pulse_ms > kRfActivityLedMaximumPulseMs) {
        return false;
    }
    for (std::size_t index = 0; index < unavailable_gpio_count; ++index) {
        if (config.gpio == unavailable_gpios[index]) {
            return false;
        }
    }
    return true;
}

uint8_t rf_activity_led_active_level(const RfActivityLedConfig &config)
{
    return config.active_high ? 1U : 0U;
}

uint8_t rf_activity_led_inactive_level(const RfActivityLedConfig &config)
{
    return config.active_high ? 0U : 1U;
}

uint64_t rf_activity_led_pulse_us(const RfActivityLedConfig &config)
{
    return static_cast<uint64_t>(config.pulse_ms) * 1000U;
}

RfActivityLedDeadlineDecision rf_activity_led_deadline_decision(int64_t now_us,
                                                                int64_t deadline_us)
{
    if (deadline_us <= now_us) {
        return {};
    }
    return {
        .turn_off = false,
        .rearm_us = static_cast<uint64_t>(deadline_us - now_us),
    };
}

}  // namespace rfbridge
