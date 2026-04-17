#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "A7670.h"

static const char* TAG = "A7670";

extern "C" void app_main(void)
{
    A7670Modem modem;

    for (int i = 0; i < 5; i++) {
        ESP_LOGI(TAG, "Time elapsed: %d s", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    modem.begin(CONFIG_PHONE_NUMBER_FOR_RESPONSE, "LilyGo A7670 Device Started from esp-idf v6");

    if (modem.httpsPOST(CONFIG_SENSOR_SEND_WEB_SERVER_HTTPS_IMAGE_UPLOAD_URL,
        "{\"esp32-a7670\":1}",
        CONFIG_SENSOR_SEND_WEB_SERVER_HTTPS_IMAGE_UPLOAD_API_KEY)) {
        ESP_LOGI("MAIN", "POST succeeded");
    } else {
        ESP_LOGE("MAIN", "POST failed");
    }

    if (modem.httpsGET(CONFIG_SENSOR_SEND_OTA_URL)) {
        ESP_LOGI("MAIN", "GET succeeded");
    } else {
        ESP_LOGE("MAIN", "GET failed");
    }

    modem.startSMSListener();
    while(true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
