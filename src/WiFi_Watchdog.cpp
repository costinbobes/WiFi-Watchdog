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
static const uint8_t      DEFAULT_MAX_FAILURES        = 5;
static const uint8_t      DEFAULT_PING_FAIL_THRESHOLD = 3;
static const u16_t        PING_PACKET_ID              = 0xABCD;

// --- Validation ranges ---
static const unsigned long MIN_PING_INTERVAL      = 1000;       // 1 second
static const unsigned long MAX_PING_INTERVAL      = 86400000UL; // 1 day
static const unsigned long MIN_PING_TIMEOUT       = 1000;       // 1 second
static const unsigned long MAX_PING_TIMEOUT       = 30000;      // 30 seconds
static const uint8_t      MIN_THRESHOLD           = 1;
static const uint8_t      MAX_THRESHOLD           = 20;
static const unsigned long MIN_RECONNECT_INTERVAL = 1000;       // 1 second
static const unsigned long MAX_RECONNECT_INTERVAL = 300000;     // 5 minutes
static const unsigned long RESET_DELAY_MS         = 100;        // post-disconnect delay

// --- Singleton tracking ---
bool WiFi_Watchdog::_instanceExists = false;

// =====================================================================
// lwip ICMP receive callback (static)
//   Called by lwip when any ICMP packet arrives.
//   If it is an echo reply matching our ID, set the flag and consume it.
//   NOTE: ICMP checksum is not validated — a received reply indicates
//   a live connection; packet integrity is not the concern of this
//   connectivity watchdog.
// =====================================================================
u8_t WiFi_Watchdog::icmpReceiveCallback(void *arg, struct raw_pcb *pcb,
										struct pbuf *p, const ip_addr_t *addr) {
	WiFi_Watchdog *self = (WiFi_Watchdog *)arg;

	// Need at least a minimal IP header (20) + ICMP echo header (8)
	if (p->tot_len < 28) {
		return 0;
	}

	// Read actual IPv4 header length from byte 0 (IHL field, lower nibble, in 32-bit words)
	u8_t ip_ver_ihl;
	pbuf_copy_partial(p, &ip_ver_ihl, 1, 0);
	u16_t ip_hdr_len = (ip_ver_ihl & 0x0F) * 4;

	// Read the ICMP header at offset ip_hdr_len (without modifying the pbuf)
	struct icmp_echo_hdr iecho;
	if (pbuf_copy_partial(p, &iecho, sizeof(iecho), ip_hdr_len) != sizeof(iecho)) {
		return 0;
	}

	// Check: is this an echo reply with our packet ID?
	if (ICMPH_TYPE(&iecho) == ICMP_ER && iecho.id == htons(PING_PACKET_ID)) {
		self->_icmpReplyReceived = true;
		pbuf_free(p);
		return 1; // consumed
	}

	return 0; // not ours, let lwip pass it on
}

// =====================================================================
// Constructor
// =====================================================================
WiFi_Watchdog::WiFi_Watchdog()
	: _ssid(nullptr)
	, _password(nullptr)
	, _hostname(nullptr)
	, _debug(false)
	, _status(WiFi_WatchdogStatus::DISCONNECTED)
	, _statusCallback(nullptr)
	, _useStaticIP(false)
	, _hasDns(false)
	, _resetPhase(ResetPhase::NONE)
	, _resetStartTime(0)
	, _lastReconnectAttempt(0)
	, _reconnectInterval(DEFAULT_RECONNECT_INTERVAL)
	, _lastWatchdogRun(0)
	, _consecutiveFailures(0)
	, _maxFailuresBeforeReset(DEFAULT_MAX_FAILURES)
	, _pingerEnabled(true)
	, _pingInterval(DEFAULT_PING_INTERVAL)
	, _pingTimeout(DEFAULT_PING_TIMEOUT)
	, _lastPingTime(0)
	, _pingFailCount(0)
	, _pingFailThreshold(DEFAULT_PING_FAIL_THRESHOLD)
	, _pingState(PingState::IDLE)
	, _pingSentTime(0)
	, _pingPcb(nullptr)
	, _icmpReplyReceived(false)
	, _pingSeqNo(0)
	, _hasPingTarget(false)
{
#if defined(ESP32)
	_mutex = portMUX_INITIALIZER_UNLOCKED;
#endif

	if (_instanceExists) {
		Serial.println(F("[WiFi Watchdog] WARNING: Only one WiFi_Watchdog instance should exist!"));
	}
	_instanceExists = true;
}

