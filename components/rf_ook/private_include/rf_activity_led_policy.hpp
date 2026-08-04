#pragma once

#include <cstddef>
#include <cstdint>

namespace rfbridge {

constexpr uint32_t kRfActivityLedMinimumPulseMs = 5;
constexpr uint32_t kRfActivityLedMaximumPulseMs = 250;
constexpr uint8_t kRfActivityLedStartupPulseCount = 3;
constexpr uint32_t kRfActivityLedStartupGapMs = 150;
constexpr uint8_t kRfActivityLedStartupPhaseCount =
    kRfActivityLedStartupPulseCount * 2U - 1U;

struct RfActivityLedConfig {
    bool enabled = false;
    int gpio = -1;
    bool active_high = true;
    uint32_t pulse_ms = 25;
};

struct RfActivityLedDeadlineDecision {
    bool turn_off = true;
    uint64_t rearm_us = 0;
};

struct RfActivityLedStartupStep {
    uint8_t level = 0;
    uint64_t duration_us = 0;
    bool complete = true;
};

bool rf_activity_led_config_is_valid(const RfActivityLedConfig &config,
                                     const int *unavailable_gpios,
                                     std::size_t unavailable_gpio_count);
uint8_t rf_activity_led_active_level(const RfActivityLedConfig &config);
uint8_t rf_activity_led_inactive_level(const RfActivityLedConfig &config);
uint64_t rf_activity_led_pulse_us(const RfActivityLedConfig &config);
RfActivityLedStartupStep rf_activity_led_startup_step(const RfActivityLedConfig &config,
                                                      uint8_t phase);
RfActivityLedDeadlineDecision rf_activity_led_deadline_decision(int64_t now_us,
                                                                int64_t deadline_us);

}  // namespace rfbridge
