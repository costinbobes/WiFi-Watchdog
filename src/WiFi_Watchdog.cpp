/*
 Name:		WiFi_Watchdog.cpp
 Created:	2/19/2026 7:44:20 PM
 Author:	Costin
 Editor:	http://www.visualmicro.com
*/

#include "WiFi_Watchdog.h"

// --- Debug logging helpers (runtime, works across translation units) ---
#define WD_LOG(...)    do { if (_debug) Serial.print(__VA_ARGS__); } while(0)
#define WD_LOGLN(...)  do { if (_debug) Serial.println(__VA_ARGS__); } while(0)

// --- Defaults ---
static const unsigned long DEFAULT_RECONNECT_INTERVAL = 30000;  // 30 seconds
static const unsigned long DEFAULT_PING_INTERVAL      = 60000;  // 60 seconds
static const unsigned long DEFAULT_PING_TIMEOUT       = 1000;   // 1 second
static const uint8_t      MAX_FAILURES_BEFORE_RESET   = 5;
static const uint8_t      PING_FAIL_THRESHOLD         = 3;
static const u16_t        PING_PACKET_ID              = 0xABCD;

// =====================================================================
// lwip ICMP receive callback (static)
//   Called by lwip when any ICMP packet arrives.
//   If it is an echo reply matching our ID, set the flag and consume it.
// =====================================================================
u8_t WiFi_Watchdog::icmpReceiveCallback(void *arg, struct raw_pcb *pcb,
										struct pbuf *p, const ip_addr_t *addr) {
	WiFi_Watchdog *self = (WiFi_Watchdog *)arg;

	if (p->tot_len < (PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr))) {
		return 0; // too small, not consumed
	}

	// Skip past IP header to reach ICMP header
	if (pbuf_header(p, -(s16_t)PBUF_IP_HLEN) != 0) {
		return 0;
	}

	struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)p->payload;

	// Check: is this an echo reply with our packet ID?
	if (ICMPH_TYPE(iecho) == ICMP_ER && iecho->id == htons(PING_PACKET_ID)) {
		self->_icmpReplyReceived = true;
		pbuf_header(p, (s16_t)PBUF_IP_HLEN); // restore offset
		pbuf_free(p);
		return 1; // consumed
	}

	// Not ours — restore offset and let lwip pass it on
	pbuf_header(p, (s16_t)PBUF_IP_HLEN);
	return 0;
}

// =====================================================================
// Constructor
// =====================================================================
WiFi_Watchdog::WiFi_Watchdog()
	: _ssid(nullptr)
	, _password(nullptr)
	, _hostname(nullptr)
	, _debug(false)
	, _status(WiFiWatchdogStatus::DISCONNECTED)
	, _statusCallback(nullptr)
	, _useStaticIP(false)
	, _wifiMode(WIFI_STA)
	, _lastReconnectAttempt(0)
	, _reconnectInterval(DEFAULT_RECONNECT_INTERVAL)
	, _consecutiveFailures(0)
	, _pingerEnabled(true)
	, _pingInterval(DEFAULT_PING_INTERVAL)
	, _pingTimeout(DEFAULT_PING_TIMEOUT)
	, _lastPingTime(0)
	, _pingFailCount(0)
	, _pingState(PingState::IDLE)
	, _pingSentTime(0)
	, _pingPcb(nullptr)
	, _icmpReplyReceived(false)
	, _hasPingTarget(false)
{
#if defined(ESP32)
	_mutex = portMUX_INITIALIZER_UNLOCKED;
#endif
}

// =====================================================================
// Connection
// =====================================================================
void WiFi_Watchdog::connect(const char* ssid, const char* password) {
	_ssid     = ssid;
	_password = password;

	WiFi.mode(_wifiMode);
	applyNetworkConfig();
	WiFi.begin(_ssid, _password);

	setStatus(WiFiWatchdogStatus::CONNECTING);
}

void WiFi_Watchdog::disconnect() {
	cleanupPing();
	WiFi.disconnect(true);
	setStatus(WiFiWatchdogStatus::DISCONNECTED);
	_consecutiveFailures = 0;
	_pingFailCount = 0;
}

void WiFi_Watchdog::reset() {
	cleanupPing();
	WiFi.disconnect(true);
	delay(100);
	yield();

	applyNetworkConfig();
	WiFi.begin(_ssid, _password);

	setStatus(WiFiWatchdogStatus::RECONNECTING);
	_consecutiveFailures = 0;
	_pingFailCount = 0;
}

