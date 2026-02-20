/**
 * @file    WiFi_Watchdog.h
 * @brief   Non-blocking WiFi connection manager with ICMP ping watchdog
 *          for ESP8266 and ESP32.
 * @author  Costin Bobes
 * @date    2/19/2026
 * @version 1.0.0
 * @license MIT
 * @see     https://github.com/costinbobes/WiFi-Watchdog
 *
 * @details
 * WiFi_Watchdog monitors WiFi connectivity via the built-in status API
 * and a non-blocking ICMP echo (ping) to the gateway or a custom target
 * using the lwip raw API.  It automatically reconnects with an escalating
 * strategy (soft reconnect, then full WiFi reset after repeated failures).
 *
 * ### Features
 * - Connect / disconnect / manual reset
 * - Automatic reconnection with escalating strategy
 * - Non-blocking ICMP ping to gateway (or custom target) via lwip raw API
 * - Status-change callback (DISCONNECTED → CONNECTING → CONNECTED → CONNECTION_LOST → RECONNECTING)
 * - DHCP or static IP configuration
 * - Hostname, WiFi mode, custom ping target
 * - Runtime debug logging via setDebug(true)
 * - ESP32 multi-core safe (portMUX spinlock around shared state)
 * - Zero external dependencies — uses lwip raw API directly
 *
 * ### Quick Start
 * @code
 * WiFi_Watchdog wifi;
 * wifi.setDebug(true);                        // optional: serial debug output
 * wifi.setHostname("my-device");              // optional: set hostname
 * wifi.onStatusChange(myCallback);            // optional: register status callback
 * wifi.setPingInterval(60000);                // optional: default 60 s
 * wifi.connect("SSID", "password");           // connect
 * // in loop():
 * wifi.watchdog();                            // call periodically — fully non-blocking
 * @endcode
 */

#ifndef _WiFi_Watchdog_h
#define _WiFi_Watchdog_h

#include "Arduino.h"

// --- Platform detection and WiFi includes ---
#if defined(ESP8266)
	#include <ESP8266WiFi.h>
#elif defined(ESP32)
	#include <WiFi.h>
#else
	#error "WiFi_Watchdog requires an ESP8266 or ESP32 platform"
#endif

extern "C" {
	#include <lwip/raw.h>
	#include <lwip/icmp.h>
	#include <lwip/inet_chksum.h>
	#include <lwip/sys.h>
#if defined(ESP8266)
	#include <user_interface.h>
#endif
}

/**
 * @brief WiFi connection status.
 *
 * Passed to the onStatusChange() callback whenever a state transition occurs.
 */
enum class WiFiWatchdogStatus : uint8_t {
	DISCONNECTED,     ///< WiFi is off or has been explicitly disconnected.
	CONNECTING,       ///< WiFi.begin() called, waiting for association.
	CONNECTED,        ///< Associated with AP and has a valid IP address.
	CONNECTION_LOST,  ///< Was connected, connection has dropped.
	RECONNECTING      ///< Automatic reconnection attempt in progress.
};

/**
 * @brief Callback function type for status-change notifications.
 * @param newStatus  The status the library just transitioned to.
 */
typedef void (*WiFiStatusCallback)(WiFiWatchdogStatus newStatus);

/**
 * @class WiFi_Watchdog
 * @brief Non-blocking WiFi connection manager with ICMP ping watchdog.
 *
 * Call configuration methods (setHostname(), setPingInterval(), etc.)
 * before connect().  Then call watchdog() from loop() on every iteration;
 * it is fully non-blocking and never stalls the main loop.
 *
 * ### Reconnection strategy
 * | Attempt | Action |
 * |---------|--------|
 * | 1 – 4   | `WiFi.reconnect()` (soft) |
 * | 5+      | Full disconnect → delay → `WiFi.begin()` (hard reset) |
 *
 * ### ICMP ping state machine
 * | State  | Description |
 * |--------|-------------|
 * | IDLE   | Waiting for the next ping interval to elapse. |
 * | SENT   | Echo request dispatched via lwip `raw_sendto()`; checking for reply each `watchdog()` call. |
 */
class WiFi_Watchdog {
public:
	/**
	 * @brief Construct a new WiFi_Watchdog instance.
	 *
	 * All settings are initialised to sensible defaults:
	 * - WiFi mode: WIFI_STA
	 * - DHCP enabled
	 * - Pinger enabled, 60 s interval, 1 s timeout, target = gateway
	 * - Debug output disabled
	 */
	WiFi_Watchdog();

