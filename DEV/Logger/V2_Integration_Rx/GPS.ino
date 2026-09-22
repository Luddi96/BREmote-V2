#include <math.h>
#include <TinyGPSPlus.h>  // TinyGPSPlus by Mikal Hart 1.0.3

// TinyGPS++ parser (runtime enabled via usrConf.gps_en)

// GPS functions (TinyGPS++ + Kalman) - Optimized Version
// Debug macros
#ifndef GPS_DEBUG
#define GPS_DEBUG 0
#endif

// Global GPS variables for compatibility with the rest of the code
// Structure definition moved to header
struct gps_struct gps; // Instance kept for compatibility with existing code

// Conversion date/heure (TinyGPS++) -> Epoch UTC (Unix)
static uint32_t makeUnixTimeUTC(int year, int month, int day, int hour, int minute, int second) {
  int a = (14 - month) / 12;
  int y = year + 4800 - a;
  int m = month + 12 * a - 3;
  long jdn = day + (153 * m + 2) / 5 + 365L * y + y / 4 - y / 100 + y / 400 - 32045;
  long unixDays = jdn - 2440588L;
  unsigned long seconds = (unsigned long)unixDays * 86400UL + (unsigned long)hour * 3600UL + (unsigned long)minute * 60UL + (unsigned long)second;
  return (uint32_t)seconds;
}

// Minimal NMEA parser RX side
static TinyGPSPlus gpsTiny_Rx;

// Suivi du baud courant (pour affichage diag)
static uint32_t gpsCurrentBaud = 115200;

// (removed) detectGPSBaud_Rx – unused

// GPS state variables
bool gpsInitialized = false;
unsigned long lastGPSUpdate = 0;
unsigned long lastGPSConfigAttempt = 0; // Init to 0 to force at least 30s after boot before reconfig
// Timestamp of last byte received from GPS (raw UART activity)
volatile unsigned long lastGPSByteRxMs = 0;

// GPS initialization retry logic (ISO with TX)
static uint8_t gpsInitRetryCount = 0;
static const uint8_t GPS_MAX_INIT_RETRIES = 3;
static const unsigned long GPS_RETRY_INTERVALS[] = {3000, 8000, 15000}; // Progressive intervals

// NOTE: Legacy SparkFun wrapper stubs removed. We use direct UBX + Serial1 and select MUX via setUartMux(1) where needed.

// ============================
// Low-level UBX helpers (u-blox) on Serial1 – aligned with TX
// ============================

static void ubxComputeChecksum(const uint8_t* buffer, uint16_t length, uint8_t& ckA, uint8_t& ckB) {
  ckA = 0;
  ckB = 0;
  for (uint16_t i = 0; i < length; i++) {
    ckA = ckA + buffer[i];
    ckB = ckB + ckA;
  }
}

static void ubxWriteMessage(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  uint8_t header[6];
  header[0] = 0xB5; // sync char 1
  header[1] = 0x62; // sync char 2
  header[2] = cls;
  header[3] = id;
  header[4] = (uint8_t)(len & 0xFF);
  header[5] = (uint8_t)(len >> 8);
  Serial1.write(header, 6);
  if (len && payload) Serial1.write(payload, len);
  uint8_t ckA, ckB;
  uint8_t tmp[2 + 2 + 64];
  uint16_t idx = 0;
  tmp[idx++] = header[2];
  tmp[idx++] = header[3];
  tmp[idx++] = header[4];
  tmp[idx++] = header[5];
  for (uint16_t i = 0; i < len && idx < sizeof(tmp); i++) tmp[idx++] = payload[i];
  ubxComputeChecksum(tmp, idx, ckA, ckB);
  Serial1.write(ckA);
  Serial1.write(ckB);
}

static bool ubxWaitForAck(uint8_t expectClass, uint8_t expectID, uint16_t timeoutMs) {
  unsigned long start = millis();
  enum { STATE_SYNC1, STATE_SYNC2, STATE_CLASS, STATE_ID, STATE_LEN1, STATE_LEN2, STATE_PAYLOAD, STATE_CKA, STATE_CKB };
  int state = STATE_SYNC1;
  uint8_t cls = 0, id = 0;
  uint16_t len = 0;
  uint16_t payloadRead = 0;
  uint8_t payload[8];
  while (millis() - start < timeoutMs) {
    if (Serial1.available()) {
      uint8_t b = Serial1.read();
      switch (state) {
        case STATE_SYNC1: state = (b == 0xB5) ? STATE_SYNC2 : STATE_SYNC1; break;
        case STATE_SYNC2: state = (b == 0x62) ? STATE_CLASS : STATE_SYNC1; break;
        case STATE_CLASS: cls = b; state = STATE_ID; break;
        case STATE_ID: id = b; state = STATE_LEN1; break;
        case STATE_LEN1: len = b; state = STATE_LEN2; break;
        case STATE_LEN2: len |= ((uint16_t)b << 8); payloadRead = 0; state = (len == 0) ? STATE_CKA : STATE_PAYLOAD; break;
        case STATE_PAYLOAD:
          if (payloadRead < sizeof(payload)) payload[payloadRead] = b;
          payloadRead++;
          if (payloadRead >= len) state = STATE_CKA;
          break;
        case STATE_CKA: state = STATE_CKB; break;
        case STATE_CKB:
          if (cls == 0x05) {
            if (id == 0x01 && len >= 2) { if (payload[0] == expectClass && payload[1] == expectID) return true; }
            if (id == 0x00 && len >= 2) { if (payload[0] == expectClass && payload[1] == expectID) return false; }
          }
          state = STATE_SYNC1;
          break;
      }
    }
  }
  return false;
}

