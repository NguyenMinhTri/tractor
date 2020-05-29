## Tractor

This is an ESP32 firmware for measuring load cells and publishing to the Google Cloud IoT Core.

#### Features
- Ready for remote deploy and control
- Built and tested with ESP32-DevKit-C, ESP-IDF, Google IoT Core
- HX711 readings
- MQTT Messaging
- OTA Updates
- OTA Commands

#### Configuration
1. Add an elliptic `private_key.pem` to main/certs. It must match the public key of Google IoT Core registry.
2. Run `idf.py menuconfig` and set these two configurations sets:
    - `App Tractor Config -->`
    - `Component config --> Google IoT Core Configuration -->` Device_ID can be unset, as it is defined at runtime from the base MAC address.

#### To do
- Use the ULP co-processor for HX711 readings
- Only publish when changes are detected
- OTA Wifi AP setup 
- Mesh BLE networking + 1 Wifi gateway
- Device specific keys
