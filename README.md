# ESP32 A7670
A project that connects to a mobile network over 4G and sends an initial SMS message to a registered phone number. It then POSTs a JSON file to a server over HTTPS and GETs a JSON file from the server over HTTPS. It then goes into an SMS listening loop. If an SMS arrives with "status" it responds with an SMS containing the current status of the module.

## Requirements
esp-idf v6

## Configuring the project
```
idf.py menuconfig
A7670 Configuration
```

## Supported modules
The project currently works with:
* [LilyGo T-CALL A7670](https://lilygo.cc/en-us/products/t-call-v1-4)

## Resources
The SMS/HTTPS functionality was ported from the LilyGo Arduino examples:
* [SendSMS.ino](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/blob/main/examples/SendSMS)
* [HttpsBuiltlnGet](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/tree/main/examples/HttpsBuiltlnGet)
* [HttpsBuiltlnPost](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/tree/main/examples/HttpsBuiltlnPost)

### TODO
* Enable GPS on the A7670 and read location
* Support standalone A7670 module