// =====================================================================
// Destructor
// =====================================================================
WiFi_Watchdog::~WiFi_Watchdog() {
	cleanupPing();
	freeString(_ssid);
	freeString(_password);
	freeString(_hostname);
	_instanceExists = false;
}

// =====================================================================
// String helpers
// =====================================================================
void WiFi_Watchdog::freeString(char*& ptr) {
	if (ptr != nullptr) {
		free(ptr);
		ptr = nullptr;
	}
}

void WiFi_Watchdog::copyString(char*& dest, const char* src) {
	freeString(dest);
	dest = (src != nullptr) ? strdup(src) : nullptr;
}

// =====================================================================
// Connection
// =====================================================================
void WiFi_Watchdog::connect(const char* ssid, const char* password) {
	// If already connected or connecting, disconnect first
	if (_status != WiFi_WatchdogStatus::DISCONNECTED) {
		cleanupPing();
		WiFi.disconnect(true);
		_resetPhase = ResetPhase::NONE;
	}

	copyString(_ssid, ssid);
	copyString(_password, password);

	_consecutiveFailures = 0;
	_pingFailCount = 0;

	WiFi.mode(WIFI_STA);
	applyNetworkConfig();
	WiFi.begin(_ssid, _password);

	setStatus(WiFi_WatchdogStatus::CONNECTING);
}

void WiFi_Watchdog::disconnect() {
	_resetPhase = ResetPhase::NONE;
	cleanupPing();
	WiFi.disconnect(true);
	setStatus(WiFi_WatchdogStatus::DISCONNECTED);
	_consecutiveFailures = 0;
	_pingFailCount = 0;
}

void WiFi_Watchdog::reset() {
	cleanupPing();
	WiFi.disconnect(true);

	// Non-blocking: enter waiting phase, watchdog() will complete the reset
	_resetPhase = ResetPhase::WAITING_AFTER_DISCONNECT;
	_resetStartTime = millis();

	setStatus(WiFi_WatchdogStatus::RECONNECTING);
	_consecutiveFailures = 0;
	_pingFailCount = 0;
}

