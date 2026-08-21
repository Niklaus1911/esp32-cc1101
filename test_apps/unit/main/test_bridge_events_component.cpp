#include <cstring>
#include <type_traits>

#include "bridge_events.hpp"
#include "unity.h"

static_assert(std::is_trivially_copyable_v<rfbridge::BridgeEvent>);
static_assert(sizeof(rfbridge::BridgeEvent) <= 672U);

TEST_CASE("bridge event tagged payloads remain copyable", "[bridge_events]")
{
    rfbridge::BridgeEvent rf_event{};
    rf_event.type = rfbridge::BridgeEventType::kRx;
    rf_event.payload.rf.frame.encoding = rfbridge::RfEncoding::kDecoded;
    rf_event.payload.rf.frame.decoded.code = 0xA88142;
    std::strcpy(rf_event.payload.rf.name, "gate");
    const rfbridge::BridgeEvent rf_copy = rf_event;
    TEST_ASSERT_EQUAL_STRING("gate", rf_copy.payload.rf.name);
    TEST_ASSERT_EQUAL_UINT64(0xA88142, rf_copy.payload.rf.frame.decoded.code);

    rfbridge::RfAutomationEvent automation{};
    automation.type = rfbridge::RfAutomationEventType::kActionCompleted;
    automation.action_id = 42;
    rfbridge::BridgeEvent automation_event{};
    automation_event.type = rfbridge::BridgeEventType::kAutomation;
    automation_event.payload.set_automation(automation);
    const rfbridge::BridgeEvent automation_copy = automation_event;
    TEST_ASSERT_EQUAL_UINT32(42, automation_copy.payload.automation.action_id);

    rfbridge::RfAutomationConfigEvent automation_config{};
    automation_config.change = rfbridge::RfAutomationConfigChange::kRuleAdded;
    automation_config.configuration_revision = 9;
    std::strcpy(automation_config.trigger_name, "gate");
    std::strcpy(automation_config.target_name, "light");
    rfbridge::BridgeEvent automation_config_event{};
    automation_config_event.type = rfbridge::BridgeEventType::kAutomationConfig;
    automation_config_event.payload.set_automation_config(automation_config);
    const rfbridge::BridgeEvent automation_config_copy = automation_config_event;
    TEST_ASSERT_EQUAL_UINT32(9,
                             automation_config_copy.payload.automation_config.configuration_revision);
    TEST_ASSERT_EQUAL_STRING("gate",
                             automation_config_copy.payload.automation_config.trigger_name);
    TEST_ASSERT_EQUAL_STRING("light",
                             automation_config_copy.payload.automation_config.target_name);

    rfbridge::NetworkWifiEvent network{};
    network.type = rfbridge::NetworkWifiEventType::kConnected;
    network.ip = 0x1101A8C0;
    rfbridge::BridgeEvent network_event{};
    network_event.type = rfbridge::BridgeEventType::kNetwork;
    network_event.payload.set_network(network);
    const rfbridge::BridgeEvent network_copy = network_event;
    TEST_ASSERT_EQUAL_HEX32(0x1101A8C0, network_copy.payload.network.ip);

    rfbridge::OtaUpdateEvent ota{};
    ota.type = rfbridge::OtaUpdateEventType::kProgress;
    ota.bytes_received = 4096;
    rfbridge::BridgeEvent ota_event{};
    ota_event.type = rfbridge::BridgeEventType::kOta;
    ota_event.payload.set_ota(ota);
    const rfbridge::BridgeEvent ota_copy = ota_event;
    TEST_ASSERT_EQUAL_UINT32(4096, ota_copy.payload.ota.bytes_received);
}