static bool ubxCfgRate5Hz() {
  uint8_t payload[6];
  payload[0] = 200; payload[1] = 0x00; // 200 ms
  payload[2] = 0x01; payload[3] = 0x00; // 1 cycle
  payload[4] = 0x01; payload[5] = 0x00; // GPS time
  while (Serial1.available()) Serial1.read();
  ubxWriteMessage(0x06, 0x08, payload, sizeof(payload));
  return ubxWaitForAck(0x06, 0x08, 500);
}

static bool ubxCfgPrtUART1(uint32_t baud, uint16_t inProtoMask, uint16_t outProtoMask) {
  uint8_t payload[20] = {0};
  payload[0] = 0x01; // UART1
  payload[4] = 0xD0; payload[5] = 0x08; payload[6] = 0x00; payload[7] = 0x00; // 8N1
  payload[8]  = (uint8_t)(baud & 0xFF);
  payload[9]  = (uint8_t)((baud >> 8) & 0xFF);
  payload[10] = (uint8_t)((baud >> 16) & 0xFF);
  payload[11] = (uint8_t)((baud >> 24) & 0xFF);
  payload[12] = (uint8_t)(inProtoMask & 0xFF);
  payload[13] = (uint8_t)((inProtoMask >> 8) & 0xFF);
  payload[14] = (uint8_t)(outProtoMask & 0xFF);
  payload[15] = (uint8_t)((outProtoMask >> 8) & 0xFF);
  while (Serial1.available()) Serial1.read();
  ubxWriteMessage(0x06, 0x00, payload, sizeof(payload));
  return ubxWaitForAck(0x06, 0x00, 600);
}

static bool ubxCfgMsgNMEA(uint8_t nmeaMsgId, uint8_t uart1Rate) {
  uint8_t payload[8] = {0};
  payload[0] = 0xF0; // NMEA
  payload[1] = nmeaMsgId; // 0x04=RMC, 0x00=GGA
  payload[3] = uart1Rate; // UART1 rate
  while (Serial1.available()) Serial1.read();
  ubxWriteMessage(0x06, 0x01, payload, sizeof(payload));
  return ubxWaitForAck(0x06, 0x01, 500);
}

static bool ubxCfgNav5Sea() {
  uint8_t payload[36] = {0};
  payload[0] = 0x05; // mask: dyn + fix
  payload[2] = 0x05; // dynamic model = sea
  payload[3] = 0x03; // auto 2D/3D
  while (Serial1.available()) Serial1.read();
  ubxWriteMessage(0x06, 0x24, payload, sizeof(payload));
  return ubxWaitForAck(0x06, 0x24, 500);
}

