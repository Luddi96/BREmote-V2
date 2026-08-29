/*
 * VESC-over-WiFi TCP bridge module, integrated into an existing
 * Access Point + web server project.
 *
 * Mirrors vedderb/vesc_express main/comm/comm_wifi.c:
 *   - tcp_task_local(): single-client raw TCP bridge on port 65102
 *   - broadcast_task(): UDP broadcast on port 65109, once per second,
 *     "<name>::<ip>::65102" (NUL-terminated), so VESC Tool auto-lists
 *     the device. In AP mode the real firmware always broadcasts
 *     "192.168.4.1" as the IP, since that's the fixed softAP address.
 *
 * Call vescBridgeBegin() from inside enableWebUi(), after WiFi.softAP()
 * has been started. Call vescBridgeEnd() from inside disableWebUi(),
 * before WiFi.mode(WIFI_OFF). Call vescBridgeLoop() every loop()
 * iteration (it no-ops when the bridge isn't enabled).
 */

#include <WiFi.h>
#include <WiFiUdp.h>

#define P_U1_TX 18
#define P_U1_RX 19
#define VESC_UART_BAUD 115200

#define VESC_TCP_PORT       65102
#define VESC_BROADCAST_PORT 65109
#define VESC_BROADCAST_INTERVAL_MS 1000

static WiFiServer vescTcpServer(VESC_TCP_PORT);
static WiFiClient vescTcpClient;
static WiFiUDP vescUdp;

static bool vescBridgeEnabled = false;
static unsigned long vescLastBroadcast = 0;

static const size_t VESC_BUF_SIZE = 512;
static uint8_t vescBufUartToTcp[VESC_BUF_SIZE];
static uint8_t vescBufTcpToUart[VESC_BUF_SIZE];

// Matches broadcast_task()'s AP-mode branch in comm_wifi.c, which always
// broadcasts the fixed softAP address rather than querying it at runtime.
static void vescSendDiscoveryBroadcast(const char *deviceName) {
  char sendbuf[64];
  int len = snprintf(sendbuf, sizeof(sendbuf), "%s::192.168.4.1::%d",
                      deviceName, VESC_TCP_PORT);
  len += 1; // include terminating NUL, matching the original firmware

  IPAddress broadcastAddr = WiFi.softAPBroadcastIP(); // 192.168.4.255 by default
  vescUdp.beginPacket(broadcastAddr, VESC_BROADCAST_PORT);
  vescUdp.write((const uint8_t *)sendbuf, len);
  vescUdp.endPacket();
}

// Call once WiFi.softAP(...) is already up and running.
void vescBridgeBegin(const char *deviceName) {
  if (vescBridgeEnabled) return;

  Serial1.begin(VESC_UART_BAUD, SERIAL_8N1, P_U1_RX, P_U1_TX);

  vescTcpServer.begin();
  vescTcpServer.setNoDelay(true);

  vescUdp.begin(VESC_BROADCAST_PORT);

  vescLastBroadcast = 0; // force an immediate broadcast on the next loop
  vescBridgeEnabled = true;

  Serial.println("[VESC] Bridge enabled");
  Serial.printf("[VESC] TCP listening on %s:%d\n",
                WiFi.softAPIP().toString().c_str(), VESC_TCP_PORT);
}

// Call before tearing down / switching off WiFi in disableWebUi().
void vescBridgeEnd() {
  if (!vescBridgeEnabled) return;

  if (vescTcpClient) {
    vescTcpClient.stop();
  }
  vescTcpServer.end();
  vescUdp.stop();
  Serial1.end();

  vescBridgeEnabled = false;
  Serial.println("[VESC] Bridge disabled");
}

bool vescBridgeIsEnabled() {
  return vescBridgeEnabled;
}

// Call every loop() iteration. No-ops if the bridge isn't enabled, so it
// is always safe to call unconditionally.
void vescBridgeLoop(const char *deviceName) {
  if (!vescBridgeEnabled) return;

  if (millis() - vescLastBroadcast >= VESC_BROADCAST_INTERVAL_MS) {
    vescLastBroadcast = millis();
    vescSendDiscoveryBroadcast(deviceName);
  }

  if (vescTcpServer.hasClient()) {
    WiFiClient newClient = vescTcpServer.available();
    if (vescTcpClient && vescTcpClient.connected()) {
      newClient.stop(); // single client at a time, like real VESC Express
      Serial.println("[VESC] Rejected extra TCP client");
    } else {
      vescTcpClient = newClient;
      vescTcpClient.setNoDelay(true);
      Serial.print("[VESC] Client connected: ");
      Serial.println(vescTcpClient.remoteIP());
    }
  }

  if (vescTcpClient && vescTcpClient.connected()) {
    size_t n = 0;
    while (Serial1.available() && n < VESC_BUF_SIZE) {
      vescBufUartToTcp[n++] = (uint8_t)Serial1.read();
    }
    if (n > 0) {
      vescTcpClient.write(vescBufUartToTcp, n);
    }
  } else {
    while (Serial1.available()) {
      Serial1.read(); // drain so the VESC's TX side never stalls
    }
  }

  if (vescTcpClient && vescTcpClient.connected()) {
    size_t n = 0;
    while (vescTcpClient.available() && n < VESC_BUF_SIZE) {
      vescBufTcpToUart[n++] = (uint8_t)vescTcpClient.read();
    }
    if (n > 0) {
      Serial1.write(vescBufTcpToUart, n);
    }
  } else if (vescTcpClient) {
    Serial.println("[VESC] Client disconnected");
    vescTcpClient.stop();
  }
}
