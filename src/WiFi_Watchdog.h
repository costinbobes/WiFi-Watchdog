/**
 * @file    WiFi_Watchdog.h
 * @brief   Non-blocking WiFi connection manager with ICMP ping watchdog
 *          for ESP8266 and ESP32.
 * @author  Costin Bobes
 * @date    2/19/2026
 * @version 1.1.0
 * @license MIT
 * @see     https://github.com/costinbobes/WiFi-Watchdog
 *
 * @details
 * WiFi_Watchdog monitors WiFi connectivity via the built-in status API
 * and a non-blocking ICMP echo (ping) to the gateway or a custom target
 * using the lwip raw API.  It automatically reconnects with an escalating
 * strategy (soft reconnect, then full WiFi reset after repeated failures).
 *
 * @note Only one instance of WiFi_Watchdog should exist at a time.
 *       The library enforces this with a runtime check. Creating a second
 *       instance will print a warning to Serial. The ICMP packet ID is
 *       shared, so multiple instances would misattribute echo replies.
 *
 * @note ICMP checksums are not validated on received echo replies.
 *       The library only cares whether a reply was received (indicating
 *       a live connection), not about packet integrity or network quality.
 *
 * ### Features
 * - Connect / disconnect / manual reset
 * - Automatic reconnection with escalating strategy
 * - Non-blocking ICMP ping to gateway (or custom target) via lwip raw API
 * - Status-change callback (DISCONNECTED -> CONNECTING -> CONNECTED -> CONNECTION_LOST -> RECONNECTING)
 * - DHCP or static IP configuration (including DNS server)
 * - Hostname configuration
 * - Configurable thresholds for reconnect failures and ping failures
 * - Runtime debug logging via setDebug(true)
 * - ESP32 multi-core safe (portMUX spinlock + std::atomic for shared state)
 * - Zero external dependencies -- uses lwip raw API directly
 * - All operations fully non-blocking -- no delay() calls
 *
 * ### Quick Start
 * @code
 * WiFi_Watchdog wifi;
 * wifi.setDebug(true);                        // optional: serial debug output
 * wifi.setHostname("my-device");              // optional: set hostname
 * wifi.onStatusChange(myCallback);            // optional: register status callback
 * wifi.setPingInterval(60000);                // optional: default 60 s
 * wifi.connect("SSID", "password");           // connect (copies strings internally)
 * // in loop():
 * wifi.watchdog();                            // call periodically -- fully non-blocking
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
	#include <atomic>
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
#elif defined(ESP32)
	#include <lwip/tcpip.h>
#endif
}

/**
 * @brief WiFi connection status.
 *
 * Passed to the onStatusChange() callback whenever a state transition occurs.
 */
enum class WiFi_WatchdogStatus : uint8_t {
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
typedef void (*WiFiStatusCallback)(WiFi_WatchdogStatus newStatus);

/**
 * @class WiFi_Watchdog
 * @brief Non-blocking WiFi connection manager with ICMP ping watchdog.
 *
 * @note Only one instance should exist at a time. The constructor prints
 *       a warning to Serial if a second instance is created.
 *
 * Call configuration methods (setHostname(), setPingInterval(), etc.)
 * before connect().  Then call watchdog() from loop() on every iteration;
 * it is fully non-blocking and never stalls the main loop.
 *
 * ### Reconnection strategy
 * | Attempt | Action |
 * |---------|--------|
 * | 1 - (maxFailures-1) | `WiFi.reconnect()` (soft) |
 * | maxFailures+         | Full disconnect -> `WiFi.begin()` (hard reset) |
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
	 * - WiFi mode: WIFI_STA (only STA mode is supported)
	 * - DHCP enabled
	 * - Pinger enabled, 60 s interval, 1 s timeout, target = gateway
	 * - Debug output disabled
	 * - Max failures before reset: 5
	 * - Ping fail threshold: 3
	 * - Reconnect interval: 30 s
	 *
	 * @warning Only one instance should exist. A runtime warning is printed
	 *          if a second instance is created.
	 */
	WiFi_Watchdog();

	/**
	 * @brief Destructor -- cleans up ICMP PCB and frees owned strings.
	 */
	~WiFi_Watchdog();

	// Non-copyable, non-movable (prevents dangling lwip callbacks and double-free)
	WiFi_Watchdog(const WiFi_Watchdog&) = delete;
	WiFi_Watchdog& operator=(const WiFi_Watchdog&) = delete;
	WiFi_Watchdog(WiFi_Watchdog&&) = delete;
	WiFi_Watchdog& operator=(WiFi_Watchdog&&) = delete;

	/** @name Connection Management */
	///@{

