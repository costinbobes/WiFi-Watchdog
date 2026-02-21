/**
 * @file BasicUsage.ino
 * @brief Basic example of WiFi-Watchdog library usage
 * 
 * This example demonstrates:
 * - Simple WiFi connection with auto-reconnect
 * - Status change callback
 * - Runtime debug output
 * - ICMP ping monitoring
 * 
 * @author Costin Bobes
 * @license MIT
 */

// Platform-specific WiFi includes
#if defined(ESP8266)
    #include <ESP8266WiFi.h>
#elif defined(ESP32)
    #include <WiFi.h>
#else
    #error "This example requires ESP8266 or ESP32"
#endif

#include <WiFi_Watchdog.h>

// --- WiFi credentials ---
const char* ssid     = "YourSSID";
const char* password = "YourPassword";

// Create WiFi watchdog instance
WiFi_Watchdog wifiWatchdog;

/**
 * @brief Status change callback
 * 
 * This function is called whenever the WiFi connection status changes.
 * Use it to update your application state, trigger actions, or log events.
 */
void onWiFiStatusChange(WiFi_WatchdogStatus status) {
    switch (status) {
        case WiFi_WatchdogStatus::DISCONNECTED:
            Serial.println(F("[WiFi] Disconnected"));
            break;

        case WiFi_WatchdogStatus::CONNECTING:
            Serial.println(F("[WiFi] Connecting..."));
            break;

        case WiFi_WatchdogStatus::CONNECTED:
            Serial.print(F("[WiFi] ✓ Connected  IP: "));
            Serial.print(wifiWatchdog.getIPAddress());
            Serial.print(F("  RSSI: "));
            Serial.print(wifiWatchdog.getSignalStrength());
            Serial.println(F(" dBm"));
            break;

        case WiFi_WatchdogStatus::CONNECTION_LOST:
            Serial.println(F("[WiFi] ✗ Connection lost!"));
            break;

        case WiFi_WatchdogStatus::RECONNECTING:
            Serial.println(F("[WiFi] Reconnecting..."));
            break;
    }
}

void setup() {
    // Initialize serial for debug output
    Serial.begin(115200);
    while (!Serial && millis() < 3000); // Wait for serial on some boards
    Serial.println(F("\n\n=== WiFi-Watchdog Basic Example ===\n"));

    // --- Optional configuration (call before connect) ---
    
    // Enable debug output from the library
    wifiWatchdog.setDebug(true);
    
    // Set device hostname (will auto-generate from MAC if not set)
    wifiWatchdog.setHostname("esp-watchdog-demo");
    
    // Register status change callback
    wifiWatchdog.onStatusChange(onWiFiStatusChange);
    
    // Configure ping settings (optional)
    // wifiWatchdog.setPingInterval(60000);    // ping every 60 seconds (default)
    // wifiWatchdog.setPingTimeout(1000);      // 1 second timeout (default)
    // wifiWatchdog.enablePinger(true);        // enable ICMP ping (default)
    
    // Static IP configuration (optional)
    // IPAddress ip(192, 168, 1, 200);
    // IPAddress gw(192, 168, 1, 1);
    // IPAddress sn(255, 255, 255, 0);
    // IPAddress dns(8, 8, 8, 8);
    // wifiWatchdog.setStaticIP(ip, gw, sn);        // DNS defaults to gateway
    // wifiWatchdog.setStaticIP(ip, gw, sn, dns);   // or specify DNS explicitly
    
    // --- Connect to WiFi ---
    Serial.println(F("Connecting to WiFi..."));
    wifiWatchdog.connect(ssid, password);
}

void loop() {
    // Call watchdog on every iteration - it's fully non-blocking
    // Built-in 100ms throttle prevents excessive execution
    wifiWatchdog.watchdog();
    
    // Your application code here
    // ...
}
