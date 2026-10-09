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
#include <algorithm>
#include <sstream>
#include <array>
#include <cmath>
#include <string_view>

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

bool gprs_connect(const std::string &apn, const std::string &username, const std::string &password, int timeout_ms) {
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

std::string trim(const std::string& string_to_be_trimmed) {
    size_t start = string_to_be_trimmed.find_first_not_of("\r\n ");
    size_t end = string_to_be_trimmed.find_last_not_of("\r\n ");
    return (start == std::string::npos) ? "" : string_to_be_trimmed.substr(start, end - start + 1);
}

} // namespace

A7670Modem::A7670Modem() {
  uart_number = static_cast<uart_port_t>(1);
  sms_task_handle = nullptr;
}

A7670Modem::~A7670Modem() {
  if (sms_task_handle) vTaskDelete(sms_task_handle);
}

bool A7670Modem::init_hardware() {
    uart_config_t config = {};

    config.baud_rate = 115200;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(uart_number, &config));

    ESP_ERROR_CHECK(uart_set_pin(uart_number, MODEM_TX_PIN, MODEM_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(uart_driver_install(uart_number, 4096, 0, 0, nullptr, 0));

    ESP_ERROR_CHECK(gpio_set_direction(MODEM_DTR_PIN, GPIO_MODE_OUTPUT));

    ESP_ERROR_CHECK(gpio_set_direction(BOARD_PWRKEY_PIN, GPIO_MODE_OUTPUT));

    ESP_ERROR_CHECK(gpio_set_direction(BOARD_LED_PIN, GPIO_MODE_OUTPUT));

    gpio_set_level(MODEM_DTR_PIN, 0);
    gpio_set_level(BOARD_PWRKEY_PIN, 0);
    gpio_set_level(BOARD_LED_PIN, 0);

    return true;
}

bool A7670Modem::power_on_hardware() {
    ESP_LOGI(TAG, "Powering on modem");

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

    // Turn on the modem using the T-Call PWRKEY sequence
    gpio_set_level(BOARD_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(MODEM_POWERON_PULSE_WIDTH_MS));
    gpio_set_level(BOARD_PWRKEY_PIN, 0);

    ESP_LOGI(TAG, "%s", PRODUCT_MODEL_NAME);

    // Give modem ~3s to boot
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Allow time for UART to become available
    bool modem_started = false;
    for (int c = 0; c < 30; c++)
    {
        if (check_respond()) {
            ESP_LOGI(TAG, "modem started");
            gpio_set_level(BOARD_LED_PIN, 1);

            std::string modem_name = get_modem_name();
            ESP_LOGI(TAG, "Modem name: %s", modem_name.c_str());

            std::string modem_info = get_modem_info();
            ESP_LOGI(TAG, "Modem info: %s", modem_info.c_str());

            std::string sim_ccid = get_sim_ccid(10000);
            ESP_LOGI(TAG, "SIM CCID: %s", sim_ccid.c_str());

            std::string imei = get_imei();
            ESP_LOGI(TAG, "IMEI: %s", imei.c_str());

            modem_started = true;

            break;
        }
        else {
            ESP_LOGI(TAG, "waiting for modem to start...");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return modem_started;
}

bool A7670Modem::connect_to_network() {
    bool obtained_signal = false;
    bool connected_to_network = false;

    // Wait for a signal
    int signal_quality = 99;
    for (int c = 0; c < 30; c++) {
        signal_quality = get_signal_quality();
        ESP_LOGI(TAG, "Signal quality (0-31): %d", signal_quality);
        if (signal_quality != 99) {
            obtained_signal = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (obtained_signal) {
        ESP_LOGI(TAG, "Waiting for network...");

        if (wait_for_network(1200000)) {
            ESP_LOGI(TAG, " success");
            if (is_network_connected()) {
                ESP_LOGI(TAG, "Network connected");
                std::string network_operator = get_operator();
                ESP_LOGI(TAG, "Operator: %s", network_operator.c_str());
                connected_to_network = true;
            }
        }
        else {
            ESP_LOGI(TAG, " failed to connect to network");
            vTaskDelay(pdMS_TO_TICKS(5000)); 
        }
    } // if (obtained_signal) {

    return connected_to_network;
}

bool A7670Modem::connect_to_data_service() {
    bool connected_to_data_service = false;

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
        connected_to_data_service = true;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    else {
        ESP_LOGI(TAG, "GPRS connection failed");
    }

    return connected_to_data_service;
}

bool A7670Modem::power_on_modem() {
    bool modem_started = false;

    if (power_on_hardware()) {
        if (connect_to_network()) {
            if (connect_to_data_service()) {
                modem_started = true;
            } // connect_to_data_service
            else {
                ESP_LOGE(TAG, "failed to connect to data service");    
            }
        } // connect_to_network
        else {
            ESP_LOGE(TAG, "failed to connect to network");
        }

    } // power_on_hardware
    else {
        ESP_LOGE(TAG, "failed to power on hardware");
    }

    return modem_started;
}

bool A7670Modem::power_on_gps() {
    bool gnss_enabled = false;

    ESP_LOGI(TAG, "Starting GNSS");

    write_command("AT+CGNSSPWR=1");
    std::string response = read_response(10000);
    log_response("AT+CGNSSPWR=1", response);

    AtResponse at_response = parse_at_response(response, "+CGNSSPWR");
    if (at_response.status == "OK") {
        // delay for the system to start or the baud rate setting will fail
        vTaskDelay(pdMS_TO_TICKS(5000));

        // Verify that GNSS is powered on
        write_command("AT+CGNSSPWR?");
        response = read_response(2000);
        at_response = parse_at_response(response, "+CGNSSPWR");
        if (at_response.status != "OK" || at_response.text.empty() || at_response.text[0] != '1') {
            ESP_LOGE(TAG, "GNSS is not enabled");
        }
        else {
            // ...set the baud rate...
            write_command("AT+CGNSSIPR=115200");
            response = read_response(2000);
            log_response("AT+CGNSSIPR=115200", response); // ERROR
            at_response = parse_at_response(response, "+CGNSSIPR");
            ESP_LOGI(TAG, "command = %s", at_response.command.c_str());
            ESP_LOGI(TAG, "text = %s", at_response.text.c_str());
            ESP_LOGI(TAG, "status = %s", at_response.status.c_str());

            // Enable NMEA forwarding
            // write_command("AT+CGNSSTST=1");
            // log_response("AT+CGNSSTST=1", read_response(2000));
            // write_command("AT+CGNSSPORTSWITCH=?");
            // log_response("Supported ports", read_response(2000));
            // write_command("AT+CGNSSPORTSWITCH?");
            // log_response("Current ports", read_response(2000));
            // write_command("AT+CGNSSPORTSWITCH=0,1");
            // log_response("Set NMEA UART", read_response(2000));
            // write_command("AT+CGNSSTST=1");
            // log_response("Enable NMEA", read_response(2000));

            gnss_enabled = true;
        }
    }
    else {
        ESP_LOGE(TAG, "Failed to enable GNSS");
    }

    return gnss_enabled;
}

bool A7670Modem::power_off_gps() {
    ESP_LOGI(TAG, "Stopping GNSS");

    // Stop NMEA streaming if enabled.
    write_command("AT+CGNSSTST=0");
    read_response(2000);

    write_command("AT+CGNSSPWR=0");
    std::string response = read_response(10000);

    return response.find("OK") != std::string::npos;
}

std::optional<A7670Modem::GpsFix> A7670Modem::get_gps_fix(uint32_t timeout_ms) {
    uint32_t start = pdTICKS_TO_MS(xTaskGetTickCount());

    while (pdTICKS_TO_MS(xTaskGetTickCount()) - start < timeout_ms) {
        write_command("AT+CGNSSINFO");
        std::string response = read_response(2000);

        ESP_LOGI(TAG, "%s", response.c_str());

        // Parse the 18 comma-separated GNSS fields
        std::optional<A7670Modem::GpsFix> fix = parse_gnss_fix(response);

        // Return a GpsFix only when the fix is valid
        if (fix.has_value()) {
            ESP_LOGI(TAG, "GPS fix obtained");
            return fix;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    ESP_LOGW(TAG, "GPS acquisition timed out");

    return std::nullopt;
}

std::optional<A7670Modem::GpsFix> A7670Modem::parse_gnss_fix(const std::string& response) {
    constexpr std::string_view prefix = "+CGNSSINFO:";

    // Find the GNSS response, ignoring AT echoes and URCs.
    size_t start = response.find(prefix);

    if (start == std::string::npos) {
        return std::nullopt;
    }

    start += prefix.length();

    size_t end = response.find_first_of("\r\n", start);

    std::string data = response.substr(start, end == std::string::npos ? end : end - start);

    // Trim spaces around fields.
    auto trim = [](std::string& value) {
        size_t first = value.find_first_not_of(" \t");
        if (first == std::string::npos) {
            value.clear();
            return;
        }

        size_t last = value.find_last_not_of(" \t");
        value = value.substr(first, last - first + 1);
    };

    // Split the 18 fields, preserving empty fields.
    std::array<std::string, 18> fields;
    size_t pos = 0;

    for (size_t c = 0; c < fields.size(); c++) {
        size_t comma = data.find(',', pos);

        if (c < fields.size() - 1 && comma == std::string::npos) {
            return std::nullopt;
        }

        if (c == fields.size() - 1 && comma != std::string::npos) {
            return std::nullopt;
        }

        fields[c] = data.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos
        );

        trim(fields[c]);

        if (comma == std::string::npos) {
            break;
        }

        pos = comma + 1;
    }

    // Strict numeric conversion.
    auto parse_double = [](const std::string& text, double& result) -> bool {
        if (text.empty()) {
            return false;
        }

        char* endptr = nullptr;
        result = std::strtod(text.c_str(), &endptr);

        return endptr != text.c_str() && *endptr == '\0' && std::isfinite(result);
    };

    auto parse_int = [&](const std::string& text, int& result) -> bool {
        double value;

        if (!parse_double(text, value) || value < 0 || value > 100 || std::floor(value) != value) {
            return false;
        }

        result = static_cast<int>(value);
        return true;
    };

    // Field 0: fix mode (2=2D, 3=3D).
    int mode;

    if (!parse_int(fields[0], mode) || (mode != 2 && mode != 3)) {
        return std::nullopt;
    }

    // Field 17: satellites used for positioning.
    int satellites;

    if (!parse_int(fields[17], satellites) || satellites < (mode == 3 ? 4 : 3)) {
        return std::nullopt;
    }

    // SIMCom coordinates: ddmm.mmmmmm / dddmm.mmmmmm.
    auto parse_coordinate = [&](const std::string& text,
                                const std::string& hemisphere,
                                bool latitude,
                                double& result) -> bool {
        double raw;

        if (!parse_double(text, raw) || raw < 0) {
            return false;
        }

        int degrees = static_cast<int>(raw / 100);
        double minutes = raw - degrees * 100.0;

        if (minutes < 0 || minutes >= 60) {
            return false;
        }

        result = degrees + minutes / 60.0;

        if (latitude) {
            if (result > 90) {
                return false;
            }

            if (hemisphere == "S") {
                result = -result;
            }

            else if (hemisphere != "N") {
                return false;
            }
        }
        else {
            if (result > 180) {
                return false;
            }

            if (hemisphere == "W") {
                result = -result;
            }

            else if (hemisphere != "E") {
                return false;
            }
        }

        return true;
    };

    A7670Modem::GpsFix fix{};

    // Fields 5-8: position.
    if (!parse_coordinate(fields[5], fields[6], true, fix.latitude) || !parse_coordinate(fields[7], fields[8], false, fix.longitude)) {
        return std::nullopt;
    }

    // Field 11: altitude (metres).
    // A 2D fix may have no valid altitude.
    if (!parse_double(fields[11], fix.altitude) && mode == 3) {
        return std::nullopt;
    }

    // Field 15: horizontal dilution of precision.
    if (!parse_double(fields[15], fix.hdop) || fix.hdop <= 0) {
        return std::nullopt;
    }

    fix.satellites = satellites;

    return fix;
}

bool A7670Modem::connect_network(uint32_t timeout_ms) {
    ESP_LOGI(TAG, "Enabling cellular modem");

    // Enable full cellular functionality
    write_command("AT+CFUN=1");

    std::string response = read_response(10000);
    log_response("AT+CFUN=1", response);

    if (response.find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to enable cellular modem");
        return false;
    }

    ESP_LOGI(TAG, "Waiting for network registration...");

    if (!wait_for_network(timeout_ms)) {
        ESP_LOGE(TAG, "Network registration timed out");
        return false;
    }

    ESP_LOGI(TAG, "Network registered");

    // Configure SMS text mode
    write_command("AT+CMGF=1");

    response = read_response(2000);
    log_response("AT+CMGF=1", response);

    if (response.find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to configure SMS text mode");
        return false;
    }

    ESP_LOGI(TAG, "SMS ready");

    return true;
}

std::string A7670Modem::format_location_sms(const GpsFix& fix) {
    char buffer[256];

    snprintf(buffer,
        sizeof(buffer),
        "GPS Location:\n"
        "https://maps.google.com/?q=%.6f,%.6f\n"
        "Altitude: %.1f m\n"
        "Satellites: %d\n"
        "HDOP: %.2f",
        fix.latitude,
        fix.longitude,
        fix.altitude,
        fix.satellites,
        fix.hdop
    );

    return std::string(buffer);
}

bool A7670Modem::power_off_modem() {
    ESP_LOGI(TAG, "Powering off A7670");

    write_command("AT+CPOF");

    std::string response = read_response(10000);
    log_response("AT+CPOF", response);

    // Examine the response and, ideally, a modem
    // status signal to confirm completed shutdown.
    // Don't treat transmission of AT+CPOF alone
    // as proof that shutdown succeeded.

    vTaskDelay(pdMS_TO_TICKS(3000));

    gpio_set_level(BOARD_LED_PIN, 0);

    ESP_ERROR_CHECK(uart_driver_delete(uart_number));

    return !check_respond();
}

void A7670Modem::deinit_hardware() {
    ESP_LOGI(TAG, "Deinitialising modem hardware");

    // Ensure board LED is off
    gpio_set_level(BOARD_LED_PIN, 0);

    // Release UART driver
    if (uart_is_driver_installed(uart_number)) {
        ESP_ERROR_CHECK(uart_wait_tx_done(uart_number, pdMS_TO_TICKS(1000)));
        ESP_ERROR_CHECK(uart_driver_delete(uart_number));
    }

    ESP_LOGI(TAG, "Modem hardware deinitialised");
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

// Client should call this to start listening for incoming SMS and calls
void A7670Modem::start_network_event_listener() {
    if (!sms_task_handle) {
        xTaskCreate(network_event_task_wrapper, "sms_task", 4096, this, 5, &sms_task_handle);
    }
}

// FreeRTOS tasks need static functions, so we wrap the member function
void A7670Modem::network_event_task_wrapper(void* param) {
    A7670Modem* modem = static_cast<A7670Modem*>(param);
    modem->network_event_task();
}

// Handl incoming SMS and calls
void A7670Modem::network_event_task() {
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

