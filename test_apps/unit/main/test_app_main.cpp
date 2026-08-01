#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

namespace {

void unity_task(void *)
{
    unity_run_menu();
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void)
{
    if (xTaskCreate(unity_task, "unity_menu", 12288, nullptr, 5, nullptr) != pdPASS) {
        std::abort();
    }
}