	/** @name Connection Management */
	///@{

	/**
	 * @brief Connect to a WiFi network.
	 *
	 * Call configuration methods (setHostname(), setStaticIP(), etc.)
	 * **before** calling this.  Triggers a CONNECTING status transition.
	 *
	 * @param ssid      Network SSID (must remain valid for the lifetime of the connection).
	 * @param password  Network password (must remain valid for the lifetime of the connection).
	 */
	void connect(const char* ssid, const char* password);

	/**
	 * @brief Disconnect from WiFi and stop all monitoring.
	 *
	 * Cleans up any in-flight ICMP ping and sets status to DISCONNECTED.
	 */
	void disconnect();

	/**
	 * @brief Manual reconnect.
	 *
	 * Disconnects, re-applies network configuration (hostname, static IP),
	 * and calls WiFi.begin() again.  Sets status to RECONNECTING.
	 */
	void reset();

	///@}

	/** @name Monitoring */
	///@{

	/**
	 * @brief Non-blocking watchdog — call from loop().
	 *
	 * On each invocation this method:
	 * 1. Checks WiFi.status() and validates the local IP.
	 * 2. Drives the ICMP ping state machine (send / check reply / timeout).
	 * 3. Triggers automatic reconnection when a disconnect is detected.
	 *
	 * It is safe to call on every loop() iteration; all operations are
	 * non-blocking and time-gated internally.
	 */
	void watchdog();

	///@}

	/** @name Status */
	///@{

	/**
	 * @brief Get the current connection status.
	 * @return WiFiWatchdogStatus  Current status value.
	 * @note Thread-safe on ESP32 (uses portMUX spinlock).
	 */
	WiFiWatchdogStatus getStatus();

	/**
	 * @brief Register a callback for status transitions.
	 *
	 * The callback fires whenever the internal status changes, including
	 * on connection, disconnection, loss, and reconnection attempts.
	 *
	 * @param callback  Function pointer with signature `void(WiFiWatchdogStatus)`.
	 *                  Pass `nullptr` to unregister.
	 */
	void onStatusChange(WiFiStatusCallback callback);

	///@}

	/** @name Network Info */
	///@{

	/**
	 * @brief Get the current local IP address.
	 * @return String  IP address (e.g. "192.168.1.100"), or "0.0.0.0" if not connected.
	 */
	String getIPAddress();

	/**
	 * @brief Get the current WiFi signal strength.
	 * @return int32_t  RSSI in dBm (e.g. -65).  Lower (more negative) = weaker.
	 */
	int32_t getSignalStrength();

	/**
	 * @brief Get the WiFi MAC address.
	 * @return String  MAC address in "AA:BB:CC:DD:EE:FF" format.
	 */
	String getMACAddress();

	///@}

	/** @name Configuration (call before connect()) */
	///@{

	/**
	 * @brief Set the device hostname.
	 *
	 * Applied when connect() or reset() is called.
	 * Uses WiFi.hostname() on ESP8266 and WiFi.setHostname() on ESP32.
	 *
	 * @param hostname  Null-terminated hostname string (must remain valid).
	 */
	void setHostname(const char* hostname);

	/**
	 * @brief Use DHCP for IP configuration (this is the default).
	 *
	 * Calling this after setStaticIP() reverts to DHCP.
	 */
	void useDHCP();

	/**
	 * @brief Use a static IP address.
	 *
	 * Overrides useDHCP().  Call before connect().
	 *
	 * @param ip       Static IP address.
	 * @param gateway  Gateway IP address.
	 * @param subnet   Subnet mask.
	 */
	void setStaticIP(IPAddress ip, IPAddress gateway, IPAddress subnet);

	/**
	 * @brief Set the WiFi operating mode.
	 * @param mode  One of WIFI_STA (default), WIFI_AP, or WIFI_AP_STA.
	 */
	void setWiFiMode(WiFiMode_t mode);

	/**
	 * @brief Enable or disable runtime serial debug output.
	 *
	 * When enabled, the library prints diagnostic messages to Serial
	 * (e.g. ping results, reconnection attempts).  Disabled by default.
	 *
	 * @param enable  true to enable, false to disable.
	 */
	void setDebug(bool enable);

	///@}