// =====================================================================
// Watchdog – call from loop(), fully non-blocking
// =====================================================================
void WiFi_Watchdog::watchdog() {
	yield();

	wl_status_t wifiStatus = WiFi.status();
	IPAddress   localIP    = WiFi.localIP();
	bool        hasValidIP = (localIP[0] != 0);

	// --- Connected with valid IP ---
	if (wifiStatus == WL_CONNECTED && hasValidIP) {
		if (_status != WiFiWatchdogStatus::CONNECTED) {
			_consecutiveFailures = 0;
			_pingFailCount       = 0;
			_lastPingTime        = 0; // fire first ping immediately
			setStatus(WiFiWatchdogStatus::CONNECTED);
		}
		_lastReconnectAttempt = millis();

		// --- Non-blocking ICMP ping state machine ---
		if (_pingerEnabled) {
			switch (_pingState) {

			case PingState::IDLE:
				// Time to send next ping?
				if (millis() - _lastPingTime >= _pingInterval) {
					sendPing();
				}
				break;

			case PingState::SENT:
				// Check for reply or timeout
				checkPingReply();
				break;
			}
		}

		return;
	}

	// If a ping was in flight and we lost WiFi, clean it up
	cleanupPing();

	// --- Associated but no IP (DHCP issue) ---
	if (wifiStatus == WL_CONNECTED && !hasValidIP) {
		WiFi.disconnect(false);
		delay(100);
		yield();
		return;
	}

	// --- Not connected ---
	if (_status == WiFiWatchdogStatus::CONNECTED) {
		setStatus(WiFiWatchdogStatus::CONNECTION_LOST);
		_lastReconnectAttempt = 0; // trigger immediate reconnect attempt
	}

	unsigned long now = millis();
	if (now - _lastReconnectAttempt >= _reconnectInterval) {
		WD_LOGLN(F("[WiFi Watchdog] Attempting reconnection..."));
		_lastReconnectAttempt = now;
		_consecutiveFailures++;

		if (_status != WiFiWatchdogStatus::RECONNECTING) {
			setStatus(WiFiWatchdogStatus::RECONNECTING);
		}

		if (_consecutiveFailures >= MAX_FAILURES_BEFORE_RESET) {
			WD_LOGLN(F("[WiFi Watchdog] Maximum failures reached, performing full reset..."));
			performFullReset();
		} else {
			WD_LOGLN(F("[WiFi Watchdog] Reconnecting..."));
			WiFi.reconnect();
		}
	}
}

// =====================================================================
// Status
// =====================================================================
WiFiWatchdogStatus WiFi_Watchdog::getStatus() {
#if defined(ESP32)
	portENTER_CRITICAL(&_mutex);
	WiFiWatchdogStatus s = _status;
	portEXIT_CRITICAL(&_mutex);
	return s;
#else
	return _status;
#endif
}

void WiFi_Watchdog::onStatusChange(WiFiStatusCallback callback) {
	_statusCallback = callback;
}

// =====================================================================
// Network info
// =====================================================================
String WiFi_Watchdog::getIPAddress() {
	return WiFi.localIP().toString();
}

int32_t WiFi_Watchdog::getSignalStrength() {
	return WiFi.RSSI();
}

String WiFi_Watchdog::getMACAddress() {
	return WiFi.macAddress();
}

// =====================================================================
// Configuration – call before connect()
// =====================================================================
void WiFi_Watchdog::setHostname(const char* hostname) {
	_hostname = hostname;
}

void WiFi_Watchdog::useDHCP() {
	_useStaticIP = false;
}

void WiFi_Watchdog::setStaticIP(IPAddress ip, IPAddress gateway, IPAddress subnet) {
	_useStaticIP = true;
	_staticIP    = ip;
	_gateway     = gateway;
	_subnet      = subnet;
}

void WiFi_Watchdog::setWiFiMode(WiFiMode_t mode) {
	_wifiMode = mode;
}

void WiFi_Watchdog::setDebug(bool enable) {
	_debug = enable;
}

// =====================================================================
// Pinger configuration
// =====================================================================
void WiFi_Watchdog::enablePinger(bool enable) {
	_pingerEnabled = enable;
	if (!enable) {
		cleanupPing();
	}
}

void WiFi_Watchdog::setPingInterval(unsigned long intervalMs) {
	_pingInterval = intervalMs;
}

void WiFi_Watchdog::setPingTimeout(unsigned long timeoutMs) {
	_pingTimeout = timeoutMs;
}

void WiFi_Watchdog::setPingTarget(IPAddress target) {
	_pingTarget    = target;
	_hasPingTarget = true;
}

