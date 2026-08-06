#include "rf_activity_led_policy.hpp"

namespace rfbridge {
bool rf_activity_led_config_is_valid(const RfActivityLedConfig &config,
                                     const int *unavailable_gpios,
                                     std::size_t unavailable_gpio_count)
{
    return rf_activity_led_config_is_valid(BoardProfile::kEsp32Devkit, config,
                                           unavailable_gpios, unavailable_gpio_count);
}

bool rf_activity_led_config_is_valid(BoardProfile profile,
                                     const RfActivityLedConfig &config,
                                     const int *unavailable_gpios,
                                     std::size_t unavailable_gpio_count)
{
    if (!config.enabled) {
        return true;
    }
    if (unavailable_gpio_count != 0 && unavailable_gpios == nullptr) {
        return false;
    }
    if (!board_activity_led_gpio_is_valid(profile, config.gpio, unavailable_gpios,
                                          unavailable_gpio_count) ||
        config.pulse_ms < kRfActivityLedMinimumPulseMs ||
        config.pulse_ms > kRfActivityLedMaximumPulseMs) {
        return false;
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

RfActivityLedStartupStep rf_activity_led_startup_step(const RfActivityLedConfig &config,
                                                      uint8_t phase)
{
    if (phase >= kRfActivityLedStartupPhaseCount) {
        return {
            .level = rf_activity_led_inactive_level(config),
            .duration_us = static_cast<uint64_t>(kRfActivityLedStartupGapMs) * 1000U,
            .complete = true,
        };
    }
    const bool active = phase % 2U == 0;
    return {
        .level = active ? rf_activity_led_active_level(config)
                        : rf_activity_led_inactive_level(config),
        .duration_us = active ? rf_activity_led_pulse_us(config)
                              : static_cast<uint64_t>(kRfActivityLedStartupGapMs) * 1000U,
        .complete = false,
    };
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
