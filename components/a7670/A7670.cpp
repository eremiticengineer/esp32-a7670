#include "A7670.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <sstream>

// From:
// https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/blob/main/examples/HttpsBuiltlnPost/utilities.h
// converted for esp-idf v6
// Power on/off sequence
// not board specific in utilities.h
#define MODEM_START_WAIT_MS             3000
// TINY_GSM_MODEM_A7670
#define MODEM_POWERON_PULSE_WIDTH_MS      (100)
#define MODEM_POWEROFF_PULSE_WIDTH_MS     (3000)
// LILYGO_T_CALL_A7670_V1_0
#define MODEM_BAUDRATE                      (115200)
#define MODEM_DTR_PIN                       GPIO_NUM_14
#define MODEM_TX_PIN                        GPIO_NUM_26
#define MODEM_RX_PIN                        GPIO_NUM_25
// The modem boot pin needs to follow the startup sequence.
#define BOARD_PWRKEY_PIN                    GPIO_NUM_4
#define BOARD_LED_PIN                       GPIO_NUM_12
#define LED_ON                              HIGH
#define MODEM_RING_PIN                      GPIO_NUM_13
#define MODEM_RESET_PIN                     GPIO_NUM_27
#define MODEM_RESET_LEVEL                   0
#define SerialAT                            Serial1
#define MODEM_GPS_ENABLE_GPIO               (-1)
#define MODEM_GPS_ENABLE_LEVEL              (-1)
#ifndef TINY_GSM_MODEM_A7670
  #define TINY_GSM_MODEM_A7670
#endif
#define PRODUCT_MODEL_NAME                  "LilyGo-T-Call A7670 V1.0"

#define READ_RESPONSE_TIMEOUT 2000

static const char* TAG = "A7670Modem";

static uart_port_t uart_number;
static TaskHandle_t sms_task_handle;

namespace {

struct AtResponse
{
    std::string command;   // e.g. "+CGNSSINFO"
    std::string text;      // e.g. ",,,,,,,,"
    std::string status;    // "OK" or "ERROR"
};

void log_response(std::string command, std::string response) {
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "command:");
    ESP_LOGI(TAG, "%s", command.c_str());
    ESP_LOGI(TAG, "response:");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "=======================================");
}

AtResponse parse_at_response(const std::string& response, const std::string& command) {
    AtResponse result;
    result.command = command;

    // 1. Find the command line (e.g. "+CGNSSINFO:")
    std::string needle = command + ":";

    auto start = response.find(needle);
    if (start != std::string::npos)
    {
        auto colon = response.find(':', start);
        if (colon != std::string::npos)
        {
            colon++; // after ':'

            // skip spaces
            while (colon < response.size() && response[colon] == ' ') {
                colon++;
            }

            auto end = response.find('\n', colon);
            if (end == std::string::npos) {
                end = response.size();
            }

            result.text = response.substr(colon, end - colon);

            // trim CR/LF
            while (!result.text.empty() && (result.text.back() == '\r' || result.text.back() == '\n'))
            {
                result.text.pop_back();
            }
        }
    }

    // 2. Extract final status
    auto ok_position = response.rfind("OK");
    auto err_position = response.rfind("ERROR");

    if (ok_position != std::string::npos && (err_position == std::string::npos || ok_position > err_position))
    {
        result.status = "OK";
    }
    else if (err_position != std::string::npos)
    {
        result.status = "ERROR";
    }
    else
    {
        result.status = "UNKNOWN";
    }

    return result;
}

std::string extract_single_at_line(const std::string &raw, const std::string &prefix)
{
    std::istringstream stream(raw);
    std::string line;

    while (std::getline(stream, line)) {
        // trim CR
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // trim leading spaces
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) {
            line = line.substr(first);
        }

        if (line.rfind(prefix, 0) == 0) { // starts with prefix
            return line;
        }
    }

    return "";
}