	/**
	 * @brief Connect to a WiFi network.
	 *
	 * Call configuration methods (setHostname(), setStaticIP(), etc.)
	 * **before** calling this.  Triggers a CONNECTING status transition.
	 *
	 * The SSID and password strings are copied internally, so the caller
	 * does not need to keep them alive after this call returns.
	 *
	 * If already connected, this method disconnects first and reconnects
	 * with the new credentials.
	 *
	 * @param ssid      Network SSID.
	 * @param password  Network password.
	 */
	void connect(const char* ssid, const char* password);

	/**
	 * @brief Disconnect from WiFi and stop all monitoring.
	 *
	 * Cleans up any in-flight ICMP ping and sets status to DISCONNECTED.
	 */
	void disconnect();

	/**
	 * @brief Manual reconnect (non-blocking).
	 *
	 * Disconnects, then after a short internal delay (handled non-blockingly
	 * by the watchdog), re-applies network configuration (hostname, static IP,
	 * DNS) and calls WiFi.begin() again.  Sets status to RECONNECTING.
	 */
	void reset();

	///@}

	/** @name Monitoring */
	///@{

	/**
	 * @brief Non-blocking watchdog -- call from loop().
	 *
	 * On each invocation this method:
	 * 1. Completes any pending non-blocking reset.
	 * 2. Checks WiFi.status() and validates the local IP.
	 * 3. Drives the ICMP ping state machine (send / check reply / timeout).
	 * 4. Triggers automatic reconnection when a disconnect is detected.
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
	 * @return WiFi_WatchdogStatus  Current status value.
	 * @note Thread-safe on ESP32 (uses portMUX spinlock).
	 */
	WiFi_WatchdogStatus getStatus();

	/**
	 * @brief Register a callback for status transitions.
	 *
	 * The callback fires whenever the internal status changes, including
	 * on connection, disconnection, loss, and reconnection attempts.
	 *
	 * @param callback  Function pointer with signature `void(WiFi_WatchdogStatus)`.
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
	 * The string is copied internally.
	 *
	 * @param hostname  Null-terminated hostname string.
	 */
	void setHostname(const char* hostname);

	/**
	 * @brief Use DHCP for IP configuration (this is the default).
	 *
	 * Calling this after setStaticIP() reverts to DHCP.
	 */
	void useDHCP();

	/**
	 * @brief Use a static IP address (without custom DNS).
	 *
	 * Overrides useDHCP().  Call before connect().
	 * DNS server defaults to the gateway address.
	 *
	 * @param ip       Static IP address.
	 * @param gateway  Gateway IP address.
	 * @param subnet   Subnet mask.
	 */
	void setStaticIP(IPAddress ip, IPAddress gateway, IPAddress subnet);

	/**
	 * @brief Use a static IP address with a custom DNS server.
	 *
	 * Overrides useDHCP().  Call before connect().
	 *
	 * @param ip       Static IP address.
	 * @param gateway  Gateway IP address.
	 * @param subnet   Subnet mask.
	 * @param dns      DNS server address.
	 */
	void setStaticIP(IPAddress ip, IPAddress gateway, IPAddress subnet, IPAddress dns);

	/**
	 * @brief Enable or disable runtime serial debug output.
	 *
	 * When enabled, the library prints diagnostic messages to Serial
	 * (e.g. ping results, reconnection attempts, threshold warnings).
	 * Disabled by default.
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
	 * After consecutive timeouts exceeding the ping fail threshold,
	 * a full WiFi reset is triggered.  When disabled, only WiFi.status()
	 * is monitored.
	 *
	 * @param enable  true to enable, false to disable.
	 */
	void enablePinger(bool enable);

	/**
	 * @brief Set the interval between ICMP ping attempts.
	 *
	 * Valid range: 1000 - 86400000 ms (1 second - 1 day).
	 * Values outside this range are clamped. A debug warning is printed
	 * if the value is clamped and debug output is enabled.
	 *
	 * @param intervalMs  Interval in milliseconds (default 60 000).
	 */
	void setPingInterval(unsigned long intervalMs);

	/**
	 * @brief Set the per-ping reply timeout.
	 *
	 * Valid range: 1000 - 30000 ms (1 second - 30 seconds).
	 * Values outside this range are clamped. A debug warning is printed
	 * if the value is clamped and debug output is enabled.
	 *
	 * @param timeoutMs  Timeout in milliseconds (default 1 000).
	 */
	void setPingTimeout(unsigned long timeoutMs);

	/**
	 * @brief Override the default ping target (gateway) with a custom IP.
	 *
	 * By default the library pings WiFi.gatewayIP().  Use this to ping
	 * a different host -- for example a reliable server on your LAN.
	 *
	 * @param target  IP address to ping instead of the gateway.
	 */
	void setPingTarget(IPAddress target);

	/**
	 * @brief Clear a custom ping target, reverting to the default gateway.
	 */
	void clearPingTarget();

	///@}

	/** @name Threshold Configuration */
	///@{