// Enhanced GPS configuration with progressive retry logic
bool gpsConfigure() {
  Serial.print("Starting GPS...");
  
  #ifdef DEBUG_GPS
  Serial.printf("=== Configuring GPS RX (attempt %d/%d) ===\n", gpsInitRetryCount + 1, GPS_MAX_INIT_RETRIES);
  #endif
  lastGPSConfigAttempt = millis();
  
  // Enhanced MUX locking with longer duration for initial setup
  extern void gpsMuxLock(uint32_t);
  gpsMuxLock(8000); // Extended lock for more robust init
  
  // Ensure MUX is properly set before starting
  for (int i = 0; i < 3; i++) {
    setUartMux(1);
    delay(100);
    verifyGPSMuxConfiguration();
    delay(200);
  }

  // Quick mux verification (best effort) — run once per boot to avoid spam
  static bool muxVerifiedOnce = false;
  if (!muxVerifiedOnce) {
    verifyGPSMuxConfiguration();
    muxVerifiedOnce = true;
  }

  // Enhanced NMEA detection with progressive timeouts
  auto listenForNMEA = [&](uint32_t baud, unsigned long windowMs) -> bool {
    Serial1.end();
    delay(100); // Extended delay for GPS stability
    Serial1.begin(baud, SERIAL_8N1, P_U1_RX, P_U1_TX);
    while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));
    delay(200); // Longer startup delay
    
    // Progressive timeout: longer detection window based on retry count (ISO with TX)
    unsigned long extendedWindow = windowMs + (gpsInitRetryCount * 800);
    unsigned long start = millis();
    int nmeaChars = 0;
    
    while (millis() - start < extendedWindow) {
      if (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '$') {
          nmeaChars++;
          if (nmeaChars >= 2) return true; // Multiple NMEA sentences for better confidence
        }
      }
      delayMicroseconds(200); // Slightly longer between reads
    }
    return false;
  };

  // First, try passive NMEA detection and use PUBX fallback at that baud
  {
    // Passive NMEA scan on common boot bauds to detect live stream before UBX config
    uint32_t scanBauds[] = {9600, 115200, 57600, 38400, 19200};
    const int nb = sizeof(scanBauds)/sizeof(scanBauds[0]);
    for (int i = 0; i < nb; i++) {
      uint32_t b = scanBauds[i];
      #ifdef DEBUG_GPS
      Serial.printf("NMEA scan at %lu baud (retry %d)...\n", (unsigned long)b, gpsInitRetryCount + 1);
      #endif
      // Progressive timeout: longer detection window based on retry count (ISO with TX) 
      unsigned long scanTimeout = 800 + (gpsInitRetryCount * 400); // 1.5-3.9s
      if (listenForNMEA(b, scanTimeout)) {
        #ifdef DEBUG_GPS
        Serial.printf("→ NMEA detected at %lu, trying PUBX fallback...\n", (unsigned long)b);
        #endif

        auto sendPUBX41 = [&](uint32_t baud){
          char body[64];
          snprintf(body, sizeof(body), "PUBX,41,1,0007,0007,%lu,0", (unsigned long)baud);
          uint8_t cs = 0; for (const char* p = body; *p; ++p) cs ^= (uint8_t)(*p);
          char sentence[96]; snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", body, cs);
          Serial1.print(sentence);
        };
        // Enable UBX at current rate and switch to 115200
        for (int attempt = 1; attempt <= 2; attempt++) {
          sendPUBX41(b);
          delay(150);
          ubxCfgPrtUART1(b, 0x0007, 0x0003);
          delay(150);
        }
        sendPUBX41(115200);
        ubxCfgPrtUART1(115200, 0x0007, 0x0003);
        delay(400);
        // Reopen at 115200 and apply config
        Serial1.end();
        delay(200);
        Serial1.begin(115200, SERIAL_8N1, P_U1_RX, P_U1_TX);
        while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));
        delay(400);

        gpsCurrentBaud = 115200;
        // NAV5 -> RATE (with retries) -> NMEA filter
        ubxCfgNav5Sea();
        bool okRateScan = false; for (int a=0;a<3 && !okRateScan;a++){ okRateScan = ubxCfgRate5Hz(); if(!okRateScan) delay(120);} if(!okRateScan) Serial.println("⚠️ CFG-RATE 5Hz not acknowledged after retries");
        uint8_t toDisableS[] = {0x01,0x02,0x03,0x05,0x06,0x07,0x08,0x09,0x0A,0x0D};
        for (uint8_t j = 0; j < sizeof(toDisableS); j++) ubxCfgMsgNMEA(toDisableS[j], 0);
        ubxCfgMsgNMEA(0x04, 1); // RMC
        ubxCfgMsgNMEA(0x00, 1); // GGA
        // Enhanced NMEA verification with progressive timeout
        bool seenAfter = false;
        {
          while (Serial1.available()) Serial1.read(); // flush stale bytes
          delay(300); // Let GPS stabilize after config (ISO with TX)
          unsigned long verifyTimeout = 1500 + (gpsInitRetryCount * 500); // 2.5-5.5s (ISO with TX)
          unsigned long startSeen = millis();
          int validNmeaCount = 0;
          while (millis() - startSeen < verifyTimeout && !seenAfter) {
            while (Serial1.available()) {
              char c = (char)Serial1.read();
              if (c == '$') { 
                // simple sanity: expect $G prefix soon
                unsigned long t = millis();
                while (millis() - t < 20 && !Serial1.available()) { delayMicroseconds(100); }
                if (Serial1.available()) {
                  char n = (char)Serial1.read();
                  if (n == 'G' || n == 'P' || n == 'N') { 
                    validNmeaCount++;
                    if (validNmeaCount >= 2) { seenAfter = true; break; } // Multiple valid NMEA for confidence
                  }
                }
              }
            }
            delay(5);
          }
        }
        if (seenAfter) {
          gpsInitialized = true;
          lastGPSUpdate = millis();
          gpsInitRetryCount = 0; // Reset retry count on success
          #ifdef DEBUG_GPS
          Serial.printf("GPS RX configured: 115200 baud, 5Hz, NMEA RMC+GGA (after %d attempts)\n", gpsInitRetryCount + 1);
          #endif
          // Keep GPS MUX lock until first data is flowing in gpsPoll
          Serial.println(" Done");
          return true;
        } else {
          gpsInitialized = false;
          Serial.println(" FAILED, NOT INITIALIZED!");
          #ifdef DEBUG_GPS
          Serial.printf("No NMEA after configuration at 115200 - attempt %d failed\n", gpsInitRetryCount + 1);
          #endif
        }
      }
    }
  }

  // Try a set of common boot baud rates like TX (broadened)
  uint32_t bootBauds[] = {38400, 9600, 57600, 115200, 19200};
  const int numBauds = sizeof(bootBauds) / sizeof(bootBauds[0]);

  for (int i = 0; i < numBauds; i++) {
    uint32_t tryBaud = bootBauds[i];
    #ifdef DEBUG_GPS
    Serial.printf("Trying connection at %lu baud...\n", (unsigned long)tryBaud);
    #endif
    Serial1.end();
    delay(200);
    Serial1.begin(tryBaud, SERIAL_8N1, P_U1_RX, P_U1_TX);
    while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));
    delay(300);

    // Try to set current UART1 config (enable UBX+NMEA) at the current baud
    if (!ubxCfgPrtUART1(tryBaud, 0x0007, 0x0003)) {
      #ifdef DEBUG_GPS
      Serial.println("⚠️ UBX-CFG-PRT did not respond at this baud");
      #endif
      continue; // try next baud
    }

    // Switch locally to 115200
    Serial1.end();
    delay(200);
    Serial1.begin(115200, SERIAL_8N1, P_U1_RX, P_U1_TX);
    while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));
    delay(300);
    gpsCurrentBaud = 115200;

    // Apply NAV5 first (dynamic model), then RATE with retries, then NMEA filtering (RMC+GGA only)
    ubxCfgNav5Sea();
    bool okRate = false;
    for (int attempt = 1; attempt <= 3 && !okRate; attempt++) {
      okRate = ubxCfgRate5Hz();
      if (!okRate) delay(120);
    }
    #ifdef DEBUG_GPS
    if (!okRate) Serial.println("⚠️ CFG-RATE 5Hz not acknowledged after retries");
    #endif
    uint8_t toDisable[] = {0x01,0x02,0x03,0x05,0x06,0x07,0x08,0x09,0x0A,0x0D};
    for (uint8_t j = 0; j < sizeof(toDisable); j++) ubxCfgMsgNMEA(toDisable[j], 0);
    ubxCfgMsgNMEA(0x04, 1); // RMC
    ubxCfgMsgNMEA(0x00, 1); // GGA

    // Verify NMEA presence before confirming success
    bool seenAfterOK = false;
    {
      while (Serial1.available()) Serial1.read(); // flush stale bytes
      unsigned long startSeen = millis();
      while (millis() - startSeen < 1800 && !seenAfterOK) {
        while (Serial1.available()) {
          char c = (char)Serial1.read();
          if (c == '$') {
            unsigned long t = millis();
            while (millis() - t < 20 && !Serial1.available()) { delayMicroseconds(100); }
            if (Serial1.available()) {
              char n = (char)Serial1.read();
              if (n == 'G' || n == 'P' || n == 'N') { seenAfterOK = true; break; }
            }
          }
        }
        delay(5);
      }
    }
    if (seenAfterOK) {
      gpsInitialized = true;
      lastGPSUpdate = millis();
      gpsInitRetryCount = 0; // Reset retry count on success
      #ifdef DEBUG_GPS
      Serial.printf("GPS RX configured: 115200 baud, 5Hz, NMEA RMC+GGA (UBX method, after %d attempts)\n", gpsInitRetryCount + 1);
      #endif
      // Keep GPS MUX lock until first data is flowing in gpsPoll
      return true;
    } else {
      gpsInitialized = false;
      #ifdef DEBUG_GPS
      Serial.printf("No NMEA after UBX configuration - attempt %d failed\n", gpsInitRetryCount + 1);
      #endif
    }
  }

  // Fallback like TX: try NMEA $PUBX,41 at 38400 to enable UBX and switch to 115200
  #ifdef DEBUG_GPS
  Serial.println("Could not connect via UBX at common bauds");
  Serial.println("Fallback: trying NMEA $PUBX,41 handshake...");
  #endif

  Serial1.end();
  delay(200);
  Serial1.begin(38400, SERIAL_8N1, P_U1_RX, P_U1_TX);
  while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));
  delay(300);

  // Short listen to ensure NMEA is present
  unsigned long startListen = millis();
  bool sawNMEA = false;
  while (millis() - startListen < 1200) {
    if (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '$') { sawNMEA = true; break; }
    }
  }

  if (sawNMEA) {
    auto sendPUBX41 = [](uint32_t baud){
      char body[64];
      snprintf(body, sizeof(body), "PUBX,41,1,0007,0007,%lu,0", (unsigned long)baud);
      uint8_t cs = 0;
      for (const char* p = body; *p; ++p) cs ^= (uint8_t)(*p);
      char sentence[96];
      snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", body, cs);
      Serial1.print(sentence);
    };

    // Enable UBX on UART1 at current baud and then switch to 115200
    #ifdef DEBUG_GPS
    Serial.println("→ Enabling UBX on UART1 at 38400 via PUBX and CFG-PRT...");
    #endif
    for (int attempt = 1; attempt <= 3; attempt++) {
      sendPUBX41(38400);
      delay(120);
      ubxCfgPrtUART1(38400, 0x0007, 0x0003);
      delay(200);
    }

    #ifdef DEBUG_GPS
    Serial.println("→ Switching UART1 to 115200 via PUBX+CFG-PRT, then re-open locally...");
    #endif
    sendPUBX41(115200);
    ubxCfgPrtUART1(115200, 0x0007, 0x0003);
    delay(500);
    Serial1.end();
    delay(200);
    Serial1.begin(115200, SERIAL_8N1, P_U1_RX, P_U1_TX);
    while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));
    delay(400);

    // Apply NAV5, 5 Hz (with retries) and NMEA minimal set
    ubxCfgNav5Sea();
    bool okRate2 = false;
    for (int attempt = 1; attempt <= 3 && !okRate2; attempt++) {
      okRate2 = ubxCfgRate5Hz();
      if (!okRate2) delay(120);
    }
    #ifdef DEBUG_GPS
    if (!okRate2) Serial.println("⚠️ CFG-RATE 5Hz not acknowledged after retries (fallback)");
    #endif
    uint8_t toDisable2[] = {0x01,0x02,0x03,0x05,0x06,0x07,0x08,0x09,0x0A,0x0D};
    for (uint8_t j = 0; j < sizeof(toDisable2); j++) ubxCfgMsgNMEA(toDisable2[j], 0);
    ubxCfgMsgNMEA(0x04, 1); // RMC
    ubxCfgMsgNMEA(0x00, 1); // GGA
    gpsCurrentBaud = 115200;
    // Verify NMEA presence before confirming success
    bool seenAfterFB = false;
    {
      while (Serial1.available()) Serial1.read(); // flush stale bytes
      unsigned long startSeen = millis();
      while (millis() - startSeen < 1800 && !seenAfterFB) {
        while (Serial1.available()) {
          char c = (char)Serial1.read();
          if (c == '$') {
            unsigned long t = millis();
            while (millis() - t < 20 && !Serial1.available()) { delayMicroseconds(100); }
            if (Serial1.available()) {
              char n = (char)Serial1.read();
              if (n == 'G' || n == 'P' || n == 'N') { seenAfterFB = true; break; }
            }
          }
        }
        delay(5);
      }
    }
    if (seenAfterFB) {
      gpsInitialized = true;
      lastGPSUpdate = millis();
      gpsInitRetryCount = 0; // Reset retry count on success
      #ifdef DEBUG_GPS
      Serial.printf("GPS RX configured (PUBX fallback): 115200 baud, 5Hz, NMEA RMC+GGA (after %d attempts)\n", gpsInitRetryCount + 1);
      #endif
      // Keep GPS MUX lock until first data is flowing in gpsPoll
      return true;
    } else {
      gpsInitialized = false;
      #ifdef DEBUG_GPS
      Serial.printf("No NMEA after PUBX fallback - attempt %d failed\n", gpsInitRetryCount + 1);
      #endif
    }
  } else {
    #ifdef DEBUG_GPS
    Serial.printf("No NMEA detected at 38400 during PUBX fallback - attempt %d\n", gpsInitRetryCount + 1);
    #endif
  }

  // Increment retry count and determine if we should continue trying
  gpsInitRetryCount++;
  if (gpsInitRetryCount >= GPS_MAX_INIT_RETRIES) {
    #ifdef DEBUG_GPS
    Serial.println("GPS RX: Maximum retry attempts reached - marking as failed");
    #endif
    gpsInitRetryCount = GPS_MAX_INIT_RETRIES - 1; // Cap to prevent overflow
    gpsInitialized = false;
  }
  
  return false;
}

