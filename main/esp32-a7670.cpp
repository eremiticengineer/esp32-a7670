#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "sdkconfig.h"

#include <optional>

#include "A7670.h"

static const char* TAG = "A7670";

void enter_deep_sleep() {
    ESP_LOGI(TAG, "Entering deep sleep for 10 minutes");
    esp_sleep_enable_timer_wakeup(10ULL * 60ULL * 1000000ULL);
    esp_deep_sleep_start();
}

extern "C" void app_main()
{
    A7670Modem modem;

    modem.init_hardware();

    if (modem.power_on_modem())
    {
        modem.send_sms(CONFIG_PHONE_NUMBER_FOR_RESPONSE, "LilyGo A7670 Device Started from esp-idf v6");
        
        // GPS phase
        std::optional<A7670Modem::GpsFix> fix;

        if (modem.power_on_gps()) {
            fix = modem.get_gps_fix(100 * 60 * 1000); // 180000 3mins
        }

        // modem.power_off_gps();

        // SMS phase
        // if (fix.has_value()) {
        //     if (modem.connect_network(120000)) {
        //         std::string message = modem.format_location_sms(*fix);
        //         modem.send_sms(CONFIG_PHONE_NUMBER_FOR_RESPONSE, message);
        //     }
        // }

        // Complete modem shutdown
        modem.power_off_modem();
    }

    while(true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // modem.deinit_hardware();

    // Ten-minute sleep
    // esp_sleep_enable_timer_wakeup(600ULL * 1000000ULL);
    // esp_deep_sleep_start();
}
