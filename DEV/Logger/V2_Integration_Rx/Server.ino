extern const char CONF_HTML[];

void handleRoot() {
  const char* html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>BREmote Rx</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;background:#111;color:#ddd;padding:40px;}"
    "h1{color:#fff;}"
    "a{display:block;color:#6cf;text-decoration:none;font-size:18px;margin:12px 0;}"
    "a:hover{text-decoration:underline;}"
    "</style></head><body>"
    "<h1>BREmote Rx</h1>"
    "<a href='/conf.html'>Configuration</a>"
    "<a href='/logs'>Log Files</a>"
    "</body></html>";
  server.send(200, "text/html", html);
}

// GET /conf.html
// Serves the page directly from flash (PROGMEM), no SPIFFS access needed.
void handleConfPage() {
  server.send_P(200, "text/html", CONF_HTML);
}

// GET /getconf
// Reads the current confStruct base64 blob fresh from SPIFFS and returns
// it as plain text, exactly as stored.
void handleGetConf() {
  if (!SPIFFS.exists(CONF_FILE_PATH)) {
    server.send(404, "text/plain", "No config file on SPIFFS");
    return;
  }

  File file = SPIFFS.open(CONF_FILE_PATH, FILE_READ);
  if (!file) {
    server.send(500, "text/plain", "Failed to open config file");
    return;
  }

  String encodedString = file.readString();
  file.close();

  server.send(200, "text/plain", encodedString);
}

// POST /setconf   (form field "data" = new base64 string)
// Validates that the decoded size matches sizeof(confStruct) before writing,
// so a malformed edit from the webpage can't corrupt SPIFFS with a bad-length blob.
void handleSetConf() {
  if (!server.hasArg("data")) {
    server.send(400, "text/plain", "Missing data");
    return;
  }

  String data = server.arg("data");

  size_t decodedLen = 0;
  mbedtls_base64_decode(NULL, 0, &decodedLen, (const uint8_t*)data.c_str(), data.length());
  uint8_t* decodedData = new uint8_t[decodedLen];
  int rc = mbedtls_base64_decode(decodedData, decodedLen, &decodedLen,
                                  (const uint8_t*)data.c_str(), data.length());
  delete[] decodedData;

  if (rc != 0) {
    server.send(400, "text/plain", "Invalid base64 data");
    return;
  }
  if (decodedLen != sizeof(confStruct)) {
    server.send(400, "text/plain",
                "Size mismatch: expected " + String(sizeof(confStruct)) +
                " bytes, got " + String(decodedLen));
    return;
  }

  File file = SPIFFS.open(CONF_FILE_PATH, FILE_WRITE);
  if (!file) {
    server.send(500, "text/plain", "Failed to open file for writing");
    return;
  }
  file.print(data);
  file.close();

  Serial.println("New config saved via /setconf: " + data);
  server.send(200, "text/plain", "OK");
}

// GET /applyconf
// Re-reads the just-saved SPIFFS config into the live usrConf struct
// without rebooting the device.
void handleApplyConf() {
  serApplyConf();
  server.send(200, "text/plain", "Applied");
}

// GET /rebootdevice
// Acknowledges the request, then restarts the ESP32 so the new config
// takes effect from a clean boot.
void handleRebootDevice() {
  server.send(200, "text/plain", "Rebooting");
  server.client().flush();
  delay(200);
  ESP.restart();
}

String makeApSsid() {
  uint64_t chipId = ESP.getEfuseMac();
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "BREmote_Rx_%012llX", chipId);
  return String(ssid);
}

String urlEncode(const String& s) {
  String out;
  char hex[4];
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/') out += char(c);
    else {
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

String htmlEscape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else out += c;
  }
  return out;
}

void handleDownload() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Missing file name");
    return;
  }

  String filename = server.arg("name");
  if (!filename.startsWith("/")) filename = "/" + filename;

  File file = SPIFFS.open(filename, "r");
  if (!file) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  String shortName = filename;
  int slashPos = shortName.lastIndexOf('/');
  if (slashPos >= 0) shortName = shortName.substring(slashPos + 1);

  server.sendHeader("Content-Disposition", "attachment; filename=\"" + shortName + "\"");
  server.streamFile(file, "application/octet-stream");
  file.close();
}