// GPS data read
void gpsPoll() {
  if (!gpsInitialized) {
    return;
  }
  
  // Protection against too frequent reconnects
  const unsigned long GPS_RECONNECT_INTERVAL = 20000; // 20s entre tentatives
  const unsigned long GPS_DATA_TIMEOUT = 7000; // reduce to 7s to align with observed 5Hz cadence and guard early
  const unsigned long GPS_FIRST_BOOT_RETRY_MS = 15000; // retry sooner if never received data
  
  unsigned long nowMs = millis();
  unsigned long timeSinceLastConfig = nowMs - lastGPSConfigAttempt;
  unsigned long timeSinceLastData = nowMs - lastGPSUpdate;
  unsigned long timeSinceLastByte = nowMs - lastGPSByteRxMs;
  
  // NEW LOGIC: base decisions only on actual data reception
  // Allow more time after a recent configuration
  unsigned long effectiveTimeout = GPS_DATA_TIMEOUT;
  if (timeSinceLastConfig < 60000) { // If configured less than 1 minute ago
    effectiveTimeout = (uint32_t)(GPS_DATA_TIMEOUT * 1.5f); // grace period after config
  }
  
  bool hasRecentData = (lastGPSUpdate > 0) && (timeSinceLastData < effectiveTimeout);
  bool hasRecentBytes = (lastGPSByteRxMs > 0) && (timeSinceLastByte < effectiveTimeout);
  
  // Simplified DEBUG - do not use myGNSS.isConnected() which is unreliable
  #ifdef DEBUG_GPS
  static unsigned long lastDebugMsg = 0;
  if (millis() - lastDebugMsg > 5000) { // Debug toutes les 5 secondes
    lastDebugMsg = millis();
    Serial.printf("[GPS Rx] dataAge=%lums recent=%s\n", 
                  timeSinceLastData, hasRecentData ? "YES" : "NO");
  }
  #endif
  
  // Enhanced reconnect logic with progressive intervals
  if (!hasRecentData && !hasRecentBytes) {
    bool shouldRetry = false;
    unsigned long retryInterval = GPS_FIRST_BOOT_RETRY_MS;
    
    // Use progressive retry intervals based on retry count
    if (gpsInitRetryCount < GPS_MAX_INIT_RETRIES && lastGPSUpdate == 0) {
      retryInterval = GPS_RETRY_INTERVALS[min(gpsInitRetryCount, (uint8_t)(sizeof(GPS_RETRY_INTERVALS)/sizeof(GPS_RETRY_INTERVALS[0]) - 1))];
      shouldRetry = (timeSinceLastConfig > retryInterval);
    } else if (gpsInitRetryCount < GPS_MAX_INIT_RETRIES) {
      // After first successful connection, use longer intervals
      shouldRetry = (timeSinceLastConfig > GPS_RECONNECT_INTERVAL);
    } else {
      // Max retries reached, only try every 3 minutes (ISO with TX)
      shouldRetry = (timeSinceLastConfig > 180000);
    }
    
    if (shouldRetry) {
      Serial.printf("GPS data timeout - reconnection attempt %d (interval: %lums)\n", gpsInitRetryCount + 1, retryInterval);
      lastGPSConfigAttempt = millis();
      if (gpsConfigure()) {
        Serial.println("GPS reconnection successful");
        lastGPSUpdate = millis();
      } else {
        Serial.printf("GPS reconnection failed (attempt %d/%d) - will retry later\n", gpsInitRetryCount, GPS_MAX_INIT_RETRIES);
        if (gpsInitRetryCount >= GPS_MAX_INIT_RETRIES) {
          gpsInitialized = false;
        }
        telemetry.foil_speed = 0xFF;
        return;
      }
    } else {
      telemetry.foil_speed = 0xFF;
      return;
    }
  }

  // Nouveau: lecture NMEA via TinyGPS++
  // Temporarily secure MUX on GPS side during parsing window
  extern void gpsMuxLock(uint32_t);
  // Short lock, but avoid sticking to VESC tick: 220 ms
  gpsMuxLock(400);
  setUartMux(1);
  vTaskDelay(pdMS_TO_TICKS(10));
  Serial1.end();
  Serial1.begin(115200, SERIAL_8N1, P_U1_RX, P_U1_TX);
  while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));

  bool parsed = false;
  unsigned long feedUntil = millis() + 300; // larger parsing window at init to ease lock
  int rlength = 0;
  int encodetrue = 0;

  //Wait 300ms, this will be enough to capture both $GNRMC and $GNGGA packets at least once fully in the UART input buffers
  vTaskDelay(pdMS_TO_TICKS(300)); 
  //After the delay read out the buffers
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    lastGPSByteRxMs = millis();
    if (gpsTiny_Rx.encode(c)) parsed = true;
  }

  // Release MUX lock after parsing window
  extern void gpsMuxUnlock();
  gpsMuxUnlock();
  // Return UART to VESC immediately after parsing
  setUartMux(0);

  if (parsed) {
    if (gpsTiny_Rx.location.isUpdated() || gpsTiny_Rx.location.isValid()) {
      double raw_lat = gpsTiny_Rx.location.lat();
      double raw_lon = gpsTiny_Rx.location.lng();
      gps.latitude = raw_lat;
      gps.longitude = raw_lon;
      float hd = gpsTiny_Rx.hdop.isValid() ? gpsTiny_Rx.hdop.hdop() : 3.0f;
      gps.hdop = hd;
    }
    if (gpsTiny_Rx.speed.isUpdated() || gpsTiny_Rx.speed.isValid()) {
      float s = gpsTiny_Rx.speed.kmph();
      if (s < 0.0f) s = 0.0f;
      if (s > 150.0f) s = 150.0f;
      gps.speed = s;
    }
    // Cap depuis NMEA (TinyGPS++)
    if (gpsTiny_Rx.course.isUpdated() || gpsTiny_Rx.course.isValid()) {
      gps.heading = gpsTiny_Rx.course.deg();
    }
    if (gpsTiny_Rx.satellites.isValid()) {
      gps.satellites = (uint8_t)gpsTiny_Rx.satellites.value();
    }
    if (gpsTiny_Rx.date.isValid() && gpsTiny_Rx.time.isValid()) {
      int year = gpsTiny_Rx.date.year();
      int month = gpsTiny_Rx.date.month();
      int day = gpsTiny_Rx.date.day();
      int hour = gpsTiny_Rx.time.hour();
      int minute = gpsTiny_Rx.time.minute();
      int second = gpsTiny_Rx.time.second();
      gps.datetime = makeUnixTimeUTC(year, month, day, hour, minute, second);
    }
    gps.fix_quality = gpsTiny_Rx.location.isValid() ? 2 : gps.fix_quality;
    
    // Update speed for telemetry
    if (gps.fix_quality >= 2 && gps.hdop < 20) { // Si on a au moins un fix 2D
      telemetry.foil_speed = (uint8_t)constrain(gps.speed, 0, 254);
    } else {
      telemetry.foil_speed = 0xFF; // Pas de fix valide
    }
    
    lastGPSUpdate = millis();
    // Release GPS MUX lock after first valid parsing so other tasks can use UART
    extern void gpsMuxUnlock();
    gpsMuxUnlock();
    
    // Detailed debug message every 10 seconds when OK
    static unsigned long lastWorkingMsg = 0;
    if (millis() - lastWorkingMsg > 10000) {
      lastWorkingMsg = millis();
      if(usrConf.debug_byte & 1<<1)
      {
       Serial.printf("[DEBUG] GPS DATA OK - Fix:%d, Sats:%d, Speed:%.1fkm/h\n", 
                     gps.fix_quality, gps.satellites, gps.speed);
      }
    }
  } else {
    // No new data, mark invalid if too old
    if (millis() - lastGPSUpdate > 2000) { // Timeout de 2 secondes
      telemetry.foil_speed = 0xFF;
    }
  }
}