	/** @name ICMP Pinger Configuration */
	///@{

	/**
	 * @brief Enable or disable the non-blocking ICMP gateway ping.
	 *
	 * When enabled (default), the library sends ICMP echo requests to
	 * the gateway (or custom target) at the configured interval.
	 * After @ref PING_FAIL_THRESHOLD consecutive timeouts a full WiFi
	 * reset is triggered.  When disabled, only WiFi.status() is monitored.
	 *
	 * @param enable  true to enable, false to disable.
	 */
	void enablePinger(bool enable);

	/**
	 * @brief Set the interval between ICMP ping attempts.
	 * @param intervalMs  Interval in milliseconds (default 60 000).
	 */
	void setPingInterval(unsigned long intervalMs);

	/**
	 * @brief Set the per-ping reply timeout.
	 * @param timeoutMs  Timeout in milliseconds (default 1 000).
	 */
	void setPingTimeout(unsigned long timeoutMs);

	/**
	 * @brief Override the default ping target (gateway) with a custom IP.
	 *
	 * By default the library pings WiFi.gatewayIP().  Use this to ping
	 * a different host — for example a reliable server on your LAN.
	 *
	 * @param target  IP address to ping instead of the gateway.
	 */
	void setPingTarget(IPAddress target);

	///@}

private:
	/// @cond INTERNAL

	/** @brief Internal ICMP ping state machine states. */
	enum class PingState : uint8_t {
		IDLE,       ///< No ping in flight; waiting for next interval.
		SENT        ///< Echo request sent; waiting for reply or timeout.
	};

	void setStatus(WiFiWatchdogStatus newStatus);
	void performFullReset();
	void applyNetworkConfig();

	// Non-blocking ICMP helpers
	void      sendPing();
	void      checkPingReply();
	void      cleanupPing();
	IPAddress getPingTarget();

	/**
	 * @brief lwip raw ICMP receive callback (static).
	 *
	 * Called by lwip for every incoming ICMP packet.  Checks for an
	 * echo reply matching our packet ID and sets _icmpReplyReceived.
	 */
	static u8_t icmpReceiveCallback(void *arg, struct raw_pcb *pcb,
									struct pbuf *p, const ip_addr_t *addr);

	// --- Connection state ---
	const char* _ssid;              ///< WiFi SSID (externally owned).
	const char* _password;          ///< WiFi password (externally owned).
	const char* _hostname;          ///< Device hostname (externally owned).
	bool _debug;                    ///< Runtime debug output flag.

	volatile WiFiWatchdogStatus _status;   ///< Current connection status.
	WiFiStatusCallback _statusCallback;    ///< User-registered status callback.

	// IP configuration
	bool _useStaticIP;              ///< true if static IP is configured.
	IPAddress _staticIP;            ///< Static IP address.
	IPAddress _gateway;             ///< Static gateway address.
	IPAddress _subnet;              ///< Static subnet mask.

	WiFiMode_t _wifiMode;          ///< WiFi operating mode.

	// Reconnection tracking
	unsigned long _lastReconnectAttempt;   ///< millis() of last reconnection attempt.
	unsigned long _reconnectInterval;      ///< Delay between reconnection attempts.
	uint8_t _consecutiveFailures;          ///< Counter for escalation strategy.

	// --- Pinger state ---
	bool          _pingerEnabled;          ///< true if ICMP ping is active.
	unsigned long _pingInterval;           ///< Interval between pings (ms).
	unsigned long _pingTimeout;            ///< Per-ping timeout (ms).
	unsigned long _lastPingTime;           ///< millis() when last ping cycle completed.
	uint8_t       _pingFailCount;          ///< Consecutive ping failures.

	PingState          _pingState;         ///< Current ping state machine state.
	unsigned long      _pingSentTime;      ///< millis() when echo request was sent.
	struct raw_pcb*    _pingPcb;           ///< lwip protocol control block for ICMP.
	volatile bool      _icmpReplyReceived; ///< Flag set by lwip callback on echo reply.

	// Optional custom ping target (overrides gateway)
	bool      _hasPingTarget;              ///< true if a custom target was set.
	IPAddress _pingTarget;                 ///< Custom ping target IP.

	// ESP32 thread-safety
#if defined(ESP32)
	portMUX_TYPE _mutex;                   ///< Spinlock for multi-core safe access.
#endif

	/// @endcond
};

#endif

