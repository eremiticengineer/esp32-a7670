#pragma once

#include <string>

class A7670Modem {
public:
    A7670Modem();
    ~A7670Modem();

    // Initialize modem, power on, and send startup SMS
    void begin(const std::string& startupNumber, const std::string& startupMessage);

    // Start background task to monitor incoming SMS
    void start_sms_listener();
    static void sms_task_wrapper(void* param);
    void sms_task();

    // Send SMS to a number
    bool send_sms(const std::string& number, const std::string& message);

    bool https_post(const std::string &url, const std::string &json_data, const std::string &apiKey);
    bool https_get(const std::string &url);

private:
    int tx_pin, rx_pin, dtr_pin, power_pin, led_pin;

    std::string pending_startup_number;
    std::string pending_startup_message;

    void power_on_modem();

    void handle_incoming_sms(const std::string &from, const std::string &msg);
    void handle_incoming_call();

    // Helpers to build the status message
    std::string get_time();
    std::string get_modem_name();
    std::string get_modem_info();
    std::string get_sim_ccid(int timeout_ms);
    std::string get_imei();
    int get_signal_quality();
    std::string get_operator();

    // Helper: trim CR/LF
    static std::string trim(const std::string& s);
};