std::string extract_http_section(const std::string &raw, const std::string &section_prefix) {
    // Find section start
    size_t start = raw.find(section_prefix);
    if (start == std::string::npos) {
        return "";
    }

    // Skip the +HTTP... line
    size_t line_end = raw.find('\n', start);
    if (line_end == std::string::npos) {
        return "";
    }
    start = line_end + 1;

    // Take everything after that
    std::string section = raw.substr(start);

    // Split into lines
    std::vector<std::string> lines;
    size_t position = 0;
    while (position < section.size()) {
        size_t next = section.find('\n', position);
        std::string line;

        if (next != std::string::npos) {
            line = section.substr(position, next - position);
            position = next + 1;
        }
        else {
            line = section.substr(position);
            position = section.size();
        }

        // trim whitespace
        line.erase(0, line.find_first_not_of("\r\n\t "));
        line.erase(line.find_last_not_of("\r\n\t ") + 1);

        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    // Remove last line (modem-specific: OK or +HTTPREAD: 0)
    if (!lines.empty()) {
        lines.pop_back();
    }

    // Rebuild result
    std::string result;
    for (const auto& length : lines) {
        if (!result.empty()) {
            result += "\n";
        }
        result += length;
    }

    return result;
}

int parse_length(const std::string &response) {
    // resp example: "+HTTPREAD: LEN,36"
    size_t position = response.find(',');
    if (position != std::string::npos) {
        return atoi(response.substr(position + 1).c_str());
    }
    return 0; // fallback
}

// Reads a single line (terminated by \n) from the modem with a timeout
std::string read_line(int timeout_ms) {
    std::string line;
    char c;
    int64_t start = esp_timer_get_time(); // microseconds

    while ((esp_timer_get_time() - start) < timeout_ms * 1000) {
        int len = uart_read_bytes(uart_number, (uint8_t*)&c, 1, pdMS_TO_TICKS(50));
        if (len > 0) {
            if (c == '\n') {
                break; // end of line
            }
            else if (c != '\r') {
                line += c;
            }
        }
        else {
            vTaskDelay(pdMS_TO_TICKS(10)); // small delay
        }
    }

    return line;
}

void write_command(const std::string& command) {
    std::string command_string = command + "\r\n";
    uart_write_bytes(uart_number, command_string.c_str(), command_string.length());
}

std::string read_response(int timeout_ms) {
    std::string response;
    char buffer[128];

    int64_t start = esp_timer_get_time() / 1000;

    while ((esp_timer_get_time() / 1000 - start) < timeout_ms) {
        int len = uart_read_bytes(uart_number, (uint8_t*)buffer, sizeof(buffer)-1, pdMS_TO_TICKS(100));
        if (len > 0) {
            buffer[len] = '\0';
            response += buffer;

            if (response.find("\r\nOK") != std::string::npos || response.find("\r\nERROR") != std::string::npos) {
                break;
            }
        }
    }

    return response;
}

bool check_respond() {
    const int max_retries = 10;
    const int delay_ms = 1000; // 1 second between retries

    for (int j = 0; j < max_retries; j++) {
        write_command("AT"); // simple ping
        std::string response = read_response(1000);
        if (response.find("OK") != std::string::npos) {
            ESP_LOGI(TAG, "Modem responded");

            // Safe configuration
            write_command("ATE0");          // Disable echo
            read_response(500);

            write_command("ATI");           // Get modem info
            read_response(500);

            write_command("AT+CMGF=1");     // SMS text mode
            read_response(500);

            return true;
        }

        ESP_LOGW(TAG, "No response, retry %d/%d", j + 1, max_retries);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    ESP_LOGE(TAG, "Modem did not respond after %d retries", max_retries);
    return false;
}

bool wait_for_network(int timeout_ms) {
    const int poll_interval_ms = 1000; // check every 1s
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        write_command("AT+CEREG?");
        std::string response = read_response(500);

        // look for +CEREG: <n>,<modem_registration_status>
        size_t position = response.find("+CEREG:");
        if (position != std::string::npos) {
            int modem_registration_status = -1;
            int matched_fields = sscanf(response.c_str() + position, "+CEREG: %*d,%d", &modem_registration_status);
            ESP_LOGI("MODEM", "Response: [%s]", response.c_str());
            ESP_LOGI("MODEM", "sscanf matched_fields=%d, modem_registration_status=%d", matched_fields, modem_registration_status);

            if (modem_registration_status == 1 || modem_registration_status == 5) { 
                // 1 = registered home network
                // 5 = registered roaming
                ESP_LOGI("MODEM", "Network registered!");
                return true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        elapsed += poll_interval_ms;
    }

    ESP_LOGW("MODEM", "Network registration timeout");
    return false;
}

bool is_network_connected() {
    write_command("AT+CEREG?"); // Read EPS network registration status

    std::string response = read_response(500);

    // look for +CEREG: <n>,<modem_registration_status>
    size_t position_of_cereg_response = response.find("+CEREG:");
    if (position_of_cereg_response != std::string::npos) {
        int modem_registration_status = -1;
        // AT+CEREG EPS network registration status, response:
        //                                                    +CEREG:0,1 = 0=LTE notifications disabled
        //                                                              read 0, discard
        //                                                              | read 1, store
        //                                                              |  |
        sscanf(response.c_str() + position_of_cereg_response, "+CEREG: %*d,%d", &modem_registration_status);

        // 1 = registered home network, 5 = registered roaming
        if (modem_registration_status == 1 || modem_registration_status == 5) {
            return true;
        }
    }

    return false;
}

bool gprs_connect(const std::string &apn, const std::string &username, const std::string &password, int timeout_ms)
{
    ESP_LOGI(TAG, "Configuring GPRS...");

    std::string response;
    std::string command;

    // Authentication
    if (!username.empty() || !password.empty()) {
        command = "AT+CGAUTH=1,0,\"" + username + "\",\"" + password + "\"";
        write_command(command.c_str());
        response = read_response(2000);
        if (response.find("OK") == std::string::npos) {
            ESP_LOGW(TAG, "Auth command returned error (likely empty password), ignoring...");
            // Continue anyway, like TinyGSM
        }
        else {
            ESP_LOGI(TAG, "Auth set successfully");
        }
    }
    else {
        // If both empty, still send command for compatibility
        command = "AT+CGAUTH=1,0";
        write_command(command);
        command = read_response(2000);
        ESP_LOGI(TAG, "Auth command sent with no credentials");
    }

    // Set PDP context
    command = "AT+CGDCONT=1,\"IP\",\"" + apn + "\",\"0.0.0.0\",0,0";
    write_command(command.c_str());
    response = read_response(2000);
    if (response.find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to set PDP context: %s", response.c_str());
        return false;
    }

    // Activate context
    write_command("AT+CGACT=1,1");
    response = read_response(5000);
    if (response.find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to activate PDP context: %s", response.c_str());
        return false;
    }

    // Open network connection
    write_command("AT+NETOPEN");
    response = read_response(timeout_ms);

    if (response.find("OK") != std::string::npos ||
      response.find("+NETOPEN: 0") != std::string::npos ||
      response.find("Network is already opened") != std::string::npos) {
      ESP_LOGI(TAG, "GPRS connected!");
      return true;
    }

    ESP_LOGE(TAG, "Failed to connect GPRS: %s", response.c_str());
    return false;
}

std::string get_Local_ip() {
    std::string ip = "0.0.0.0"; // default if not found

    write_command("AT+IPADDR");
    std::string response = read_response(1000);

    // The response usually contains: +IPADDR: x.x.x.x
    size_t ipaddr_position = response.find("+IPADDR:");
    if (ipaddr_position != std::string::npos) {
        // skip past "+IPADDR:" and any whitespace
        ip = response.substr(ipaddr_position + 8);
        // remove trailing newline/carriage return
        ip.erase(ip.find_last_not_of("\r\n ") + 1);
    }

    return ip;
}

} // namespace

A7670Modem::A7670Modem() {
  uart_number = static_cast<uart_port_t>(1);
  sms_task_handle = nullptr;
}

A7670Modem::~A7670Modem() {
  if (sms_task_handle) vTaskDelete(sms_task_handle);
}

void A7670Modem::begin_gps() {
    power_on_gps();
}

void A7670Modem::power_on_gps() {
    uart_config_t uart_config = {};
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.rx_flow_ctrl_thresh = 0;
    uart_config.rx_glitch_filt_thresh = 0;

    uart_param_config(uart_number, &uart_config);
    uart_set_pin(uart_number, MODEM_TX_PIN, MODEM_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(uart_number, 2048, 0, 0, nullptr, 0);

    gpio_set_direction(MODEM_DTR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_PWRKEY_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_LED_PIN, GPIO_MODE_OUTPUT);





    // Set LED pin off
    gpio_set_level(BOARD_LED_PIN, 0);

    // Reset modem
    gpio_set_level(MODEM_DTR_PIN, !MODEM_RESET_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(MODEM_DTR_PIN, MODEM_RESET_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(2600));
    gpio_set_level(MODEM_DTR_PIN, !MODEM_RESET_LEVEL);

    // Pull down DTR to ensure the modem is not in sleep state
    gpio_set_level(MODEM_DTR_PIN, 0);

    // Turn on the modem
    gpio_set_level(BOARD_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(MODEM_POWERON_PULSE_WIDTH_MS));
    gpio_set_level(BOARD_PWRKEY_PIN, 0);

    ESP_LOGI(TAG, "%s", PRODUCT_MODEL_NAME);

    // Give modem ~3s to boot
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Check whether it has been started
    bool started = check_respond();
    if (!started) {
      ESP_LOGI(TAG, "modem failed to start");
      return;
    }
    else {
      ESP_LOGI(TAG, "modem started");
    }

    // Set LED pin on
    gpio_set_level(BOARD_LED_PIN, 1);

    std::string modem_name = get_modem_name();
    ESP_LOGI(TAG, "Modem name: %s", modem_name.c_str());

    std::string modem_info = get_modem_info();
    ESP_LOGI(TAG, "Modem info: %s", modem_info.c_str());





    // Power on...
    write_command("AT+CGNSSPWR=1");
    std::string response = read_response(2000);
    log_response("AT+CGNSSPWR=1", response);

    AtResponse at_response = parse_at_response(response, "+CGNSSPWR");
    ESP_LOGI(TAG, "command = %s", at_response.command.c_str());
    ESP_LOGI(TAG, "text = %s", at_response.text.c_str());
    ESP_LOGI(TAG, "status = %s", at_response.status.c_str());
    // delay for the system to start or the baud rate setting will fail
    vTaskDelay(pdMS_TO_TICKS(5000));

    // ...set the baud rate...
    write_command("AT+CGNSSIPR=115200");
    response = read_response(2000);
    log_response("AT+CGNSSIPR=115200", response); // ERROR
    at_response = parse_at_response(response, "+CGNSSIPR");
    ESP_LOGI(TAG, "command = %s", at_response.command.c_str());
    ESP_LOGI(TAG, "text = %s", at_response.text.c_str());
    ESP_LOGI(TAG, "status = %s", at_response.status.c_str());

    // Enable NMEA forwarding
    write_command("AT+CGNSSTST=1");
    log_response("AT+CGNSSTST=1", read_response(2000));
    write_command("AT+CGNSSPORTSWITCH=?");
    log_response("Supported ports", read_response(2000));
    write_command("AT+CGNSSPORTSWITCH?");
    log_response("Current ports", read_response(2000));
    write_command("AT+CGNSSPORTSWITCH=0,1");
    log_response("Set NMEA UART", read_response(2000));
    write_command("AT+CGNSSTST=1");
    log_response("Enable NMEA", read_response(2000));
    
    // ...request location...
    while (1) {
        // +CGNSSINFO: ,,,,,,,,
        // SIM767XX Series_AT Command Manual_V1.01 manual p341
        // [<mode>],
        // [<GPS-SVs>],
        // [<GLONASS-SVs>],
        // [GALILEO-SVs],
        // [BEIDOU-SVs],[<lat>],
        // [<N/S>],
        // [<log>],
        // [<E/W>],
        // [<date>],
        // [<UTC-time>],
        // [<alt>],
        // [<speed>],
        // [<course>],
        // [<PDOP>],
        // [HDOP],
        // [VDOP],
        // [NoSV]

        ESP_LOGI(TAG, "-----------------------------------------------------");
        // +CGNSSPWR: 1,0,1 = GNSS receiver enabled
        // write_command("AT+CGNSSPWR?");
        // log_response("AT+CGNSSPWR?", read_response(2000));

        // write_command("AT+CGNSSTST?");
        // log_response("AT+CGNSSTST?", read_response(2000));

        // +CVAUXS: 1 = Auxiliary voltage output enabled
        // write_command("AT+CVAUXS?");
        // log_response("AT+CVAUXS?", read_response(2000));

        // +CVAUXV: 3000 = Auxiliary voltage configured to 3000 mV (3.0 V)
        // write_command("AT+CVAUXV?");
        // log_response("AT+CVAUXV?", read_response(2000));
        ESP_LOGI(TAG, "-----------------------------------------------------");

        // Log satellite info
        // $GPGSV,1,1,01,20,,,28,0*6C
        // 01 — one GPS satellite visible
        // 20 — satellite PRN 20
        // 28 — signal strength of 28 dB-Hz
        response = read_response(2000);
        if (!response.empty()) {
            std::istringstream stream(response);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.starts_with("$GPGSV")) {
                    ESP_LOGI(TAG, "NMEA: %s", line.c_str());
                }
            }
        }

        write_command("AT+CGNSSINFO");
        response = read_response(2000);
        //log_response("AT+CGNSSINFO", response);
        at_response = parse_at_response(response, "+CGNSSINFO");
        ESP_LOGI(TAG, "command = %s", at_response.command.c_str());
        ESP_LOGI(TAG, "text = %s", at_response.text.c_str());
        ESP_LOGI(TAG, "status = %s", at_response.status.c_str());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // ...power off the gps
    write_command("AT+CGNSSPWR=0");
    response = read_response(2000);
    log_response("AT+CGNSSPWR=1", response);
}

void A7670Modem::begin_modem(const std::string& startup_number, const std::string& startup_message) {
    pending_startup_number = startup_number;
    pending_startup_message = startup_message;

    uart_config_t uart_config;
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.rx_flow_ctrl_thresh = 0;
    uart_config.rx_glitch_filt_thresh = 0;

    uart_param_config(uart_number, &uart_config);
    uart_set_pin(uart_number, MODEM_TX_PIN, MODEM_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(uart_number, 2048, 0, 0, nullptr, 0);

    gpio_set_direction(MODEM_DTR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_PWRKEY_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_LED_PIN, GPIO_MODE_OUTPUT);

    power_on_modem();

    // Send startup SMS
    send_sms(pending_startup_number, pending_startup_message);
}

void A7670Modem::power_on_modem() {
    // Set LED pin off
    gpio_set_level(BOARD_LED_PIN, 0);

    // Reset modem
    gpio_set_level(MODEM_DTR_PIN, !MODEM_RESET_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(MODEM_DTR_PIN, MODEM_RESET_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(2600));
    gpio_set_level(MODEM_DTR_PIN, !MODEM_RESET_LEVEL);

    // Pull down DTR to ensure the modem is not in sleep state
    gpio_set_level(MODEM_DTR_PIN, 0);

    // Turn on the modem
    gpio_set_level(BOARD_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(MODEM_POWERON_PULSE_WIDTH_MS));
    gpio_set_level(BOARD_PWRKEY_PIN, 0);

    ESP_LOGI(TAG, "%s", PRODUCT_MODEL_NAME);

    // Give modem ~3s to boot
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Check whether it has been started
    bool started = check_respond();
    if (!started) {
      ESP_LOGI(TAG, "modem failed to start");
      return;
    }
    else {
      ESP_LOGI(TAG, "modem started");
    }

    // Set LED pin on
    gpio_set_level(BOARD_LED_PIN, 1);

    std::string modem_name = get_modem_name();
    ESP_LOGI(TAG, "Modem name: %s", modem_name.c_str());

    std::string modem_info = get_modem_info();
    ESP_LOGI(TAG, "Modem info: %s", modem_info.c_str());

    std::string sim_ccid = get_sim_ccid(10000);
    ESP_LOGI(TAG, "SIM CCID: %s", sim_ccid.c_str());

    std::string imei = get_imei();
    ESP_LOGI(TAG, "IMEI: %s", imei.c_str());

    int signal_quality = 99;
    while (signal_quality == 99)
    {
        signal_quality = get_signal_quality();
        ESP_LOGI(TAG, "Signal quality (0-31): %d", signal_quality);
        vTaskDelay(pdMS_TO_TICKS(5000)); 
    }

    // Wait for the network
    ESP_LOGI(TAG, "Waiting for network...");
    if (!wait_for_network(1200000)) {
      ESP_LOGI(TAG, " fail");
      vTaskDelay(pdMS_TO_TICKS(5000)); 
      return;
    }
    ESP_LOGI(TAG, " success");
    if (is_network_connected()) {
      ESP_LOGI(TAG, "Network connected");
    }

    std::string network_operator = get_operator();
    ESP_LOGI(TAG, "Operator: %s", network_operator.c_str());

    // Connect to GPRS
    ESP_LOGI(TAG, "Connecting to GPRS...");
    std::string apn = CONFIG_LTE_NETWORK_APN;
    std::string username = CONFIG_LTE_NETWORK_USER;
    std::string password = CONFIG_LTE_NETWORK_PASSWORD;
    if (gprs_connect(apn, username, password, 10000)) {
        ESP_LOGI(TAG, "GPRS connected!");
        ESP_LOGI(TAG, "Local IP: ");
        ESP_LOGI(TAG, "%s", get_Local_ip().c_str());
        ESP_LOGI(TAG, "waiting for services...");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    else {
        ESP_LOGI(TAG, "GPRS connection failed");
        return;
    }

    write_command("AT+CREG?");
    std::string response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "AT+CREG?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "=========================================================");

    write_command("AT+CGREG?");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "AT+CGREG?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "=========================================================");

    // Turn off echo
    write_command("ATE0");
    read_response(READ_RESPONSE_TIMEOUT);

    write_command("AT+CPSI?");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "*********************************************************");
    ESP_LOGI(TAG, "AT+CPSI?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "*********************************************************");

    write_command("AT+CEREG?");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "*********************************************************");
    ESP_LOGI(TAG, "AT+CEREG?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "*********************************************************");

    write_command("AT+CIREG?");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "*********************************************************");
    ESP_LOGI(TAG, "AT+CIREG?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "*********************************************************");

    write_command("AT+CLIP=1");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "*********************************************************");
    ESP_LOGI(TAG, "AT+CLIP=1");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "*********************************************************");

    write_command("AT+CRC=1");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "*********************************************************");
    ESP_LOGI(TAG, "AT+CRC=1");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "*********************************************************");



    // Enable verbose error reporting
    write_command("AT+CMEE=2");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CMEE=2");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    write_command("AT+CIREG?");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CIREG?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    write_command("AT+CPSI?");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CPSI?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    char atd_command[32];
    snprintf(atd_command, sizeof(atd_command), "ATD%s;", CONFIG_PHONE_NUMBER_FOR_RESPONSE);
    write_command(atd_command);
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "%s", atd_command);
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    vTaskDelay(pdMS_TO_TICKS(3000));

    write_command("AT+CEER");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CEER");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    write_command("AT+CLCC");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CLCC");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    write_command("AT+CPSI?");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CPSI?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    write_command("AT+CLCC");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CLCC");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    write_command("AT+CPSI?");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CPSI?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    vTaskDelay(pdMS_TO_TICKS(3000));

    write_command("AT+CLCC");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CLCC");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    write_command("AT+CPSI?");
    response = read_response(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ESP_LOGI(TAG, "AT+CPSI?");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    write_command("ATH");
}

bool A7670Modem::send_sms(const std::string& number, const std::string& message) {
    // SMS text mode
    write_command("AT+CMGF=1");
    read_response(READ_RESPONSE_TIMEOUT);

    // Tell the modem which character encoding to use for text messages
    // "GSM" means the GSM 7-bit default alphabet
    write_command("AT+CSCS=\"GSM\"");
    read_response(READ_RESPONSE_TIMEOUT);

    write_command("AT+CMGS=\"" + number + "\"");
    read_response(READ_RESPONSE_TIMEOUT);

    uart_write_bytes(uart_number, message.c_str(), message.length());
    uint8_t ctrl_z = 26;
    uart_write_bytes(uart_number, (char*)&ctrl_z, 1);

    std::string response = read_response(5000);
    bool ok = response.find("OK") != std::string::npos;
    ESP_LOGI(TAG, "SMS send %s", ok ? "success" : "failed");
    return ok;
}

void A7670Modem::start_sms_listener() {
    if (!sms_task_handle) {
        xTaskCreate(sms_task_wrapper, "sms_task", 4096, this, 5, &sms_task_handle);
    }
}

// FreeRTOS tasks need static functions, so we wrap the member function
void A7670Modem::sms_task_wrapper(void* param) {
    A7670Modem* modem = static_cast<A7670Modem*>(param);
    modem->sms_task();
}

void A7670Modem::sms_task() {
    char buffer[512];
    std::string line;

    bool receiving_message = false;
    std::string sender_number;

    while (true) {
        int length = uart_read_bytes(uart_number, reinterpret_cast<uint8_t*>(buffer), sizeof(buffer) - 1, pdMS_TO_TICKS(500));

        if (length <= 0) {
            continue;
        }

        buffer[length] = 0;
        line += buffer;

        size_t newline_position;

        while ((newline_position = line.find('\n')) != std::string::npos) {
            std::string current_line = trim(line.substr(0, newline_position));

            line.erase(0, newline_position + 1);

            if (current_line.empty()) {
                continue;
            }

            ESP_LOGI(TAG, "URC: %s", current_line.c_str());

            // -------------------------------------------------
            // Incoming SMS header
            // -------------------------------------------------
            if (current_line.starts_with("+CMT: ")) {
                size_t first_quote = current_line.find('"');
                size_t second_quote = current_line.find('"', first_quote + 1);

                if (first_quote != std::string::npos && second_quote != std::string::npos) {
                    sender_number = current_line.substr(first_quote + 1, second_quote - first_quote - 1);
                    receiving_message = true;
                }
            }

            // -------------------------------------------------
            // Incoming SMS body
            // -------------------------------------------------
            else if (receiving_message) {
                std::string receivedMessage = trim(current_line);
                receiving_message = false;
                handle_incoming_sms(sender_number, receivedMessage);
            }

            // -------------------------------------------------
            // Incoming call
            // -------------------------------------------------
            else if (current_line == "RING" || current_line.starts_with("+CRING:")) {
                ESP_LOGI(TAG, "Incoming call");

                write_command("AT+CLCC");
                std::string response = read_response(READ_RESPONSE_TIMEOUT);
                ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                ESP_LOGI(TAG, "Incoming AT+CLCC");
                ESP_LOGI(TAG, "%s", response.c_str());
                ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

                handle_incoming_call();
            }

            // -------------------------------------------------
            // Caller ID
            // -------------------------------------------------
            else if (current_line.starts_with("+CLIP:")) {
                ESP_LOGI(TAG, "Caller ID: %s", current_line.c_str());
            }

            // -------------------------------------------------
            // Call started
            // -------------------------------------------------
            else if (current_line == "VOICE CALL: BEGIN") {
                ESP_LOGI(TAG, "Voice call connected");

                write_command("AT+CLCC");
                std::string response = read_response(READ_RESPONSE_TIMEOUT);
                ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                ESP_LOGI(TAG, "AT+CLCC");
                ESP_LOGI(TAG, "%s", response.c_str());
                ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

                vTaskDelay(pdMS_TO_TICKS(100));

                write_command("AT+CPSI?");
                response = read_response(READ_RESPONSE_TIMEOUT);
                ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                ESP_LOGI(TAG, "AT+CPSI?");
                ESP_LOGI(TAG, "%s", response.c_str());
                ESP_LOGI(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
            }

            // -------------------------------------------------
            // Call ended
            // -------------------------------------------------
            else if (current_line == "VOICE CALL: END" || current_line == "NO CARRIER") {
                ESP_LOGI(TAG, "Voice call ended");
            }
        }
    }
}

void A7670Modem::handle_incoming_call() {
    ESP_LOGI(TAG, "Answering incoming call");
    write_command("ATA");
}

void A7670Modem::handle_incoming_sms(const std::string &from_number, const std::string &message)
{
    ESP_LOGI(TAG, "SMS from number: %s message: %s", from_number.c_str(), message.c_str());

    // If message contains "status", reply
    if (message.find("status") != std::string::npos) {
        ESP_LOGI(TAG, "'status' command received, sending reply");

        std::string reply = "--- Device Status ---\n";
        reply += "Status: Online\n";
        reply += "Operator: " + get_operator() + "\n";
        reply += "Signal: " + std::to_string(get_signal_quality()) + "\n";
        reply += "Time: " + get_time() + "\n";

        send_sms(from_number, reply);
    }
}

std::string A7670Modem::get_modem_name() {
    write_command("ATI");
    std::string response = read_response(READ_RESPONSE_TIMEOUT);

    size_t position_of_model = response.find("Model:");
    if (position_of_model != std::string::npos) {
        size_t end = response.find("\n", position_of_model);
        return trim(response.substr(position_of_model + 6, end - (position_of_model + 6)));
    }

    return "Unknown";
}

std::string A7670Modem::get_modem_info() {
    write_command("ATI");
    std::string response = read_response(READ_RESPONSE_TIMEOUT);

    std::string modem_info;

    size_t position_of_symbol;

    if ((position_of_symbol = response.find("Manufacturer:")) != std::string::npos) {
        size_t end = response.find("\n", position_of_symbol);
        modem_info += trim(response.substr(position_of_symbol, end - position_of_symbol)) + " ";
    }

    if ((position_of_symbol = response.find("Model:")) != std::string::npos) {
        size_t end = response.find("\n", position_of_symbol);
        modem_info += trim(response.substr(position_of_symbol, end - position_of_symbol)) + " ";
    }

    if ((position_of_symbol = response.find("Revision:")) != std::string::npos) {
        size_t end = response.find("\n", position_of_symbol);
        modem_info += trim(response.substr(position_of_symbol, end - position_of_symbol));
    }

    return trim(modem_info);
}

std::string A7670Modem::get_sim_ccid(int timeout_ms) {
    std::string sim_ccid = "N/A"; // timed out waiting for SIM

    int elapsed = 0;
    const int interval = 500; // ms

    while (elapsed < timeout_ms) {
        write_command("AT+CPIN?");
        std::string response = read_response(1000);

        if (response.find("READY") != std::string::npos) {
            // SIM is ready, now get CCID
            write_command("AT+CICCID");
            response = read_response(1000);

            // Extract digits only
            size_t start = response.find_first_of("0123456789");
            if (start != std::string::npos) {
                size_t end = response.find_first_not_of("0123456789", start);
                sim_ccid = response.substr(start, end - start);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(interval));
        elapsed += interval;
    }

    return sim_ccid;
}

std::string A7670Modem::get_imei() {
    std::string imei = "N/A";

    write_command("AT+GSN");
    std::string response = read_response(READ_RESPONSE_TIMEOUT);

    // IMEI is usually just a number line before OK
    size_t start = response.find_first_of("0123456789");
    if (start != std::string::npos) {
        size_t end = response.find("\n", start);
        imei = trim(response.substr(start, end - start));
    }

    return imei;
}

int A7670Modem::get_signal_quality() {
    int signal_quality = -1; // invalid

    write_command("AT+CSQ");
    std::string response = read_response(READ_RESPONSE_TIMEOUT);

    // Example: +CSQ: 12,99
    size_t position_of_csq = response.find("+CSQ:");
    if (position_of_csq != std::string::npos) {
        int rssi = -1;
        sscanf(response.c_str() + position_of_csq, "+CSQ: %d", &rssi);
        signal_quality = rssi;
    }

    return signal_quality;
}

std::string A7670Modem::get_operator() {
    std::string operator_name = "N/A";

    write_command("AT+COPS?");
    std::string response = read_response(READ_RESPONSE_TIMEOUT);

    // Example: +COPS: 0,2,"23410",7
    size_t position_of_cops = response.find("+COPS:");
    if (position_of_cops != std::string::npos) {
        size_t first_quote = response.find('"', position_of_cops);
        size_t second_quote = response.find('"', first_quote + 1);

        if (first_quote != std::string::npos && second_quote != std::string::npos) {
            operator_name = response.substr(first_quote + 1, second_quote - first_quote - 1);
        }
    }

    return operator_name;
}

std::string A7670Modem::get_time() {
    // Placeholder for actual time; can be expanded with AT+CCLK or NITZ parsing
    return "Not available";
}

std::string A7670Modem::trim(const std::string& string_to_be_trimmed) {
    size_t start = string_to_be_trimmed.find_first_not_of("\r\n ");
    size_t end = string_to_be_trimmed.find_last_not_of("\r\n ");
    return (start == std::string::npos) ? "" : string_to_be_trimmed.substr(start, end - start + 1);
}

bool A7670Modem::https_post(const std::string &url, const std::string &json_data, const std::string &api_key)
{
    ESP_LOGI(TAG, "Starting HTTPS POST...");

    write_command("AT+QNWINFO");
    std::string raw_response = read_response(2000);

    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "QNWINFO:");
    ESP_LOGI(TAG, "%s", raw_response.c_str());
    ESP_LOGI(TAG, "=======================================");

    std::string qnwinfo = extract_single_at_line(raw_response, "+QNWINFO:");
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "QNWINFO:");
    ESP_LOGI(TAG, "%s", qnwinfo.c_str());
    ESP_LOGI(TAG, "=======================================");

    // Ensure PDP context is active
    write_command("AT+CGACT=1,1");
    if (read_response(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to activate PDP context");
        return false;
    }

    // Close any previous HTTP session
    write_command("AT+HTTPTERM");
    read_response(2000); // ignore ERROR if no previous session

    // Init HTTP service
    write_command("AT+HTTPINIT");
    if (read_response(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "HTTPINIT failed");
        return false;
    }

    // Set SSL/TLS version (TLS 1.2)
    write_command("AT+CSSLCFG=\"sslversion\",0,4");
    read_response(2000);

    // Enable SNI
    write_command("AT+CSSLCFG=\"enableSNI\",0,1");
    read_response(2000);

    // Set URL
    write_command(("AT+HTTPPARA=\"URL\",\"" + url + "\"").c_str());
    if (read_response(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to set URL");
        return false;
    }

    // Set user-agent
    write_command("AT+HTTPPARA=\"USERDATA\",\"User-Agent: TinyGSM/ESP-IDF\"");
    read_response(2000);

    // Set custom API key header
    write_command(("AT+HTTPPARA=\"USERDATA\",\"X-API-KEY: " + api_key + "\"").c_str());
    read_response(2000);

    // Set data to send
    char command[64];
    snprintf(command, sizeof(command), "AT+HTTPDATA=%d,10000", (int)json_data.length());
    write_command(command);

    if (read_response(2000).find("DOWNLOAD") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to enter HTTPDATA mode");
        return false;
    }

    // Send JSON payload
    write_command(json_data.c_str());
    if (read_response(5000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to send HTTP data");
        return false;
    }

    // Send POST
    write_command("AT+HTTPACTION=1");

    // Wait for +HTTPACTION
    std::string response;
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < 15000000) {
        std::string line = read_line(500);
        if (!line.empty()) {
            response += line + "\n";
            if (line.find("+HTTPACTION: 1,") != std::string::npos) break;
        }
    }
    if (response.find("+HTTPACTION: 1,200") == std::string::npos) {
        ESP_LOGE(TAG, "HTTPACTION failed: %s", response.c_str());
        return false;
    }

    // HTTP HEAD
    write_command("AT+HTTPHEAD");
    raw_response = read_response(2000);
    ESP_LOGI(TAG, "HEADERS START:");
    ESP_LOGI(TAG, "%s", raw_response.c_str());
    ESP_LOGI(TAG, "HEADERS STOP");

    // HTTP Headers
    std::string http_headers = extract_http_section(raw_response, "+HTTPHEAD:");
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "HTTPS HEADERS:");
    ESP_LOGI(TAG, "%s", http_headers.c_str());
    ESP_LOGI(TAG, "=======================================");

    // Query length
    write_command("AT+HTTPREAD?");
    std::string header = read_response(2000); // +HTTPREAD: <len>
    int length = parse_length(header); // extract the available length

    // 1. Send HTTPREAD
    snprintf(command, sizeof(command), "AT+HTTPREAD=0,%d", length);
    write_command(command);

    raw_response = read_response(2000);

    ESP_LOGI(TAG, "HTTPREAD RESPONSE START:");
    ESP_LOGI(TAG, "%s", raw_response.c_str());
    ESP_LOGI(TAG, "HTTPREAD RESPONSE STOP");

    std::string response_body = extract_http_section(raw_response, "+HTTPREAD:");

    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "HTTPS RESPONSE:");
    ESP_LOGI(TAG, "%s", response_body.c_str());
    ESP_LOGI(TAG, "=======================================");

    write_command("AT+HTTPTERM"); read_response(2000);

    return true;
}

bool A7670Modem::https_get(const std::string &url) {
    // Ensure PDP context is active
    write_command("AT+CGACT=1,1");
    if (read_response(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to activate PDP context");
        return false;
    }

    // Close any previous HTTP session
    write_command("AT+HTTPTERM");
    read_response(2000); // ignore ERROR if no previous session

    // Init HTTP service
    write_command("AT+HTTPINIT");
    if (read_response(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "HTTPINIT failed");
        return false;
    }

    // Set SSL/TLS version (TLS 1.2)
    write_command("AT+CSSLCFG=\"sslversion\",0,4");
    read_response(2000);

    // Enable SNI
    write_command("AT+CSSLCFG=\"enableSNI\",0,1");
    read_response(2000);

    // Set the URL
    std::string set_url_command = "AT+HTTPPARA=\"URL\",\"" + url + "\"";
    write_command(set_url_command);
    if (read_response(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to set GET URL");
        return false;
    }

    // Set user-agent
    write_command("AT+HTTPPARA=\"USERDATA\",\"User-Agent: TinyGSM/ESP-IDF\"");
    read_response(2000);
    write_command("AT+HTTPACTION=0");

    // Wait for +HTTPACTION
    std::string response;
    size_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < 15000000) {
        std::string line = read_line(500);
        if (!line.empty()) {
            response += line + "\n";
            if (line.find("+HTTPACTION: 0,") != std::string::npos) break;
        }
    }
    if (response.find("+HTTPACTION: 0,200") == std::string::npos) {
        ESP_LOGE(TAG, "HTTPACTION GET failed: %s", response.c_str());
        return false;
    }

    // Query length
    write_command("AT+HTTPREAD?");
    std::string header = read_response(2000); // +HTTPREAD: <len>
    int length = parse_length(header); // extract the available length

    // 1. Send HTTPREAD
    char command[64];
    snprintf(command, sizeof(command), "AT+HTTPREAD=0,%d", length);
    write_command(command);
    
    response = read_response(2000);

    ESP_LOGI(TAG, "GET RESPONSE START");
    ESP_LOGI(TAG, "%s", response.c_str());
    ESP_LOGI(TAG, "GET RESPONE STOP");
    std::string response_body = extract_http_section(response, "+HTTPREAD:");
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "HTTPS RESPONSE:");
    ESP_LOGI(TAG, "%s", response_body.c_str());
    ESP_LOGI(TAG, "=======================================");

    write_command("AT+HTTPTERM"); read_response(2000);

    return true;
}
