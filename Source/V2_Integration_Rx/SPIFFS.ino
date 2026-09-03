void initSPIFFS()
{
  Serial.print("Initializing SPIFFS...");
  if(!SPIFFS.begin(false)) 
  {
    Serial.println("SPIFFS Mount Failed, Formatting Flash");
    safeAwWrite(AP_L_AUX, LOW);
    if (!SPIFFS.format())
    {
      Serial.println("FORMAT ERROR!");
      while(1) blinkErr(3, AP_L_AUX);
    }
    safeAwWrite(AP_L_AUX, HIGH);
    Serial.println("Rebooting..");
    delay(200);
    esp_restart();
  }
  Serial.println("Done");
}

void getConfFromSPIFFS()
{
  #ifdef DELETE_SPIFFS_CONF_AT_STARTUP
  deleteConfFromSPIFFS();
  #endif
  
  Serial.println("Getting usr conf from SPIFFS...");
  if (readConfFromSPIFFS(usrConf)) 
  {
    if(SW_VERSION != usrConf.version)
    {
      Serial.println("Error, version mismatch! Update firmware!");
      Serial.print("Config version: ");
      Serial.print(usrConf.version);
      while(1) blinkErr(2, AP_L_AUX);
    }
  }
  else
  {
    Serial.println("No conf in SPIFFS, writing default...");
    //Generate Device Address

    uint64_t mac = ESP.getEfuseMac(); // Get MAC from ESP32 eFuse
    uint8_t mac_address[6];
    for (int i = 0; i < 6; i++) {
        mac_address[i] = (mac >> (8 * i)) & 0xFF;
    }

    defaultConf.own_address[0] = esp_crc8(mac_address, 4); // Compute CRC8 over the first 4 bytes
    defaultConf.own_address[1] = mac_address[4];
    defaultConf.own_address[2] = mac_address[5];

    saveConfToSPIFFS(defaultConf);
    if (!readConfFromSPIFFS(usrConf)) 
    {
      Serial.println("Error wiriting default conf!");
      while(1) blinkErr(4, AP_L_AUX);
    }
  }
  Serial.println("... Done");
}

void saveConfToSPIFFS(const confStruct& data) {
    // Convert struct to byte array
    uint8_t rawData[sizeof(confStruct)];
    memcpy(rawData, &data, sizeof(confStruct));

    // Base64 encode
    size_t encodedLen = 0;
    mbedtls_base64_encode(NULL, 0, &encodedLen, rawData, sizeof(confStruct));
    uint8_t* encodedData = new uint8_t[encodedLen];
    if (mbedtls_base64_encode(encodedData, encodedLen, &encodedLen, rawData, sizeof(confStruct)) != 0) {
        Serial.println("Base64 encoding failed");
        delete[] encodedData;
        return;
    }

    // Save to SPIFFS
    File file = SPIFFS.open(CONF_FILE_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("Failed to open file for writing");
        delete[] encodedData;
        return;
    }
    file.write(encodedData, encodedLen);
    file.close();
    Serial.println("Struct saved to SPIFFS as Base64");
    Serial.println("Encoded Data: " + String((char*)encodedData));
    delete[] encodedData;
}

bool readConfFromSPIFFS(confStruct& data) {
    if (!SPIFFS.exists(CONF_FILE_PATH)) {
        Serial.println("File does not exist");
        return false;
    }

    // Read file from SPIFFS
    File file = SPIFFS.open(CONF_FILE_PATH, FILE_READ);
    if (!file) {
        Serial.println("Failed to open file for reading");
        return false;
    }

    String encodedString = file.readString();
    Serial.println("Encoded Data Read: " + encodedString);
    file.close();

    // Decode Base64
    size_t decodedLen = 0;
    mbedtls_base64_decode(NULL, 0, &decodedLen, (const uint8_t*)encodedString.c_str(), encodedString.length());
    uint8_t* decodedData = new uint8_t[decodedLen];
    if (mbedtls_base64_decode(decodedData, decodedLen, &decodedLen, (const uint8_t*)encodedString.c_str(), encodedString.length()) != 0) {
        Serial.println("Base64 decoding failed");
        delete[] decodedData;
        return false;
    }

    memcpy(&data, decodedData, sizeof(confStruct));
    delete[] decodedData;
    Serial.println("Struct successfully read from SPIFFS");
    return true;
}

