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

static uart_port_t uartNum;
static TaskHandle_t smsTaskHandle;

std::string extractSingleAtLine(const std::string &raw, const std::string &prefix)
{
    std::istringstream stream(raw);
    std::string line;

    while (std::getline(stream, line)) {
        // trim CR
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // trim leading spaces
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos)
            line = line.substr(first);

        if (line.rfind(prefix, 0) == 0) { // starts with prefix
            return line;
        }
    }

    return "";
}

std::string extractHttpSection(const std::string &raw, const std::string &sectionPrefix) {
    // Find section start
    size_t start = raw.find(sectionPrefix);
    if (start == std::string::npos) return "";

    // Skip the +HTTP... line
    size_t lineEnd = raw.find('\n', start);
    if (lineEnd == std::string::npos) return "";
    start = lineEnd + 1;

    // Take everything after that
    std::string section = raw.substr(start);

    // Split into lines
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < section.size()) {
        size_t next = section.find('\n', pos);
        std::string line;

        if (next != std::string::npos) {
            line = section.substr(pos, next - pos);
            pos = next + 1;
        } else {
            line = section.substr(pos);
            pos = section.size();
        }

        // trim whitespace
        line.erase(0, line.find_first_not_of("\r\n\t "));
        line.erase(line.find_last_not_of("\r\n\t ") + 1);

        if (!line.empty())
            lines.push_back(line);
    }

    // Remove last line (modem-specific: OK or +HTTPREAD: 0)
    if (!lines.empty())
        lines.pop_back();

    // Rebuild result
    std::string result;
    for (const auto &l : lines) {
        if (!result.empty()) result += "\n";
        result += l;
    }

    return result;
}





int parseLen(const std::string &resp) {
    // resp example: "+HTTPREAD: LEN,36"
    size_t pos = resp.find(',');
    if (pos != std::string::npos) {
        return atoi(resp.substr(pos + 1).c_str());
    }
    return 0; // fallback
}

// Reads a single line (terminated by \n) from the modem with a timeout
std::string readLine(int timeout_ms) {
    std::string line;
    char c;
    int64_t start = esp_timer_get_time(); // microseconds

    while ((esp_timer_get_time() - start) < timeout_ms * 1000) {
        int len = uart_read_bytes(uartNum, (uint8_t*)&c, 1, pdMS_TO_TICKS(50));
        if (len > 0) {
            if (c == '\n') {
                break; // end of line
            } else if (c != '\r') {
                line += c;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10)); // small delay
        }
    }
    return line;
}

void writeCommand(const std::string& cmd) {
    std::string c = cmd + "\r\n";
    uart_write_bytes(uartNum, c.c_str(), c.length());
}

std::string readResponse(int timeout_ms) {
    std::string resp;
    char buf[128];

    int64_t start = esp_timer_get_time() / 1000;

    while ((esp_timer_get_time() / 1000 - start) < timeout_ms) {
        int len = uart_read_bytes(uartNum, (uint8_t*)buf, sizeof(buf)-1, pdMS_TO_TICKS(100));
        if (len > 0) {
            buf[len] = '\0';
            resp += buf;

            if (resp.find("\r\nOK") != std::string::npos ||
                resp.find("\r\nERROR") != std::string::npos) {
                break;
            }
        }
    }
    return resp;
}

bool checkRespond() {
    const int maxRetries = 10;
    const int delayMs = 1000; // 1 second between retries

    for (int j = 0; j < maxRetries; j++) {
        writeCommand("AT"); // simple ping
        std::string resp = readResponse(1000);
        if (resp.find("OK") != std::string::npos) {
            ESP_LOGI(TAG, "Modem responded");

            // Safe configuration
            writeCommand("ATE0");          // Disable echo
            readResponse(500);

            writeCommand("ATI");           // Get modem info
            readResponse(500);

            writeCommand("AT+CMGF=1");     // SMS text mode
            readResponse(500);

            return true;
        }

        ESP_LOGW(TAG, "No response, retry %d/%d", j + 1, maxRetries);
        vTaskDelay(pdMS_TO_TICKS(delayMs));
    }

    ESP_LOGE(TAG, "Modem did not respond after %d retries", maxRetries);
    return false;
}

