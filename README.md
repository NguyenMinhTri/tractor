# Tractor

This is an ESP32 firmware for measuring load cells and publishing to the Google Cloud IoT Core.

####Features:
- Ready for remote deploy and control
- Built and tested with ESP32-DevKit-C, ESP-IDF, Google IoT Core
- HX711 readings
- MQTT Messaging
- OTA Updates
- OTA Commands
- Auto-recovery in any non mapped situation
- Tool for source publishing and version bump `publi.sh bucketpath` (requires `gsutil`)
- Tool for device registration on Google, using esp32 MAC address as device_id `register.sh public_key.pem` (requires `gcloud`)

####Next:
- Use the ULP co-processor for HX711 readings
- Only publish when changes are detected
- OTA Wifi AP setup 
- Mesh BLE networking + 1 Wifi gateway
- Device specific keys

### Before building
1. Add the 'private_key.pem' to main/certs. It must match the public key of Google IoT Core registry.
2. Run `idf.py menuconfig and set these two configurations:
 - `App Tractor Config -->`
 - `Component config --> Google IoT Core Configuration -->` (Device ID can be unset, as it is created at runtime from the base MAC address)
