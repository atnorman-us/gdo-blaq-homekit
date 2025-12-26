# Konnected HomeKit Firmware
 This is the firmware for Konnected's HomeKit-enabled door opener.
  
 ## Submodules
 
 This repository uses git submodules. After cloning, be sure to initialize and update them:
 
 ```sh
 git submodule update --init --recursive
 ```

## Provision WiFi and Add Accessory to HomeKit

 This firmware uses the `nvs_wifi_connect` component to manage WiFi connections. The relevant code can be found in `main/wifi.cpp`.
 
 The `nvs_wifi_connect` component will start an HTTP server for configuration if the device is not connected to a WiFi network. Connect to the access point created by the device with SSID `konnected-blaq-hk` and open a web browser to `http://192.168.4.1` and enter your WiFi credentials, then click `Write and Reboot`.

 After connecting to WiFi and restarting wait about 10 seconds then open the Home app on your iOS device to add the accessory. Go to "Add Accessory" and then "more options..." to find the accessory on the network. Click on the found accessory and enter the setup code `251-02-023` when prompted and follow the instructions to complete the setup.