bool waitForNetwork(int timeout_ms) {
    const int poll_interval_ms = 1000; // check every 1s
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        writeCommand("AT+CEREG?");
        std::string resp = readResponse(500);

        // look for +CEREG: <n>,<stat>
        size_t pos = resp.find("+CEREG:");
        if (pos != std::string::npos) {
            int stat = -1;
            sscanf(resp.c_str() + pos, "+CEREG: %*d,%d", &stat);

            if (stat == 1 || stat == 5) { 
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

bool isNetworkConnected() {
    writeCommand("AT+CEREG?");
    std::string resp = readResponse(500);

    // look for +CEREG: <n>,<stat>
    size_t pos = resp.find("+CEREG:");
    if (pos != std::string::npos) {
        int stat = -1;
        sscanf(resp.c_str() + pos, "+CEREG: %*d,%d", &stat);

        // 1 = registered home network, 5 = registered roaming
        if (stat == 1 || stat == 5) {
            return true;
        }
    }

    return false;
}

bool gprsConnect(const std::string &apn,
                 const std::string &user,
                 const std::string &pass,
                 int timeout_ms)
{
    ESP_LOGI(TAG, "Configuring GPRS...");

    std::string resp;
    std::string cmd;

    // --- 1. Set authentication ---
    if (!user.empty() || !pass.empty()) {
        cmd = "AT+CGAUTH=1,0,\"" + user + "\",\"" + pass + "\"";
        writeCommand(cmd.c_str());
        resp = readResponse(2000);
        if (resp.find("OK") == std::string::npos) {
            ESP_LOGW(TAG, "Auth command returned error (likely empty password), ignoring...");
            // Continue anyway, like TinyGSM
        } else {
            ESP_LOGI(TAG, "Auth set successfully");
        }
    } else {
        // If both empty, still send command for compatibility
        cmd = "AT+CGAUTH=1,0";
        writeCommand(cmd.c_str());
        resp = readResponse(2000);
        ESP_LOGI(TAG, "Auth command sent with no credentials");
    }

    // --- 2. Set PDP context ---
    cmd = "AT+CGDCONT=1,\"IP\",\"" + apn + "\",\"0.0.0.0\",0,0";
    writeCommand(cmd.c_str());
    resp = readResponse(2000);
    if (resp.find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to set PDP context: %s", resp.c_str());
        return false;
    }

    // --- 3. Activate context ---
    writeCommand("AT+CGACT=1,1");
    resp = readResponse(5000);
    if (resp.find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to activate PDP context: %s", resp.c_str());
        return false;
    }

    // --- 4. Open network connection ---
    writeCommand("AT+NETOPEN");
    resp = readResponse(timeout_ms);

    if (resp.find("OK") != std::string::npos ||
      resp.find("+NETOPEN: 0") != std::string::npos ||
      resp.find("Network is already opened") != std::string::npos) {
      ESP_LOGI(TAG, "GPRS connected!");
      return true;
    }

    ESP_LOGE(TAG, "Failed to connect GPRS: %s", resp.c_str());
    return false;
}

std::string getLocalIP() {
    writeCommand("AT+IPADDR");
    std::string resp = readResponse(1000);

    // The response usually contains: +IPADDR: x.x.x.x
    size_t pos = resp.find("+IPADDR:");
    if (pos != std::string::npos) {
        // skip past "+IPADDR:" and any whitespace
        std::string ip = resp.substr(pos + 8);
        // remove trailing newline/carriage return
        ip.erase(ip.find_last_not_of("\r\n ") + 1);
        return ip;
    }

    return "0.0.0.0"; // default if not found
}

A7670Modem::A7670Modem() {
  uartNum = static_cast<uart_port_t>(1);
  smsTaskHandle = nullptr;
}

A7670Modem::~A7670Modem() {
  if (smsTaskHandle) vTaskDelete(smsTaskHandle);
}

void A7670Modem::begin(const std::string& startupNumber, const std::string& startupMessage) {
    pendingStartupNumber = startupNumber;
    pendingStartupMessage = startupMessage;

    uart_config_t uart_config;
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.rx_flow_ctrl_thresh = 0;
    uart_config.rx_glitch_filt_thresh = 0;

    uart_param_config(uartNum, &uart_config);
    uart_set_pin(uartNum, MODEM_TX_PIN, MODEM_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(uartNum, 2048, 0, 0, nullptr, 0);

    gpio_set_direction(MODEM_DTR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_PWRKEY_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_LED_PIN, GPIO_MODE_OUTPUT);

    powerOnModem();

    // Send startup SMS
    sendSMS(pendingStartupNumber, pendingStartupMessage);
}

void A7670Modem::powerOnModem() {
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
    bool started = checkRespond();
    if (!started) {
      ESP_LOGI(TAG, "modem failed to start");
      return;
    }
    else {
      ESP_LOGI(TAG, "modem started");
    }

    // Set LED pin on
    gpio_set_level(BOARD_LED_PIN, 1);

    std::string modemName = getModemName();
    ESP_LOGI(TAG, "Modem name: %s", modemName.c_str());

    std::string modemInfo = getModemInfo();
    ESP_LOGI(TAG, "Modem info: %s", modemInfo.c_str());

    std::string simCCID = getSimCCID(10000);
    ESP_LOGI(TAG, "SIM CCID: %s", simCCID.c_str());

    std::string imei = getIMEI();
    ESP_LOGI(TAG, "IMEI: %s", imei.c_str());

    int signalQuality = 99;
    while (signalQuality == 99)
    {
        signalQuality = getSignalQuality();
        ESP_LOGI(TAG, "Signal quality (0-31): %d", signalQuality);
        vTaskDelay(pdMS_TO_TICKS(5000)); 
    }

    // Wait for the network
    ESP_LOGI(TAG, "Waiting for network...");
    if (!waitForNetwork(10000)) {
      ESP_LOGI(TAG, " fail");
      vTaskDelay(pdMS_TO_TICKS(10000)); 
      return;
    }
    ESP_LOGI(TAG, " success");
    if (isNetworkConnected()) {
      ESP_LOGI(TAG, "Network connected");
    }

    std::string networkOperator = getOperator();
    ESP_LOGI(TAG, "Operator: %s", networkOperator.c_str());

    // Connect to GPRS
    ESP_LOGI(TAG, "Connecting to GPRS...");
    std::string apn = CONFIG_LTE_NETWORK_APN;
    std::string user = CONFIG_LTE_NETWORK_USER;
    std::string pass = CONFIG_LTE_NETWORK_PASSWORD;
    if (gprsConnect(apn, user, pass, 10000)) {
        ESP_LOGI(TAG, "GPRS connected!");
        ESP_LOGI(TAG, "Local IP: ");
        ESP_LOGI(TAG, "%s", getLocalIP().c_str());
        ESP_LOGI(TAG, "waiting for services...");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    else {
        ESP_LOGI(TAG, "GPRS connection failed");
        return;
    }

    writeCommand("AT+CREG?");
    std::string creg = readResponse(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "AT+CREG?");
    ESP_LOGI(TAG, "%s", creg.c_str());
    ESP_LOGI(TAG, "=========================================================");

    writeCommand("AT+CGREG?");
    std::string cgreg = readResponse(READ_RESPONSE_TIMEOUT);
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "AT+CGREG?");
    ESP_LOGI(TAG, "%s", cgreg.c_str());
    ESP_LOGI(TAG, "=========================================================");

    // Turn off echo
    writeCommand("ATE0"); readResponse(READ_RESPONSE_TIMEOUT);
}

bool A7670Modem::sendSMS(const std::string& number, const std::string& message) {
    // SMS text mode
    writeCommand("AT+CMGF=1"); readResponse(READ_RESPONSE_TIMEOUT);
    // Tell the modem which character encoding to use for text messages
    // "GSM" means the GSM 7-bit default alphabet
    writeCommand("AT+CSCS=\"GSM\""); readResponse(READ_RESPONSE_TIMEOUT);

    std::string cmd = "AT+CMGS=\"" + number + "\"";
    writeCommand(cmd);
    readResponse(READ_RESPONSE_TIMEOUT);

    uart_write_bytes(uartNum, message.c_str(), message.length());
    uint8_t ctrlZ = 26;
    uart_write_bytes(uartNum, (char*)&ctrlZ, 1);

    std::string resp = readResponse(5000);
    bool ok = resp.find("OK") != std::string::npos;
    ESP_LOGI(TAG, "SMS send %s", ok ? "success" : "failed");
    return ok;
}

void A7670Modem::startSMSListener() {
    if (!smsTaskHandle) {
        xTaskCreate(smsTaskWrapper, "smsTask", 4096, this, 5, &smsTaskHandle);
    }
}

// FreeRTOS tasks need static functions, so we wrap the member function
void A7670Modem::smsTaskWrapper(void* param) {
    A7670Modem* modem = static_cast<A7670Modem*>(param);
    modem->smsTask();
}

void A7670Modem::smsTask() {
    char buf[512];
    std::string line;
    bool receivingMessage = false;
    std::string senderNumber;

    while (true) {
        int len = uart_read_bytes(uartNum, (uint8_t*)buf, sizeof(buf)-1, pdMS_TO_TICKS(500));
        if (len > 0) {
            buf[len] = 0;
            line += buf;

            size_t newlinePos;
            while ((newlinePos = line.find('\n')) != std::string::npos) {
                std::string currentLine = trim(line.substr(0, newlinePos));
                line.erase(0, newlinePos + 1);

                if (currentLine.starts_with("+CMT: ")) {
                    // Extract sender
                    size_t firstQuote = currentLine.find("\"");
                    size_t secondQuote = currentLine.find("\"", firstQuote + 1);
                    senderNumber = currentLine.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                    receivingMessage = true;
                } else if (receivingMessage) {
                    std::string receivedMessage = trim(currentLine);
                    receivingMessage = false;
                    // Handle message
                    handleIncomingSMS(senderNumber, receivedMessage);
                }
            }
        }
    }
}

// In A7670.cpp
void A7670Modem::handleIncomingSMS(const std::string &from, const std::string &msg) {
    ESP_LOGI(TAG, "SMS from %s: %s", from.c_str(), msg.c_str());

    // If message contains "status", reply
    if (msg.find("status") != std::string::npos) {
        ESP_LOGI(TAG, "'status' command received, sending reply");

        std::string reply = "--- Device Status ---\n";
        reply += "Status: Online\n";
        reply += "Operator: " + getOperator() + "\n";
        reply += "Signal: " + std::to_string(getSignalQuality()) + "\n";
        reply += "Time: " + getTime() + "\n";

        sendSMS(from, reply);
    }
}

std::string A7670Modem::getModemName() {
    writeCommand("ATI");
    std::string resp = readResponse(READ_RESPONSE_TIMEOUT);

    size_t pos = resp.find("Model:");
    if (pos != std::string::npos) {
        size_t end = resp.find("\n", pos);
        return trim(resp.substr(pos + 6, end - (pos + 6)));
    }

    return "Unknown";
}

std::string A7670Modem::getModemInfo() {
    writeCommand("ATI");
    std::string resp = readResponse(READ_RESPONSE_TIMEOUT);

    std::string info;

    size_t pos;

    if ((pos = resp.find("Manufacturer:")) != std::string::npos) {
        size_t end = resp.find("\n", pos);
        info += trim(resp.substr(pos, end - pos)) + " ";
    }

    if ((pos = resp.find("Model:")) != std::string::npos) {
        size_t end = resp.find("\n", pos);
        info += trim(resp.substr(pos, end - pos)) + " ";
    }

    if ((pos = resp.find("Revision:")) != std::string::npos) {
        size_t end = resp.find("\n", pos);
        info += trim(resp.substr(pos, end - pos));
    }

    return trim(info);
}

std::string A7670Modem::getSimCCID(int timeout_ms) {
    int elapsed = 0;
    const int interval = 500; // ms

    while (elapsed < timeout_ms) {
        writeCommand("AT+CPIN?");
        std::string cpinResp = readResponse(1000);
        if (cpinResp.find("READY") != std::string::npos) {
            // SIM is ready, now get CCID
            writeCommand("AT+CICCID");
            std::string resp = readResponse(1000);

            // Extract digits only
            size_t start = resp.find_first_of("0123456789");
            if (start != std::string::npos) {
                size_t end = resp.find_first_not_of("0123456789", start);
                return resp.substr(start, end - start);
            }
            return "N/A"; // unexpected format
        }

        vTaskDelay(pdMS_TO_TICKS(interval));
        elapsed += interval;
    }

    return "N/A"; // timed out waiting for SIM
}

std::string A7670Modem::getIMEI() {
    writeCommand("AT+GSN");
    std::string resp = readResponse(READ_RESPONSE_TIMEOUT);

    // IMEI is usually just a number line before OK
    size_t start = resp.find_first_of("0123456789");
    if (start != std::string::npos) {
        size_t end = resp.find("\n", start);
        return trim(resp.substr(start, end - start));
    }

    return "N/A";
}

int A7670Modem::getSignalQuality() {
    writeCommand("AT+CSQ");
    std::string resp = readResponse(READ_RESPONSE_TIMEOUT);

    // Example: +CSQ: 12,99
    size_t pos = resp.find("+CSQ:");
    if (pos != std::string::npos) {
        int rssi = -1;
        sscanf(resp.c_str() + pos, "+CSQ: %d", &rssi);
        return rssi;
    }

    return -1; // invalid
}

std::string A7670Modem::getOperator() {
    writeCommand("AT+COPS?");
    std::string resp = readResponse(READ_RESPONSE_TIMEOUT);

    // Example: +COPS: 0,2,"23410",7
    size_t pos = resp.find("+COPS:");
    if (pos != std::string::npos) {
        size_t firstQuote = resp.find('"', pos);
        size_t secondQuote = resp.find('"', firstQuote + 1);

        if (firstQuote != std::string::npos && secondQuote != std::string::npos) {
            return resp.substr(firstQuote + 1, secondQuote - firstQuote - 1);
        }
    }

    return "N/A";
}

std::string A7670Modem::getTime() {
    // Placeholder for actual time; can be expanded with AT+CCLK or NITZ parsing
    return "Not available";
}

std::string A7670Modem::trim(const std::string& s) {
    size_t start = s.find_first_not_of("\r\n ");
    size_t end = s.find_last_not_of("\r\n ");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

bool A7670Modem::httpsPOST(const std::string &url, const std::string &json_data, const std::string &apiKey)
{
    ESP_LOGI(TAG, "Starting HTTPS POST...");

    writeCommand("AT+QNWINFO");
    std::string raw1 = readResponse(2000);
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "QNWINFO:");
    ESP_LOGI(TAG, "%s", raw1.c_str());
    ESP_LOGI(TAG, "=======================================");
    std::string qnwinfo = extractSingleAtLine(raw1, "+QNWINFO:");
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "QNWINFO:");
    ESP_LOGI(TAG, "%s", qnwinfo.c_str());
    ESP_LOGI(TAG, "=======================================");

    // Ensure PDP context is active
    writeCommand("AT+CGACT=1,1");
    if (readResponse(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to activate PDP context");
        return false;
    }

    // Close any previous HTTP session
    writeCommand("AT+HTTPTERM");
    readResponse(2000); // ignore ERROR if no previous session

    // Init HTTP service
    writeCommand("AT+HTTPINIT");
    if (readResponse(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "HTTPINIT failed");
        return false;
    }

    // Set SSL/TLS version (TLS 1.2)
    writeCommand("AT+CSSLCFG=\"sslversion\",0,4");
    readResponse(2000);

    // Enable SNI
    writeCommand("AT+CSSLCFG=\"enableSNI\",0,1");
    readResponse(2000);

    // Set URL
    writeCommand(("AT+HTTPPARA=\"URL\",\"" + url + "\"").c_str());
    if (readResponse(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to set URL");
        return false;
    }

    // Set user-agent
    writeCommand("AT+HTTPPARA=\"USERDATA\",\"User-Agent: TinyGSM/ESP-IDF\"");
    readResponse(2000);

    // Set custom API key header
    writeCommand(("AT+HTTPPARA=\"USERDATA\",\"X-API-KEY: " + apiKey + "\"").c_str());
    readResponse(2000);

    // Set data to send
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+HTTPDATA=%d,10000", (int)json_data.length());
    writeCommand(cmd);

    if (readResponse(2000).find("DOWNLOAD") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to enter HTTPDATA mode");
        return false;
    }

    // Send JSON payload
    writeCommand(json_data.c_str());
    if (readResponse(5000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to send HTTP data");
        return false;
    }

    // 1. Send POST
    writeCommand("AT+HTTPACTION=1");

    // 2. Wait for +HTTPACTION
    std::string resp;
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < 15000000) {
        std::string line = readLine(500);
        if (!line.empty()) {
            resp += line + "\n";
            if (line.find("+HTTPACTION: 1,") != std::string::npos) break;
        }
    }
    if (resp.find("+HTTPACTION: 1,200") == std::string::npos) {
        ESP_LOGE(TAG, "HTTPACTION failed: %s", resp.c_str());
        return false;
    }

    writeCommand("AT+HTTPHEAD");
    std::string raw = readResponse(2000);
    ESP_LOGI(TAG, "HEADERS START:");
    ESP_LOGI(TAG, "%s", raw.c_str());
    ESP_LOGI(TAG, "HEADERS STOP");

    std::string headers = extractHttpSection(raw, "+HTTPHEAD:");

    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "HTTPS HEADERS:");
    ESP_LOGI(TAG, "%s", headers.c_str());
    ESP_LOGI(TAG, "=======================================");

    // 3. Query length
    writeCommand("AT+HTTPREAD?");
    std::string header = readResponse(2000); // +HTTPREAD: <len>
    int len = parseLen(header); // extract the available length

    // 1. Send HTTPREAD
    snprintf(cmd, sizeof(cmd), "AT+HTTPREAD=0,%d", len);
    writeCommand(cmd);

    raw = readResponse(2000);

    ESP_LOGI(TAG, "HTTPREAD RESPONSE START:");
    ESP_LOGI(TAG, "%s", raw.c_str());
    ESP_LOGI(TAG, "HTTPREAD RESPONSE STOP");

    std::string body = extractHttpSection(raw, "+HTTPREAD:");

    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "HTTPS RESPONSE:");
    ESP_LOGI(TAG, "%s", body.c_str());
    ESP_LOGI(TAG, "=======================================");

    writeCommand("AT+HTTPTERM"); readResponse(2000);

    return true;
}

bool A7670Modem::httpsGET(const std::string &url) {
    // Ensure PDP context is active
    writeCommand("AT+CGACT=1,1");
    if (readResponse(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to activate PDP context");
        return false;
    }

    // Close any previous HTTP session
    writeCommand("AT+HTTPTERM");
    readResponse(2000); // ignore ERROR if no previous session

    // Init HTTP service
    writeCommand("AT+HTTPINIT");
    if (readResponse(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "HTTPINIT failed");
        return false;
    }

    // Set SSL/TLS version (TLS 1.2)
    writeCommand("AT+CSSLCFG=\"sslversion\",0,4");
    readResponse(2000);

    // Enable SNI
    writeCommand("AT+CSSLCFG=\"enableSNI\",0,1");
    readResponse(2000);

    // Set the URL
    std::string setUrlCommand = "AT+HTTPPARA=\"URL\",\"" + url + "\"";
    writeCommand(setUrlCommand);
    if (readResponse(2000).find("OK") == std::string::npos) {
        ESP_LOGE(TAG, "Failed to set GET URL");
        return false;
    }

    // Set user-agent
    writeCommand("AT+HTTPPARA=\"USERDATA\",\"User-Agent: TinyGSM/ESP-IDF\"");
    readResponse(2000);
    writeCommand("AT+HTTPACTION=0");

    // 2. Wait for +HTTPACTION
    std::string resp;
    size_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < 15000000) {
        std::string line = readLine(500);
        if (!line.empty()) {
            resp += line + "\n";
            if (line.find("+HTTPACTION: 0,") != std::string::npos) break;
        }
    }
    if (resp.find("+HTTPACTION: 0,200") == std::string::npos) {
        ESP_LOGE(TAG, "HTTPACTION GET failed: %s", resp.c_str());
        return false;
    }

    // 3. Query length
    writeCommand("AT+HTTPREAD?");
    std::string header = readResponse(2000); // +HTTPREAD: <len>
    int len = parseLen(header); // extract the available length

    // 1. Send HTTPREAD
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+HTTPREAD=0,%d", len);
    writeCommand(cmd);
    
    std::string raw = readResponse(2000);

    ESP_LOGI(TAG, "GET RESPONSE START");
    ESP_LOGI(TAG, "%s", raw.c_str());
    ESP_LOGI(TAG, "GET RESPONE STOP");
    std::string body = extractHttpSection(raw, "+HTTPREAD:");
    ESP_LOGI(TAG, "=======================================");
    ESP_LOGI(TAG, "HTTPS RESPONSE:");
    ESP_LOGI(TAG, "%s", body.c_str());
    ESP_LOGI(TAG, "=======================================");

    writeCommand("AT+HTTPTERM"); readResponse(2000);

    return true;
}
