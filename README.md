# WiFi-Watchdog

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.txt)
[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)](https://www.arduino.cc/reference/en/libraries/)
[![Platform: ESP8266/ESP32](https://img.shields.io/badge/Platform-ESP8266%20%7C%20ESP32-lightgrey.svg)]()

**Non-blocking WiFi connection manager with ICMP ping watchdog for ESP8266 and ESP32.**

WiFi-Watchdog monitors your ESP's WiFi connection using both the built-in `WiFi.status()` API and active ICMP echo requests (ping) to the gateway. When connectivity is lost, it automatically attempts to reconnect using an escalating strategy — from soft reconnection to full WiFi reset if needed. The entire implementation is **fully non-blocking** and never stalls your `loop()`.

---

## ✨ Features

| Feature | Description |
|---------|-------------|
| **Auto-reconnect** | Escalating strategy: soft `WiFi.reconnect()` → full disconnect/reconnect after repeated failures |
| **ICMP ping watchdog** | Non-blocking gateway ping via lwip raw API; detects upstream isolation even when WiFi reports connected |
| **100ms throttle** | Built-in call throttling prevents excessive execution if called from a tight loop |
| **APIPA rejection** | Treats `169.254.x.x` addresses as invalid, triggering reconnection |
| **Status callbacks** | Register a function that fires on every status transition |
| **DHCP / Static IP** | Switch between DHCP and static IP configuration |
| **Auto hostname** | Generates `ESP_ABCDEF` from MAC address if no hostname is set |
| **Custom ping target** | Override the default gateway with any IP on your network |
| **Runtime debug** | Enable/disable serial debug output with `setDebug(true)` — no recompilation needed |
| **ESP32 multi-core safe** | Shared state protected by `portMUX` spinlock, lwip calls wrapped with TCPIP core lock |
| **Zero dependencies** | Uses lwip raw API directly — no external ping library required |

---

## 📦 Installation

### Arduino Library Manager
1. Open Arduino IDE
2. Go to **Sketch** → **Include Library** → **Manage Libraries...**
3. Search for **"WiFi-Watchdog"**
4. Click **Install**

### Manual Installation
1. Download the latest release from [GitHub](https://github.com/costinbobes/WiFi-Watchdog/releases)
2. Extract to `~/Documents/Arduino/libraries/WiFi-Watchdog/`
3. Restart Arduino IDE

---

## 🚀 Quick Start

```cpp
#include <ESP8266WiFi.h>   // or <WiFi.h> for ESP32
#include <WiFi_Watchdog.h>

WiFi_Watchdog wifi;

void onStatusChange(WiFiWatchdogStatus status) {
    switch (status) {
        case WiFiWatchdogStatus::CONNECTED:
            Serial.print("Connected, IP: ");
            Serial.println(wifi.getIPAddress());
            break;
        case WiFiWatchdogStatus::CONNECTION_LOST:
            Serial.println("Connection lost!");
            break;
        default: break;
    }
}

void setup() {
    Serial.begin(115200);

    wifi.setDebug(true);                    // optional: enable serial debug output
    wifi.setHostname("my-device");          // optional: set hostname
    wifi.onStatusChange(onStatusChange);    // optional: register callback
    wifi.connect("MySSID", "MyPassword");
}

void loop() {
    wifi.watchdog();   // call every iteration — fully non-blocking
    // your code here...
}
```

---

## 📖 API Reference

### Connection Status

```cpp
enum class WiFiWatchdogStatus {
    DISCONNECTED,     // WiFi is off or explicitly disconnected
    CONNECTING,       // WiFi.begin() called, waiting for association
    CONNECTED,        // Associated with AP and has a valid IP
    CONNECTION_LOST,  // Was connected, connection dropped
    RECONNECTING      // Automatic reconnection in progress
};
```

### Connection Management

| Method | Description |
|--------|-------------|
| `connect(ssid, password)` | Connect to a WiFi network. Call configuration methods before this. |
| `disconnect()` | Disconnect and stop all monitoring. |
| `reset()` | Manual disconnect → re-apply config → reconnect. |

### Monitoring

| Method | Description |
|--------|-------------|
| `watchdog()` | **Call from `loop()`.** Non-blocking. Monitors connection, drives ICMP ping state machine, triggers reconnection. Has built-in 100ms throttle. |

### Status

| Method | Description |
|--------|-------------|
| `getStatus()` | Returns current `WiFiWatchdogStatus` (thread-safe on ESP32). |
| `onStatusChange(callback)` | Register a `void(WiFiWatchdogStatus)` callback for status transitions. |

### Network Info

| Method | Description |
|--------|-------------|
| `getIPAddress()` | Current local IP as `String`. |
| `getSignalStrength()` | Current RSSI in dBm. |
| `getMACAddress()` | WiFi MAC address as `String`. |

### Configuration *(call before `connect()`)*

| Method | Default | Description |
|--------|---------|-------------|
| `setHostname(hostname)` | `ESP_ABCDEF` | Set device hostname (auto-generated from MAC if not set). |
| `useDHCP()` | ✓ | Use DHCP for IP configuration. |
| `setStaticIP(ip, gw, subnet)` | - | Use static IP; overrides `useDHCP()`. |
| `setWiFiMode(mode)` | `WIFI_STA` | Set WiFi mode: `WIFI_STA`, `WIFI_AP`, `WIFI_AP_STA`. |
| `setDebug(enable)` | `false` | Enable/disable runtime serial debug output. |

### ICMP Pinger

| Method | Default | Description |
|--------|---------|-------------|
| `enablePinger(enable)` | `true` | Enable/disable the non-blocking ICMP ping. |
| `setPingInterval(ms)` | `60000` | Interval between ping attempts (ms). |
| `setPingTimeout(ms)` | `1000` | Per-ping reply timeout (ms). |
| `setPingTarget(ip)` | gateway | Override the ping destination. |

---

## 🔧 How It Works

### Connection Monitoring

1. **Primary detection**: Each `watchdog()` call checks `WiFi.status()` and validates the local IP
   - Rejects `0.0.0.0` (no IP)
   - Rejects `169.254.x.x` (APIPA/link-local)

2. **Secondary detection**: Non-blocking ICMP ping to gateway (or custom target)
   - State machine: `IDLE` → `SENT` (never blocks)
   - After 3 consecutive ping timeouts → full WiFi reset

3. **Reconnection strategy**:
   - Immediate first attempt when connection is lost
   - Subsequent retries every 30 seconds
   - After 5 consecutive failures → full reset (disconnect → delay → `WiFi.begin()`)

### Non-Blocking ICMP Ping

Uses the **lwip raw API** with a two-state machine:

| State | Description |
|-------|-------------|
| `IDLE` | Waiting for the next ping interval to elapse. |
| `SENT` | Echo request dispatched via lwip `raw_sendto()`; checking for reply each `watchdog()` call. |

- **ESP8266**: Uses standard lwip raw API
- **ESP32**: Wraps all lwip calls with `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` (required for FreeRTOS/ESP-IDF)

No blocking loops. The main `loop()` is never stalled.

### 100ms Throttle

The `watchdog()` method has a built-in throttle — if called again within 100ms of the last execution, it returns immediately. This prevents overloading when called from a tight loop while still allowing frequent checks.

---

## ⚙️ Default Values

| Parameter | Value |
|-----------|-------|
| Reconnect interval | 30 seconds |
| Ping interval | 60 seconds |
| Ping timeout | 1 second |
| Ping fail threshold | 3 consecutive failures |
| Max reconnect failures before full reset | 5 |
| Watchdog throttle | 100 ms |

---

## 🔍 Example Scenarios

### Basic Usage with Debug

```cpp
void setup() {
    Serial.begin(115200);
    wifi.setDebug(true);
    wifi.connect("MySSID", "MyPassword");
}

void loop() {
    wifi.watchdog();
}
```

### Static IP Configuration

```cpp
void setup() {
    Serial.begin(115200);

    IPAddress ip(192, 168, 1, 200);
    IPAddress gw(192, 168, 1, 1);
    IPAddress sn(255, 255, 255, 0);

    wifi.setStaticIP(ip, gw, sn);
    wifi.setHostname("sensor-node-01");
    wifi.connect("MySSID", "MyPassword");
}
```

### Aggressive Ping Monitoring

```cpp
void setup() {
    Serial.begin(115200);

    wifi.setPingInterval(10000);  // ping every 10 seconds
    wifi.setPingTimeout(2000);    // 2 second timeout
    wifi.connect("MySSID", "MyPassword");
}
```

### Custom Ping Target

Useful when the gateway always responds but upstream network is unreliable:

```cpp
void setup() {
    Serial.begin(115200);

    // Ping a reliable server on your LAN instead of the gateway
    wifi.setPingTarget(IPAddress(192, 168, 1, 10));
    wifi.connect("MySSID", "MyPassword");
}
```

---

## 🐛 Troubleshooting

### Issue: Connection shows as connected but network is unreachable

**Cause**: WiFi status reports `WL_CONNECTED` but gateway or upstream network is down.

**Solution**: The ICMP ping watchdog is designed for this — it will detect the issue and trigger a reset after 3 consecutive ping failures. Ensure pinger is enabled:

```cpp
wifi.enablePinger(true);
```

### Issue: Frequent reconnections on ESP32

**Cause**: ESP32 may have more aggressive power management or different WiFi driver behavior.

**Solution**: Increase ping interval and timeout:

```cpp
wifi.setPingInterval(120000);  // 2 minutes
wifi.setPingTimeout(2000);      // 2 seconds
```

### Issue: Device doesn't reconnect after AP power cycle

**Cause**: WiFi driver may be in a stuck state.

**Solution**: The library's full reset (after 5 failures) should handle this. If it persists, add a manual reset:

```cpp
if (wifi.getStatus() == WiFiWatchdogStatus::CONNECTION_LOST) {
    // after some time...
    wifi.reset();
}
```

---

## 📋 Platform Support

- **ESP8266** — Tested on NodeMCU, Wemos D1 Mini
- **ESP32** — Tested on ESP32-DevKitC, ESP32-WROOM

| Platform | WiFi API | lwip | Thread Safety |
|----------|----------|------|---------------|
| ESP8266 | `ESP8266WiFi.h` | Single-threaded | N/A |
| ESP32 | `WiFi.h` | FreeRTOS + IPv6 | `portMUX`, `LOCK_TCPIP_CORE()` |

---

## 🤝 Contributing

Contributions are welcome! Please:
1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

---

## 📄 License

This project is licensed under the **MIT License** — see [LICENSE.txt](LICENSE.txt) for details.

---

## 🙏 Acknowledgments

- Built using the lwip raw API for ICMP echo (ping)
- Inspired by various ESP8266/ESP32 WiFi management patterns from the Arduino community

---

## 📬 Support

- **Issues**: [GitHub Issues](https://github.com/costinbobes/WiFi-Watchdog/issues)
- **Documentation**: [API Reference](https://github.com/costinbobes/WiFi-Watchdog#-api-reference)
- **Examples**: See [examples/](examples/) folder

---

**Made with ❤️ for the ESP8266/ESP32 community**