void deleteConfFromSPIFFS() {
    if (SPIFFS.remove(CONF_FILE_PATH)) {
        Serial.println("File deleted successfully");
    } else {
        Serial.println("Failed to delete file");
    }
}

void getBCFromSPIFFS()
{  
  Serial.println("Getting bat conf from SPIFFS...");
  if (readBCFromSPIFFS()) 
  {

  }
  else
  {
    Serial.println("No batconf in SPIFFS, writing default...");
    //Generate Device Address

    String data = "ANza2dfV1NLQz83LycjGxMPBv768uri3tbOysK6sq6mnpqSioZ+dm5qYlpWTkZCOjIqJh4WEgoB/fXt5eHZ0c3FvbmxqaGdlY2JgXl1bWVdWVFJRT01LSkhGRUNBQD48Ojk3NTQy";
    uint8_t* encodedData = new uint8_t[data.length()];
    // Call the function to fill the encodedData array
    for (size_t i = 0; i < data.length(); i++) {
      encodedData[i] = data[i];  // Convert each character to uint8_t
    }

    // Save to SPIFFS
    File file = SPIFFS.open(BC_FILE_PATH, FILE_WRITE);
    if (!file) {
        Serial.println("Failed to open file for writing");
        delete[] encodedData;
        return;
    }
    file.write(encodedData, data.length());
    file.close();
    Serial.println("BC saved to SPIFFS as Base64");
    delete[] encodedData;

    if (!readBCFromSPIFFS()) 
    {
      Serial.println("Error wiriting bat conf!");
      while(1) blinkErr(4, AP_L_AUX);
    }
  }
  Serial.println("... Done");
}

bool readBCFromSPIFFS() {
    if (!SPIFFS.exists(BC_FILE_PATH)) {
        Serial.println("File does not exist");
        return false;
    }

    // Read file from SPIFFS
    File file = SPIFFS.open(BC_FILE_PATH, FILE_READ);
    if (!file) {
        Serial.println("Failed to open file for reading");
        return false;
    }

    String encodedString = file.readString();
    Serial.println("Encoded Data Read: " + encodedString);
    file.close();

    // Decode Base64
    size_t decodedLen = 0;
    mbedtls_base64_decode(NULL, 0, &decodedLen, (const uint8_t*)encodedString.c_str(), encodedString.length());
    uint8_t* decodedData = new uint8_t[decodedLen];
    if (mbedtls_base64_decode(decodedData, decodedLen, &decodedLen, (const uint8_t*)encodedString.c_str(), encodedString.length()) != 0) {
        Serial.println("Base64 decoding failed");
        delete[] decodedData;
        return false;
    }

    noload_offset = ((float)decodedData[0]) / 500.0;
    memcpy(bc_arr, &decodedData[1], 101 * sizeof(decodedData[0]));

    Serial.print("batcal: noload_offset: ");
    Serial.print(noload_offset);
    Serial.print("V, 100% rem. @ ");
    Serial.print((float)bc_arr[0] / 100.0 + 2.0);
    Serial.print("V, 0% rem. @ ");
    Serial.print((float)bc_arr[100] / 100.0 + 2.0);
    Serial.println("V.");

    delete[] decodedData;
    Serial.println("Struct successfully read from SPIFFS");
    return true;
}

void deleteBCFromSPIFFS() {
    if (SPIFFS.remove(BC_FILE_PATH)) {
        Serial.println("File deleted successfully");
    } else {
        Serial.println("Failed to delete file");
    }
}