// =====================================================================
// Watchdog – call from loop(), fully non-blocking
// =====================================================================
void WiFi_Watchdog::watchdog() {
	// Throttle: skip if called again within 100 ms
	uint32_t now = millis();
	if (now - _lastWatchdogRun < 100) {
		return;
	}
	_lastWatchdogRun = now;

	yield();

	// --- Handle pending non-blocking reset ---
	if (_resetPhase == ResetPhase::WAITING_AFTER_DISCONNECT) {
		if (now - _resetStartTime >= RESET_DELAY_MS) {
			_resetPhase = ResetPhase::NONE;
			applyNetworkConfig();
			WiFi.begin(_ssid, _password);
		}
		return;
	}

	wl_status_t wifiStatus = WiFi.status();
	IPAddress   localIP    = WiFi.localIP();
	bool        hasValidIP = (localIP[0] != 0);

	// Reject APIPA / link-local addresses (169.254.x.x) — not a real connection
	if (hasValidIP && localIP[0] == 169 && localIP[1] == 254) {
		hasValidIP = false;
	}

	// --- Connected with valid IP ---
	if (wifiStatus == WL_CONNECTED && hasValidIP) {
		if (_status != WiFi_WatchdogStatus::CONNECTED) {
			_consecutiveFailures = 0;
			_pingFailCount       = 0;
			_lastPingTime        = 0; // fire first ping immediately
			setStatus(WiFi_WatchdogStatus::CONNECTED);
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
	// Disconnect without erasing config; the 100ms watchdog throttle
	// provides sufficient delay before the next check
	if (wifiStatus == WL_CONNECTED && !hasValidIP) {
		WiFi.disconnect(false);
		return;
	}

	// --- Not connected ---
	if (_status == WiFi_WatchdogStatus::CONNECTED) {
		setStatus(WiFi_WatchdogStatus::CONNECTION_LOST);
		_lastReconnectAttempt = 0; // trigger immediate reconnect attempt
	}

	if (now - _lastReconnectAttempt >= _reconnectInterval) {
		WD_LOGLN(F("[WiFi Watchdog] Attempting reconnection..."));
		_lastReconnectAttempt = now;
		_consecutiveFailures++;

		if (_status != WiFi_WatchdogStatus::RECONNECTING) {
			setStatus(WiFi_WatchdogStatus::RECONNECTING);
		}

		if (_consecutiveFailures >= _maxFailuresBeforeReset) {
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
WiFi_WatchdogStatus WiFi_Watchdog::getStatus() {
#if defined(ESP32)
	portENTER_CRITICAL(&_mutex);
	WiFi_WatchdogStatus s = _status;
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
	copyString(_hostname, hostname);
}

void WiFi_Watchdog::useDHCP() {
	_useStaticIP = false;
}

void WiFi_Watchdog::setStaticIP(IPAddress ip, IPAddress gateway, IPAddress subnet) {
	_useStaticIP = true;
	_staticIP    = ip;
	_gateway     = gateway;
	_subnet      = subnet;
	_hasDns      = false;
}

void WiFi_Watchdog::setStaticIP(IPAddress ip, IPAddress gateway, IPAddress subnet, IPAddress dns) {
	_useStaticIP = true;
	_staticIP    = ip;
	_gateway     = gateway;
	_subnet      = subnet;
	_dns         = dns;
	_hasDns      = true;
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
	if (intervalMs < MIN_PING_INTERVAL || intervalMs > MAX_PING_INTERVAL) {
		WD_LOG(F("[WiFi Watchdog] setPingInterval: clamping "));
		WD_LOG(intervalMs);
		WD_LOG(F(" to range "));
		WD_LOG(MIN_PING_INTERVAL);
		WD_LOG(F(".."));
		WD_LOGLN(MAX_PING_INTERVAL);
	}
	_pingInterval = constrain(intervalMs, MIN_PING_INTERVAL, MAX_PING_INTERVAL);
}

void WiFi_Watchdog::setPingTimeout(unsigned long timeoutMs) {
	if (timeoutMs < MIN_PING_TIMEOUT || timeoutMs > MAX_PING_TIMEOUT) {
		WD_LOG(F("[WiFi Watchdog] setPingTimeout: clamping "));
		WD_LOG(timeoutMs);
		WD_LOG(F(" to range "));
		WD_LOG(MIN_PING_TIMEOUT);
		WD_LOG(F(".."));
		WD_LOGLN(MAX_PING_TIMEOUT);
	}
	_pingTimeout = constrain(timeoutMs, MIN_PING_TIMEOUT, MAX_PING_TIMEOUT);
}

void WiFi_Watchdog::setPingTarget(IPAddress target) {
	_pingTarget    = target;
	_hasPingTarget = true;
}

void WiFi_Watchdog::clearPingTarget() {
	_hasPingTarget = false;
}

// =====================================================================
// Threshold configuration
// =====================================================================
void WiFi_Watchdog::setMaxFailuresBeforeReset(uint8_t count) {
	if (count < MIN_THRESHOLD || count > MAX_THRESHOLD) {
		WD_LOG(F("[WiFi Watchdog] setMaxFailuresBeforeReset: clamping "));
		WD_LOG(count);
		WD_LOG(F(" to range "));
		WD_LOG(MIN_THRESHOLD);
		WD_LOG(F(".."));
		WD_LOGLN(MAX_THRESHOLD);
	}
	_maxFailuresBeforeReset = constrain(count, MIN_THRESHOLD, MAX_THRESHOLD);
}

void WiFi_Watchdog::setPingFailThreshold(uint8_t count) {
	if (count < MIN_THRESHOLD || count > MAX_THRESHOLD) {
		WD_LOG(F("[WiFi Watchdog] setPingFailThreshold: clamping "));
		WD_LOG(count);
		WD_LOG(F(" to range "));
		WD_LOG(MIN_THRESHOLD);
		WD_LOG(F(".."));
		WD_LOGLN(MAX_THRESHOLD);
	}
	_pingFailThreshold = constrain(count, MIN_THRESHOLD, MAX_THRESHOLD);
}

void WiFi_Watchdog::setReconnectInterval(unsigned long intervalMs) {
	if (intervalMs < MIN_RECONNECT_INTERVAL || intervalMs > MAX_RECONNECT_INTERVAL) {
		WD_LOG(F("[WiFi Watchdog] setReconnectInterval: clamping "));
		WD_LOG(intervalMs);
		WD_LOG(F(" to range "));
		WD_LOG(MIN_RECONNECT_INTERVAL);
		WD_LOG(F(".."));
		WD_LOGLN(MAX_RECONNECT_INTERVAL);
	}
	_reconnectInterval = constrain(intervalMs, MIN_RECONNECT_INTERVAL, MAX_RECONNECT_INTERVAL);
}

// =====================================================================
// Private helpers
// =====================================================================
void WiFi_Watchdog::setStatus(WiFi_WatchdogStatus newStatus) {
	WiFi_WatchdogStatus oldStatus;

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

	// Non-blocking: enter waiting phase, watchdog() will complete the reset
	// (applyNetworkConfig + WiFi.begin are called when the phase completes)
	_resetPhase = ResetPhase::WAITING_AFTER_DISCONNECT;
	_resetStartTime = millis();
	_consecutiveFailures = 0;
}

void WiFi_Watchdog::applyNetworkConfig() {
	// If no hostname was set, generate one from last 6 hex chars of MAC
	if (_hostname == nullptr) {
		String mac = WiFi.macAddress();    // "AA:BB:CC:DD:EE:FF"
		char autoName[13];                 // "ESP_DDEEFF\0"
		snprintf(autoName, sizeof(autoName), "ESP_%c%c%c%c%c%c",
			mac[9], mac[10], mac[12], mac[13], mac[15], mac[16]);
		copyString(_hostname, autoName);
	}

#if defined(ESP8266)
	WiFi.hostname(_hostname);
#elif defined(ESP32)
	WiFi.setHostname(_hostname);
#endif

	if (_useStaticIP) {
		if (_hasDns) {
			WiFi.config(_staticIP, _gateway, _subnet, _dns);
		} else {
			WiFi.config(_staticIP, _gateway, _subnet);
		}
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

#if defined(ESP32)
	LOCK_TCPIP_CORE();
#endif

	// Create raw PCB for ICMP if not already allocated (reused across pings)
	if (_pingPcb == nullptr) {
		_pingPcb = raw_new(IP_PROTO_ICMP);
		if (_pingPcb == nullptr) {
#if defined(ESP32)
			UNLOCK_TCPIP_CORE();
#endif
			WD_LOGLN(F("[WiFi Watchdog] Failed to create ICMP PCB"));
			_lastPingTime = millis();
			return;
		}

		// Register receive callback, pass 'this' so callback can set our flag
		raw_recv(_pingPcb, icmpReceiveCallback, this);
	}

	// Allocate packet buffer for ICMP echo request
	u16_t packetSize = sizeof(struct icmp_echo_hdr);
	struct pbuf *p = pbuf_alloc(PBUF_IP, packetSize, PBUF_RAM);
	if (p == nullptr) {
#if defined(ESP32)
		UNLOCK_TCPIP_CORE();
#endif
		WD_LOGLN(F("[WiFi Watchdog] Failed to allocate ICMP packet"));
		_lastPingTime = millis();
		return;
	}

	// Increment sequence number for each ping
	_pingSeqNo++;

	// Build ICMP echo request
	struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)p->payload;
	ICMPH_TYPE_SET(iecho, ICMP_ECHO);
	ICMPH_CODE_SET(iecho, 0);
	iecho->chksum = 0;
	iecho->id     = htons(PING_PACKET_ID);
	iecho->seqno  = htons(_pingSeqNo);
	iecho->chksum = inet_chksum(iecho, packetSize);

	// Send packet to target IP
	// ESP32's lwIP has IPv6 support, so ip_addr_t uses a union structure instead of a flat addr field
	ip_addr_t dest;
#if defined(ESP32)
	dest.u_addr.ip4.addr = (uint32_t)target;
	dest.type = IPADDR_TYPE_V4;
#else
	dest.addr = (uint32_t)target;
#endif
	raw_sendto(_pingPcb, p, &dest);
	pbuf_free(p);

#if defined(ESP32)
	UNLOCK_TCPIP_CORE();
#endif

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
		_pingState = PingState::IDLE;
		_lastPingTime = millis();
		return;
	}

	// Timeout expired?
	if (millis() - _pingSentTime >= _pingTimeout) {
		WD_LOGLN(F("[WiFi Watchdog] ICMP timeout"));
		_pingState = PingState::IDLE;
		_lastPingTime = millis();

		_pingFailCount++;

		WD_LOG(F("[WiFi Watchdog] Ping fail count: "));
		WD_LOG(_pingFailCount);
		WD_LOG(F(" / "));
		WD_LOGLN(_pingFailThreshold);

		if (_pingFailCount >= _pingFailThreshold) {
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
#if defined(ESP32)
		LOCK_TCPIP_CORE();
#endif
		raw_remove(_pingPcb);
#if defined(ESP32)
		UNLOCK_TCPIP_CORE();
#endif
		_pingPcb = nullptr;
	}
	_pingState = PingState::IDLE;
}
