# ESP32 A7670
A project that connects to a mobile network over 4G and sends an initial SMS message to a registered phone number. It then POSTs a JSON file to a server over HTTPS and GETs a JSON file from the server over HTTPS. It then goes into an SMS listening loop. If an SMS arrives with "status" it responds with an SMS containing the current status of the module.

## Configuring the project
```
idf.py menuconfig
A7670 Configuration
```
