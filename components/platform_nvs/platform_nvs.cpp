#include "platform_nvs.hpp"

#include <atomic>

#include "nvs_flash.h"

namespace rfbridge {
namespace {

std::atomic<bool> s_initialization_started{false};
std::atomic<esp_err_t> s_initialization_error{ESP_ERR_INVALID_STATE};

}  // namespace

esp_err_t initialize_platform_nvs()
{
    bool expected = false;
    if (!s_initialization_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return s_initialization_error.load(std::memory_order_acquire);
    }

    const esp_err_t error = nvs_flash_init();
    s_initialization_error.store(error, std::memory_order_release);
    return error;
}

bool platform_nvs_is_available()
{
    return s_initialization_error.load(std::memory_order_acquire) == ESP_OK;
}

esp_err_t platform_nvs_initialization_error()
{
    return s_initialization_error.load(std::memory_order_acquire);
}

}  // namespace rfbridge
