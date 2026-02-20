/*
 Name:		WiFi_Watchdog.h
 Created:	2/19/2026 7:44:20 PM
 Author:	Costin
 Editor:	http://www.visualmicro.com
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

// --- WiFi connection status enum ---
enum class WiFiWatchdogStatus : uint8_t {
	DISCONNECTED,
	CONNECTING,
	CONNECTED,
	CONNECTION_LOST,
	RECONNECTING
};

// --- Callback type ---
typedef void (*WiFiStatusCallback)(WiFiWatchdogStatus newStatus);

// --- Main class ---
class WiFi_Watchdog {
public:
	WiFi_Watchdog();

	// Connection
	void connect(const char* ssid, const char* password);
	void disconnect();
	void reset();

	// Monitoring – call from loop()
	void watchdog();

	// Status
	WiFiWatchdogStatus getStatus();
	void onStatusChange(WiFiStatusCallback callback);

	// Network info
	String getIPAddress();
	int32_t getSignalStrength();
	String getMACAddress();

	// Configuration (call before connect)
	void setHostname(const char* hostname);
	void useDHCP();
	void setStaticIP(IPAddress ip, IPAddress gateway, IPAddress subnet);
	void setWiFiMode(WiFiMode_t mode);
	void setDebug(bool enable);

	// Pinger configuration
	void enablePinger(bool enable);
	void setPingInterval(unsigned long intervalMs);
	void setPingTimeout(unsigned long timeoutMs);
	void setPingTarget(IPAddress target);

private:
	// --- ICMP ping state machine ---
	enum class PingState : uint8_t {
		IDLE,       // no ping in flight
		SENT        // echo request sent, waiting for reply
	};

	void setStatus(WiFiWatchdogStatus newStatus);
	void performFullReset();
	void applyNetworkConfig();

	// Non-blocking ICMP helpers
	void     sendPing();
	void     checkPingReply();
	void     cleanupPing();
	IPAddress getPingTarget();

	// lwip raw callback (static, sets _icmpReplyReceived flag)
	static u8_t icmpReceiveCallback(void *arg, struct raw_pcb *pcb,
									struct pbuf *p, const ip_addr_t *addr);

	// --- Connection state ---
	const char* _ssid;
	const char* _password;
	const char* _hostname;
	bool _debug;

	volatile WiFiWatchdogStatus _status;
	WiFiStatusCallback _statusCallback;

	// IP configuration
	bool _useStaticIP;
	IPAddress _staticIP;
	IPAddress _gateway;
	IPAddress _subnet;

	// WiFi mode
	WiFiMode_t _wifiMode;

	// Reconnection tracking
	unsigned long _lastReconnectAttempt;
	unsigned long _reconnectInterval;
	uint8_t _consecutiveFailures;

	// --- Pinger state ---
	bool          _pingerEnabled;
	unsigned long _pingInterval;
	unsigned long _pingTimeout;
	unsigned long _lastPingTime;
	uint8_t       _pingFailCount;

	PingState          _pingState;
	unsigned long      _pingSentTime;
	struct raw_pcb*    _pingPcb;
	volatile bool      _icmpReplyReceived;

	// Optional custom ping target (overrides gateway)
	bool      _hasPingTarget;
	IPAddress _pingTarget;

	// ESP32 thread-safety
#if defined(ESP32)
	portMUX_TYPE _mutex;
#endif
};

#endif

