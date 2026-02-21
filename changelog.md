## Changelog

### v1.0.1 (2026-06-07)

#### Fixed
- Non-blocking: Replaced all `delay(100)` calls in reset paths with a ResetPhase state machine; `watchdog()` completes the reset after 100ms non-blockingly.
- `performFullReset()` now applies network config (hostname, static IP, DNS) before reconnecting.
- Added destructor (`~WiFi_Watchdog()`) to clean up PCB and free owned strings; deleted copy/move constructors and assignment operators (rule of five).
- ESP32: `_icmpReplyReceived` is now `std::atomic<bool>` for thread safety.
- PCB is now created lazily and reused for all pings; only destroyed in `cleanupPing()` (disconnect/destructor/WiFi loss).
- `connect()` now disconnects first if already connected.
- `setPingInterval()` and `setPingTimeout()` now clamp values to valid ranges and print debug warnings if needed.
- Added `clearPingTarget()` method to revert to default gateway ping.

#### Changed
- All string arguments (SSID, password, hostname) are copied internally using `strdup()`; freed in destructor and on overwrite.
- ICMP ping sequence number now increments for each ping; singleton enforcement with static `_instanceExists` flag and runtime warning.
- `setStaticIP()` now has a DNS overload; `applyNetworkConfig()` passes DNS to `WiFi.config()`.
- Removed `setWiFiMode()`; library now always uses `WIFI_STA` mode.
- Naming: Renamed `WiFiWatchdogStatus` → `WiFi_WatchdogStatus` everywhere (header, implementation, examples, README).
- Removed static buffer for hostname; now uses owned string.
- Documented that ICMP checksum is not validated (intentional: watchdog cares about live status, not packet integrity).

#### Added
- `setMaxFailuresBeforeReset()`, `setPingFailThreshold()`, and `setReconnectInterval()` methods for configurable thresholds (with range clamping and debug prints).
