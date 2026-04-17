#pragma once

#include <string>

class A7670Modem {
public:
    A7670Modem();
    ~A7670Modem();

    // Initialize modem, power on, and send startup SMS
    void begin(const std::string& startupNumber, const std::string& startupMessage);

    // Start background task to monitor incoming SMS
    void startSMSListener();
    static void smsTaskWrapper(void* param);
    void smsTask();

    // Send SMS to a number
    bool sendSMS(const std::string& number, const std::string& message);

    bool httpsPOST(const std::string &url, const std::string &json_data, const std::string &apiKey);
    bool httpsGET(const std::string &url);

private:
    int txPin, rxPin;
    int dtrPin, powerPin, ledPin;

    std::string pendingStartupNumber;
    std::string pendingStartupMessage;

    void powerOnModem();

    static void smsTask(void* param);
    void handleIncomingSMS(const std::string &from, const std::string &msg);

    // Helpers to build the status message
    std::string getTime();
    std::string getModemName();
    std::string getModemInfo();
    std::string getSimCCID(int timeout_ms);
    std::string getIMEI();
    int getSignalQuality();
    std::string getOperator();

    // Helper: trim CR/LF
    static std::string trim(const std::string& s);
};
