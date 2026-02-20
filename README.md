# WiFi_Watchdog

Non-blocking WiFi connection manager with ICMP ping watchdog for **ESP8266** and **ESP32**.

## Features

| Feature | Description |
|---|---|
| **Auto-reconnect** | Escalating strategy: soft `WiFi.reconnect()` → full disconnect/reconnect after repeated failures |
| **ICMP ping watchdog** | Non-blocking gateway ping via lwip raw API; detects upstream isolation even when WiFi reports connected |
| **Status callbacks** | Register a function that fires on every status transition |
| **DHCP / Static IP** | Switch between DHCP and static IP at any time before `connect()` |
| **Custom ping target** | Override the default gateway with any IP on your network |
| **Runtime debug logging** | Enable/disable serial debug output with `setDebug(true)` — no recompilation needed |
| **ESP32 multi-core safe** | Shared state protected by `portMUX` spinlock |
| **Zero external dependencies** | Uses lwip raw API directly — no third-party ping library required |

## Status Enum

```cpp
enum class WiFiWatchdogStatus : uint8_t {
    DISCONNECTED,     // WiFi is off or explicitly disconnected
    CONNECTING,       // WiFi.begin() called, waiting for association
    CONNECTED,        // Associated with AP and has a valid IP
    CONNECTION_LOST,  // Was connected, connection dropped
    RECONNECTING      // Automatic reconnection in progress
};
```

## Quick Start

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

    wifi.setDebug(true);                    // optional: serial debug output
    wifi.setHostname("my-device");          // optional
    wifi.onStatusChange(onStatusChange);    // optional
    wifi.connect("MySSID", "MyPassword");
}

void loop() {
    wifi.watchdog();
    // your code here...
}
```

## API Reference

### Connection

| Method | Description |
|---|---|
| `connect(ssid, password)` | Connect to a WiFi network |
| `disconnect()` | Disconnect and stop monitoring |
| `reset()` | Manual disconnect → re-apply config → reconnect |

### Monitoring

| Method | Description |
|---|---|
| `watchdog()` | **Call from `loop()`.** Non-blocking. Monitors connection, drives ICMP ping state machine, triggers reconnection |

### Status

| Method | Description |
|---|---|
| `getStatus()` | Returns current `WiFiWatchdogStatus` (thread-safe on ESP32) |
| `onStatusChange(callback)` | Register a `void(WiFiWatchdogStatus)` callback for status transitions |

### Network Info

| Method | Description |
|---|---|
| `getIPAddress()` | Current local IP as `String` |
| `getSignalStrength()` | Current RSSI in dBm |
| `getMACAddress()` | WiFi MAC address as `String` |

### Configuration (call before `connect()`)

| Method | Description |
|---|---|
| `setHostname(hostname)` | Set device hostname |
| `useDHCP()` | Use DHCP (default) |
| `setStaticIP(ip, gw, subnet)` | Use static IP; overrides `useDHCP()` |
| `setWiFiMode(mode)` | `WIFI_STA` (default), `WIFI_AP`, `WIFI_AP_STA` |
| `setDebug(enable)` | Enable/disable runtime serial debug output |

### ICMP Pinger

| Method | Default | Description |
|---|---|---|
| `enablePinger(enable)` | `true` | Enable/disable the non-blocking ICMP ping |
| `setPingInterval(ms)` | `60000` | Interval between ping attempts |
| `setPingTimeout(ms)` | `1000` | Per-ping reply timeout |
| `setPingTarget(ip)` | gateway | Override the ping destination |

## How It Works

### Connection Monitoring

1. Each `watchdog()` call checks `WiFi.status()` and validates the local IP
2. If connected with a valid IP, the ICMP ping state machine runs
3. If disconnected, an immediate reconnection attempt is triggered, followed by retries every 30 seconds
4. After 5 consecutive failures, a full WiFi reset is performed (disconnect → delay → reconnect)

### Non-Blocking ICMP Ping

The ping uses the **lwip raw API** (same layer used by the ESP8266-ping library) with a two-state machine:

- **IDLE** → when the ping interval elapses, `sendPing()` builds an ICMP echo request via `raw_sendto()` and returns immediately
- **SENT** → on each subsequent `watchdog()` call, `checkPingReply()` checks if the lwip callback set the reply flag, or if the timeout expired

No blocking loops. The main `loop()` is never stalled.

After 3 consecutive ping timeouts, a full WiFi reset is triggered.

## Defaults

| Parameter | Value |
|---|---|
| Reconnect interval | 30 seconds |
| Ping interval | 60 seconds |
| Ping timeout | 1 second |
| Ping fail threshold | 3 consecutive failures |
| Max reconnect failures before full reset | 5 |

## Platform Support

- **ESP8266** — uses `ESP8266WiFi.h`, `WiFi.hostname()`, lwip raw API
- **ESP32** — uses `WiFi.h`, `WiFi.setHostname()`, lwip raw API, `portMUX` for thread safety

## License

MIT — see [LICENSE.txt](LICENSE.txt)