// Affichage des informations satellites (remplace printSatelliteInfo)
void gpsPrintStatus() {
  if (!gpsInitialized) {
    Serial.println("GPS not initialized");
    return;
  }
  
  Serial.println("----- GPS Remote Status -----");
  
  // Forcer une lecture rapide TinyGPS++
  unsigned long feedStart = millis();
  while (millis() - feedStart < 100) {
    while (Serial1.available()) gpsTiny_Rx.encode((char)Serial1.read());
    delayMicroseconds(100);
  }

  // Synchronize state from TinyGPS++ for consistency
  if (gpsTiny_Rx.satellites.isValid()) {
    gps.satellites = (uint8_t)gpsTiny_Rx.satellites.value();
  }
  if (gpsTiny_Rx.hdop.isValid()) {
    gps.hdop = gpsTiny_Rx.hdop.hdop();
  }
  if (gpsTiny_Rx.location.isValid()) {
    gps.latitude = gpsTiny_Rx.location.lat();
    gps.longitude = gpsTiny_Rx.location.lng();
    if (gps.fix_quality < 2) gps.fix_quality = 2;
  }
  // altitude removed
  if (gpsTiny_Rx.speed.isValid()) {
    float s = gpsTiny_Rx.speed.kmph();
    if (s < 0.0f) s = 0.0f; if (s > 50.0f) s = 50.0f; gps.speed = s;
  }
  if (gpsTiny_Rx.course.isValid()) {
    gps.heading = gpsTiny_Rx.course.deg();
  }

  if (gpsTiny_Rx.location.isValid() || gpsTiny_Rx.speed.isValid()) {
    Serial.printf("Fix Type: %d ", gps.fix_quality);
    switch(gps.fix_quality) {
      case 0: Serial.println("(No fix)"); break;
      case 1: Serial.println("(Dead reckoning only)"); break;
      case 2: Serial.println("(2D fix)"); break;
      case 3: Serial.println("(3D fix)"); break;
      case 4: Serial.println("(GNSS + dead reckoning)"); break;
      case 5: Serial.println("(Time only fix)"); break;
      default: Serial.println("(Unknown)"); break;
    }
    
    Serial.printf("Satellites: %d\n", gps.satellites);
    Serial.printf("HDOP: %.2f\n", gps.hdop);
    if (gps.fix_quality >= 2) {
      Serial.printf("Latitude: %.8f°\n", gps.latitude);
      Serial.printf("Longitude: %.8f°\n", gps.longitude);
      // altitude removed
      Serial.printf("Vitesse: %.2f km/h\n", gps.speed);
      Serial.printf("Cap: %.2f°\n", gps.heading);
    }
  } else {
    Serial.println("Could not get PVT data from GPS");
  }
  
  Serial.println("----------------------------------------------");
}