// =====================================================================
// Private helpers
// =====================================================================
void WiFi_Watchdog::setStatus(WiFiWatchdogStatus newStatus) {
	WiFiWatchdogStatus oldStatus;

#if defined(ESP32)
	portENTER_CRITICAL(&_mutex);
#endif

	oldStatus = _status;
	if (oldStatus != newStatus) {
		_status = newStatus;
	}

#if defined(ESP32)
	portEXIT_CRITICAL(&_mutex);
#endif

	// Fire callback outside critical section so it can do real work
	if (oldStatus != newStatus && _statusCallback != nullptr) {
		_statusCallback(newStatus);
	}
}

void WiFi_Watchdog::performFullReset() {
	cleanupPing();
	WiFi.disconnect(true);
	delay(100);
	yield();
	WiFi.begin(_ssid, _password);
	_consecutiveFailures = 0;
}

void WiFi_Watchdog::applyNetworkConfig() {
	if (_hostname != nullptr) {
#if defined(ESP8266)
		WiFi.hostname(_hostname);
#elif defined(ESP32)
		WiFi.setHostname(_hostname);
#endif
	}

	if (_useStaticIP) {
		WiFi.config(_staticIP, _gateway, _subnet);
	}
}

// =====================================================================
// Returns the IP to ping: user-supplied target, or gateway
// =====================================================================
IPAddress WiFi_Watchdog::getPingTarget() {
	if (_hasPingTarget) {
		return _pingTarget;
	}
	return WiFi.gatewayIP();
}

// =====================================================================
// Non-blocking ICMP: dispatch echo request, return immediately
// =====================================================================
void WiFi_Watchdog::sendPing() {
	IPAddress target = getPingTarget();

	// Validate target
	if (target[0] == 0 && target[1] == 0 && target[2] == 0 && target[3] == 0) {
		WD_LOGLN(F("[WiFi Watchdog] No valid ping target"));
		_lastPingTime = millis();
		return;
	}

	WD_LOG(F("[WiFi Watchdog] ICMP ping -> "));
	WD_LOGLN(target);

	_icmpReplyReceived = false;

	// Create raw PCB for ICMP
	_pingPcb = raw_new(IP_PROTO_ICMP);
	if (_pingPcb == nullptr) {
		WD_LOGLN(F("[WiFi Watchdog] Failed to create ICMP PCB"));
		_lastPingTime = millis();
		return;
	}

	// Register receive callback, pass 'this' so callback can set our flag
	raw_recv(_pingPcb, icmpReceiveCallback, this);

	// Allocate packet buffer for ICMP echo request
	u16_t packetSize = sizeof(struct icmp_echo_hdr);
	struct pbuf *p = pbuf_alloc(PBUF_IP, packetSize, PBUF_RAM);
	if (p == nullptr) {
		raw_remove(_pingPcb);
		_pingPcb = nullptr;
		WD_LOGLN(F("[WiFi Watchdog] Failed to allocate ICMP packet"));
		_lastPingTime = millis();
		return;
	}

	// Build ICMP echo request
	struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)p->payload;
	ICMPH_TYPE_SET(iecho, ICMP_ECHO);
	ICMPH_CODE_SET(iecho, 0);
	iecho->chksum = 0;
	iecho->id     = htons(PING_PACKET_ID);
	iecho->seqno  = htons(1);
	iecho->chksum = inet_chksum(iecho, packetSize);

	// Send
	ip_addr_t dest;
	dest.addr = (uint32_t)target;
	raw_sendto(_pingPcb, p, &dest);
	pbuf_free(p);

	// Transition to SENT state — return to loop immediately
	_pingSentTime = millis();
	_pingState    = PingState::SENT;
}

// =====================================================================
// Non-blocking ICMP: check if reply arrived or timeout expired
// =====================================================================
void WiFi_Watchdog::checkPingReply() {
	// Reply received?
	if (_icmpReplyReceived) {
		WD_LOGLN(F("[WiFi Watchdog] ICMP reply OK"));
		_pingFailCount = 0;
		cleanupPing();
		_lastPingTime = millis();
		return;
	}

	// Timeout expired?
	if (millis() - _pingSentTime >= _pingTimeout) {
		WD_LOGLN(F("[WiFi Watchdog] ICMP timeout"));
		cleanupPing();
		_lastPingTime = millis();

		_pingFailCount++;
		if (_pingFailCount >= PING_FAIL_THRESHOLD) {
			_pingFailCount = 0;
			WD_LOGLN(F("[WiFi Watchdog] Gateway unreachable, resetting WiFi..."));
			performFullReset();
		}
		return;
	}

	// Still waiting — do nothing, return to loop
}

// =====================================================================
// Cleanup any in-flight ping resources
// =====================================================================
void WiFi_Watchdog::cleanupPing() {
	if (_pingPcb != nullptr) {
		raw_remove(_pingPcb);
		_pingPcb = nullptr;
	}
	_pingState = PingState::IDLE;
}


