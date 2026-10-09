#pragma once

#include <optional>
#include <string>


class A7670Modem {
public:
    struct GpsFix
    {
        double latitude;
        double longitude;
        double altitude;
        double hdop;
        int satellites;
    };

    A7670Modem();
    ~A7670Modem();

    bool init_hardware();

    bool power_on_modem();

    bool power_on_gps();

    bool power_off_gps();

    bool power_off_modem();

    bool connect_network(uint32_t timeout_ms);

    static std::string format_location_sms(const GpsFix& fix);

    void deinit_hardware();

    std::optional<GpsFix> get_gps_fix(uint32_t timeout_ms);

    // Start background task to monitor incoming SMS
    void start_network_event_listener();
    static void network_event_task_wrapper(void* param);
    void network_event_task();

    // Send SMS to a number
    bool send_sms(const std::string& number, const std::string& message);

    bool https_post(const std::string &url, const std::string &json_data, const std::string &apiKey);
    bool https_get(const std::string &url);

private:
    int tx_pin, rx_pin, dtr_pin, power_pin, led_pin;

    std::string pending_startup_number;
    std::string pending_startup_message;

    bool power_on_hardware();
    bool connect_to_network();
    bool connect_to_data_service();

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

    std::optional<GpsFix> parse_gnss_fix(const std::string& response);
};