// Diagnostic GPS complet
void gpsDiagnosticFull() {
  Serial.println("=== Full GPS Remote Diagnostic ===");
  
  // Hold GPS MUX during diagnostic to avoid contention
  extern void gpsMuxLock(uint32_t);
  gpsMuxLock(5000);
  setUartMux(1);
  delay(100);
  
  // 1. UART multiplexer check
  Serial.println("1. UART Multiplexer Check:");
  verifyGPSMuxConfiguration();
  
  // 2. Test de connexion de base
  Serial.println("\n2. Basic Connection Test:");
  Serial.printf("GPS Initialized: %s\n", gpsInitialized ? "YES" : "NO");
  
  if (!gpsInitialized) {
    Serial.println("Attempting GPS initialization...");
    if (gpsConfigure()) {
      Serial.println("✅ GPS initialization successful");
    } else {
      Serial.println("❌ GPS initialization failed");
      Serial.println("Check physical connections, power, and antenna");
      return;
    }
  }
  
  Serial.println("✅ GPS UART connected");
  
  // 3. Raw serial communication test
  Serial.println("\n3. Raw Serial Communication Test:");
  setUartMux(1);  // CRITICAL: Select GPS channel
  Serial1.end();
  delay(300);
      Serial1.begin(115200, SERIAL_8N1, P_U1_RX, P_U1_TX);
  while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));
  delay(500);
  
  // Vider le buffer
  while (Serial1.available()) {
    Serial1.read();
  }
  
  // Listen for 3 seconds
  unsigned long startTime = millis();
  int bytesReceived = 0;
  bool nmea_detected = false;
  bool ubx_detected = false;
  
  while (millis() - startTime < 3000) {
    if (Serial1.available()) {
      uint8_t inByte = Serial1.read();
      bytesReceived++;
      
      if (inByte == '$') {
        nmea_detected = true;
      }
      if (inByte == 0xB5) {
        ubx_detected = true;
      }
    }
  }
  
  Serial.printf("Bytes received: %d\n", bytesReceived);
  Serial.printf("NMEA detected: %s\n", nmea_detected ? "YES" : "NO");
  Serial.printf("UBX detected: %s\n", ubx_detected ? "YES" : "NO");
  
  if (bytesReceived == 0) {
    Serial.println("❌ No data received - check connections and power");
    return;
  }
  
  // Reconnect GPS serial link
  // Reopen UART after raw test
  setUartMux(1);
  Serial1.end();
  delay(300);
  Serial1.begin(115200, SERIAL_8N1, P_U1_RX, P_U1_TX);
  delay(300);
  
  // 2. Informations du module
  Serial.println("\n2. Informations du Module:");
  Serial.println("(u-blox via NMEA/UBX direct, version non disponible sans driver)");
  
  // 3. Configuration actuelle
  Serial.println("\n3. Configuration Actuelle:");
  Serial.println("Taux de Navigation: 5 Hz");
  Serial.printf("Serial Baud: %lu\n", (unsigned long)gpsCurrentBaud);
  
  // 4. Statut des Satellites
  Serial.println("\n4. Statut des Satellites:");
  gpsPrintStatus();
  
  // 5. Real-time Data Test (5 seconds)
  Serial.println("\n5. Real-time Data Test (5 seconds):");
  startTime = millis();
  int updates = 0;
  while (millis() - startTime < 5000) {
    // Alimente TinyGPS++ pendant le diag
    unsigned long feed = millis();
    while (millis() - feed < 100) {
      while (Serial1.available()) {
        char c = (char)Serial1.read();
        lastGPSByteRxMs = millis();
        gpsTiny_Rx.encode(c);
      }
    }
    if (gpsTiny_Rx.location.isValid() || gpsTiny_Rx.speed.isValid()) {
      updates++;
      lastGPSUpdate = millis();
      Serial.printf("Update %d: Sats: %lu, Speed: %.1f km/h\n",
                    updates,
                    gpsTiny_Rx.satellites.value(),
                    gpsTiny_Rx.speed.kmph());
    }
    delay(200);
  }
  
  if (updates == 0) {
    Serial.println("❌ No PVT updates received");
  } else {
    Serial.printf("✅ Received %d PVT updates in 5 seconds\n", updates);
  }
  
  // Avoid immediate reconnection after successful diagnostic
  lastGPSConfigAttempt = millis();
  // Release GPS MUX lock after diagnostic
  extern void gpsMuxUnlock();
  gpsMuxUnlock();
  setUartMux(1);

  Serial.println("=== Diagnostic Complete ===");
}

  // Verify UART multiplexer for GPS
void verifyGPSMuxConfiguration() {
  // Silent multiplexer verification for clean boot
  setUartMux(1); // Ensure GPS channel
  delay(100);
  
  // Read multiplexer control pins state
  bool mux0 = safeAwRead(AP_U1_MUX_0);
  bool mux1 = safeAwRead(AP_U1_MUX_1);
  
  // Only show error if multiplexer fails
  if (!(mux0 == HIGH && mux1 == LOW)) {
    Serial.println("Warning: GPS multiplexer configuration issue");
    setUartMux(1); // Try to fix
    delay(100);
  }
}
