# HomeKit Firmware for the GDO blaQ
 This is a HomeKit native firmware for Konnected's GDO blaQ, a smart garage door opener control accessory for Chamberlain/LiftMaster/Craftsman/Merlin garage openers with Security+ or Security+2.0.
  
 ## Submodules
 
 This repository uses git submodules. After cloning, be sure to initialize and update them:
 
 ```sh
 git submodule update --init --recursive
 ```

## Provision WiFi and Add Accessory to HomeKit

 This firmware uses the `nvs_wifi_connect` component to manage WiFi connections. The relevant code can be found in `main/wifi.cpp`.
 
 The `nvs_wifi_connect` component will start an HTTP server for configuration if the device is not connected to a WiFi network. Connect to the access point created by the device with SSID `konnected-blaq-hk` and open a web browser to `http://192.168.4.1` and enter your WiFi credentials, then click `Write and Reboot`.

 After connecting to WiFi and restarting wait about 10 seconds then open the Home app on your iOS device to add the accessory. Go to "Add Accessory" and then "more options..." to find the accessory on the network. Click on the found accessory and enter the setup code `251-02-023` when prompted and follow the instructions to complete the setup.

## Accessing the diagnostics web server
Find the device's IP — same one your router/Home app already knows it by (or check the boot log for esp_netif_handlers: sta ip: ...).
Open in a browser, on the same network as the device:

URLDashboard(
status + live log, auto-refreshing) --> `http://<device-ip>:8080/`

Raw status as JSON --> `http://<device-ip>:8080/status`

Log buffer, plain text (view/copy) --> `http://<device-ip>:8080/logs`

Log buffer, forced download --> `http://<device-ip>:8080/logs/download` → saves gdo-log.txt

## On the dashboard (/):
Status table: door / light / lock / obstruction / motion state, uptime, free heap, WiFi RSSI, connected HomeKit sessions, last reset reason, current log buffer usage.

Log pane: shows the last ~64KB of log history, refreshes every 3s by default (toggle off with the checkbox if you want to read a specific moment without it jumping).

Download full log button — same as hitting /logs/download directly, gives you a .txt file