	/**
	 * @brief Set the number of consecutive WiFi reconnect failures before
	 *        a full WiFi reset is performed.
	 *
	 * Valid range: 1 - 20. Values outside this range are clamped.
	 * A debug warning is printed if the value is clamped.
	 *
	 * @param count  Number of failures (default 5).
	 */
	void setMaxFailuresBeforeReset(uint8_t count);

	/**
	 * @brief Set the number of consecutive ICMP ping failures before
	 *        triggering a WiFi reset.
	 *
	 * Valid range: 1 - 20. Values outside this range are clamped.
	 * A debug warning is printed if the value is clamped.
	 *
	 * @param count  Number of failures (default 3).
	 */
	void setPingFailThreshold(uint8_t count);

	/**
	 * @brief Set the interval between automatic reconnection attempts.
	 *
	 * Valid range: 1000 - 300000 ms (1 second - 5 minutes).
	 * Values outside this range are clamped. A debug warning is printed
	 * if the value is clamped.
	 *
	 * @param intervalMs  Interval in milliseconds (default 30 000).
	 */
	void setReconnectInterval(unsigned long intervalMs);

	///@}

private:
	/// @cond INTERNAL

	/** @brief Internal ICMP ping state machine states. */
	enum class PingState : uint8_t {
		IDLE,       ///< No ping in flight; waiting for next interval.
		SENT        ///< Echo request sent; waiting for reply or timeout.
	};

	/** @brief Internal reset phase for non-blocking reset. */
	enum class ResetPhase : uint8_t {
		NONE,                       ///< No reset in progress.
		WAITING_AFTER_DISCONNECT    ///< Waiting for post-disconnect delay to elapse.
	};

	void setStatus(WiFi_WatchdogStatus newStatus);
	void performFullReset();
	void applyNetworkConfig();
	void freeString(char*& ptr);
	void copyString(char*& dest, const char* src);

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
	 *
	 * @note ICMP checksums are not validated. A received echo reply
	 *       indicates a live connection -- packet integrity is not
	 *       the concern of this connectivity watchdog.
	 */
	static u8_t icmpReceiveCallback(void *arg, struct raw_pcb *pcb,
									struct pbuf *p, const ip_addr_t *addr);

	// --- Singleton tracking ---
	static bool _instanceExists;

	// --- Connection state ---
	char* _ssid;                    ///< WiFi SSID (owned copy).
	char* _password;                ///< WiFi password (owned copy).
	char* _hostname;                ///< Device hostname (owned copy, or null for auto).
	bool _debug;                    ///< Runtime debug output flag.

	volatile WiFi_WatchdogStatus _status;   ///< Current connection status.
	WiFiStatusCallback _statusCallback;     ///< User-registered status callback.

	// IP configuration
	bool _useStaticIP;              ///< true if static IP is configured.
	IPAddress _staticIP;            ///< Static IP address.
	IPAddress _gateway;             ///< Static gateway address.
	IPAddress _subnet;              ///< Static subnet mask.
	IPAddress _dns;                 ///< DNS server address.
	bool _hasDns;                   ///< true if a custom DNS was set via setStaticIP(ip,gw,sn,dns).

	// Non-blocking reset
	ResetPhase    _resetPhase;      ///< Current reset phase.
	unsigned long _resetStartTime;  ///< millis() when reset disconnect was issued.

	// Reconnection tracking
	unsigned long _lastReconnectAttempt;   ///< millis() of last reconnection attempt.
	unsigned long _reconnectInterval;      ///< Delay between reconnection attempts.
	unsigned long _lastWatchdogRun;        ///< millis() of last watchdog() execution.
	uint8_t _consecutiveFailures;          ///< Counter for escalation strategy.
	uint8_t _maxFailuresBeforeReset;       ///< Configurable threshold (default 5).

	// --- Pinger state ---
	bool          _pingerEnabled;          ///< true if ICMP ping is active.
	unsigned long _pingInterval;           ///< Interval between pings (ms).
	unsigned long _pingTimeout;            ///< Per-ping timeout (ms).
	unsigned long _lastPingTime;           ///< millis() when last ping cycle completed.
	uint8_t       _pingFailCount;          ///< Consecutive ping failures.
	uint8_t       _pingFailThreshold;      ///< Configurable threshold (default 3).

	PingState          _pingState;         ///< Current ping state machine state.
	unsigned long      _pingSentTime;      ///< millis() when echo request was sent.
	struct raw_pcb*    _pingPcb;           ///< lwip protocol control block for ICMP (reused across pings).

#if defined(ESP32)
	std::atomic<bool>  _icmpReplyReceived; ///< Flag set by lwip callback on echo reply (atomic for multi-core safety).
#else
	volatile bool      _icmpReplyReceived; ///< Flag set by lwip callback on echo reply.
#endif

	u16_t              _pingSeqNo;         ///< Incrementing ICMP sequence number.

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