void handleLogFilesPage() {
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) {
    server.send(500, "text/plain", "Failed to open SPIFFS root");
    return;
  }

  String html;
  html.reserve(6000);

  html += F(
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Available Log Files</title>"
    "<style>"
    "body{font-family:monospace;background:#111;color:#ddd;padding:20px;}"
    "pre{font-family:monospace;font-size:14px;line-height:1.45;}"
    "a{color:#6cf;text-decoration:none;}"
    "a:hover{text-decoration:underline;}"
    "</style></head><body><pre>"
  );

  html += "\n=== Available Log Files ===\n";
  html += "Filename\t\tSize (KB)\tDate\n";
  html += "--------------------------------------------\n";

  File file = root.openNextFile();
  int fileCount = 0;

  while (file) {
    String filename = String(file.name());
    if (filename.endsWith(".log")) {
      size_t fileSize = file.size();

      int dotPos = filename.lastIndexOf('.');
      int slashPos = filename.lastIndexOf('/') + 1;
      String timestampStr = filename.substring(slashPos, dotPos);
      uint32_t timestamp = timestampStr.toInt();

      time_t rawtime = (time_t)timestamp;
      struct tm* timeinfo = gmtime(&rawtime);
      char dateStr[20];

      if (timeinfo != NULL) {
        snprintf(dateStr, sizeof(dateStr), "%02d-%02d-%04d %02d:%02d:%02d",
                 timeinfo->tm_mday,
                 timeinfo->tm_mon + 1,
                 timeinfo->tm_year + 1900,
                 timeinfo->tm_hour,
                 timeinfo->tm_min,
                 timeinfo->tm_sec);
      } else {
        strcpy(dateStr, "Invalid date");
      }

      html += "<a href='/download?name=" + urlEncode(filename) + "'>";
      html += htmlEscape(filename);
      html += "</a>\t";
      html += String(fileSize / 1024.0, 2);
      html += "\t";
      html += dateStr;
      html += "\n";

      fileCount++;
    }
    file = root.openNextFile();
  }

  html += "\nTotal log files: " + String(fileCount) + "\n";
  html += "Free space: " + String((SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024) + " KB\n";
  html += "</pre></body></html>";

  server.send(200, "text/html", html);
}

bool routesRegistered = false;

void registerRoutesOnce() {
  if (routesRegistered) return;

  server.on("/logs", HTTP_GET, handleLogFilesPage);
  server.on("/download", HTTP_GET, handleDownload);

  server.on("/conf.html", HTTP_GET, handleConfPage);
  server.on("/getconf", HTTP_GET, handleGetConf);
  server.on("/setconf", HTTP_POST, handleSetConf);
  server.on("/applyconf", HTTP_GET, handleApplyConf);
  server.on("/rebootdevice", HTTP_GET, handleRebootDevice);

  server.on("/", HTTP_GET, handleRoot);

  routesRegistered = true;
}

void enableWebUi() {
  if (webUiEnabled) return;

  String ssid = makeApSsid();

  char hexBuf[5];
  snprintf(hexBuf, sizeof(hexBuf), "%04X", usrConf.kalman_en);
  String password = "AP-" + String(hexBuf) + "-KEY"; // >= 8 chars, WPA2 compatible

  WiFi.disconnect(true, true); // (WiFi off, erase AP credentials if desired)
  WiFi.mode(WIFI_OFF); 
  delay(100); // Short delay to allow the network stack to clear
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid.c_str(), password.c_str());   // open AP; use password if you want

  registerRoutesOnce();

  server.begin();
  webUiEnabled = true;

  //Set UART to VESC
  setUartMux(0);
  vTaskDelay(pdMS_TO_TICKS(10));
  Serial1.end();
  Serial1.begin(115200, SERIAL_8N1, P_U1_RX, P_U1_TX);
  while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));

  // --- VESC bridge: start after the AP + IP stack are up ---
  vescBridgeBegin(ssid.c_str());

  Serial.println("\nWeb UI enabled");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void disableWebUi() {
  if (!webUiEnabled) return;

  // --- VESC bridge: stop before tearing down WiFi ---
  vescBridgeEnd();

  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  webUiEnabled = false;
  Serial.println("\nWeb UI disabled");
}