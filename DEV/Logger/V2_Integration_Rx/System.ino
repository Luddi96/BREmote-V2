void enterSetup()
{
  Serial.begin(115200);
  delay(100);
  Serial.println("");
  Serial.println("Entering Setup...");
  
  Serial.println("**************************************");
  Serial.println("**          BREmote V2 RX           **");
  Serial.printf("**        MAC: %012llX         **\n", ESP.getEfuseMac());
  Serial.printf("**          SW Version: %-10d  **\n", SW_VERSION);
  Serial.printf("**  Compiled: %s %s  **\n", __DATE__, __TIME__);
  Serial.println("**************************************");
}

void exitSetup()
{
  Serial.println("");
  Serial.println("...Leaving Setup");
  Serial.println("");
}

void printHexArray(const uint8_t* buffer, size_t size) {
  for (size_t i = 0; i < size; i++) {
    // Print leading zero for values less than 0x10
    if (buffer[i] < 0x10) {
      Serial.print("0");
    }
    Serial.print(buffer[i], HEX);
    // Add space between bytes (except after the last one)
    if (i < size - 1) {
      Serial.print(" ");
    }
  }
  // Add newline at the end
  Serial.println();
}

uint8_t esp_crc8(uint8_t *data, uint8_t length) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

void startupAW()
{
  i2cMutex = xSemaphoreCreateMutex();
  Serial.print("Starting AW9532...");
  
  if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {

    if (! aw.begin(0x58)) {
      Serial.println("AW9523 not found!");
      while (1) delay(10);  // halt forever
    }

    aw.pinMode(AP_U1_MUX_0, OUTPUT);
    aw.pinMode(AP_U1_MUX_1, OUTPUT);
    aw.pinMode(AP_S_BIND, INPUT);
    aw.pinMode(AP_S_AUX, INPUT);
    aw.pinMode(AP_L_BIND, OUTPUT);
    aw.pinMode(AP_L_AUX, OUTPUT);
    aw.pinMode(AP_EN_BMS_MEAS, OUTPUT);
    aw.pinMode(AP_BMS_MEAS, INPUT);
    aw.pinMode(AP_EN_PWM0, OUTPUT);
    aw.pinMode(AP_EN_PWM1, OUTPUT);
    aw.pinMode(AP_EN_WET_MEAS, OUTPUT);
    aw.pinMode(AP_WET_MEAS, INPUT);

    aw.digitalWrite(AP_L_BIND, HIGH);
    aw.digitalWrite(AP_L_AUX, HIGH);
    aw.digitalWrite(AP_EN_BMS_MEAS, HIGH);
    xSemaphoreGive(i2cMutex);
  }

  Serial.println(" Done");
}

void safeAwWrite(uint8_t pin, uint8_t val) {
  if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
    aw.digitalWrite(pin, val);
    xSemaphoreGive(i2cMutex);
  }
}

// Safe read wrapper
uint8_t safeAwRead(uint8_t pin) {
  uint8_t val = 1; // Default to HIGH in case the mutex fails
  if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
    val = aw.digitalRead(pin);
    xSemaphoreGive(i2cMutex);
  }
  return val;
}

// Timed GPS lock: force GPS channel (1) until deadline
volatile unsigned long g_force_gps_until_ms = 0; // 0 = no active lock
// Timed VESC lock: force VESC channel (0) until deadline
volatile unsigned long g_force_vesc_until_ms = 0; // 0 = no active lock

void gpsMuxLock(uint32_t durationMs)
{
  unsigned long now = millis();
  unsigned long until = now + durationMs;
  // Protect against overflow: if until < now, saturate to now+duration
  if (until == 0) { until = now + durationMs; }
  g_force_gps_until_ms = until;
}

void gpsMuxUnlock()
{
  g_force_gps_until_ms = 0;
  // Force immediate switch to VESC if critical operation arrives
  setUartMux(0);
}

void vescMuxLock(uint32_t durationMs)
{
  unsigned long now = millis();
  unsigned long until = now + durationMs;
  if (until == 0) { until = now + durationMs; }
  g_force_vesc_until_ms = until;
}

void setUartMux(int channel)
{
	// If GPS lock is active and not expired, ignore request to leave GPS channel
	if (channel != 1) {
		if (g_force_gps_until_ms != 0) {
			long rem = (long)(millis() - g_force_gps_until_ms);
			if (rem < 0) {
				return;
			} else {
				g_force_gps_until_ms = 0; // lock expired
			}
		}
	}
	// If VESC lock is active and not expired, ignore request to leave VESC channel
	if (channel != 0) {
		if (g_force_vesc_until_ms != 0) {
			long remv = (long)(millis() - g_force_vesc_until_ms);
			if (remv < 0) {
				return;
			} else {
				g_force_vesc_until_ms = 0; // lock expired
			}
		}
	}
	if(channel == 0)
	{
		safeAwWrite(AP_U1_MUX_0, LOW);
		safeAwWrite(AP_U1_MUX_1, LOW);
	}
	if(channel == 1)
	{
		safeAwWrite(AP_U1_MUX_0, HIGH);
		safeAwWrite(AP_U1_MUX_1, LOW);
	}
}

void checkWetness()
{
  safeAwWrite(AP_EN_WET_MEAS, HIGH);
  vTaskDelay(pdMS_TO_TICKS(50));
  if(!safeAwRead(AP_WET_MEAS))
  {
    uint8_t amt = 0;
    for(uint8_t i = 0; i < 5; i++)
    {
      vTaskDelay(pdMS_TO_TICKS(50));
      amt += safeAwRead(AP_WET_MEAS);
    }
    if(amt == 0)
    {
      if(telemetry.error_code == 0)
      {
       telemetry.error_code = 7;
      }
    }
  }
  else
  {
    if(telemetry.error_code == 7)
    {
      telemetry.error_code = 0;
    }
  }
  safeAwWrite(AP_EN_WET_MEAS, LOW);
}

void getUbatLoop()
{
  uint16_t raw = analogRead(P_UBAT_MEAS);
  raw += analogRead(P_UBAT_MEAS);
  raw += analogRead(P_UBAT_MEAS);

  float vActual = (float)raw*usrConf.ubat_cal;

  telemetry.foil_bat = getUbatPercent(vActual);
}

uint8_t getUbatPercent(float pack_voltage)
{
  if(millis() - percent_last_thr_change > 5000)
  {
    uint8_t thr_state = (thr_received < 10);

    if( thr_state != percent_last_thr)
    {
      //Serial.println("Change detect");
      percent_last_thr = thr_state;
      percent_last_thr_change = millis();
      return percent_last_val;
    }

    uint16_t upackvolt = 0;
    
    if(thr_state)
    {
      float fpackvolt = ((((pack_voltage+usrConf.ubat_offset) / usrConf.foil_num_cells)-2.0-noload_offset) * 100.0);
      if(fpackvolt > 0) upackvolt = (uint16_t)fpackvolt;
      else upackvolt = 0;
    }
    else
    {
      upackvolt = (uint16_t)((((pack_voltage+usrConf.ubat_offset) / usrConf.foil_num_cells)-2.0) * 100.0);
    }

    uint8_t percent_rem = 100;
    while(bc_arr[100-percent_rem] > upackvolt && percent_rem > 0) percent_rem--;

    if(percent_rem < 100 && percent_rem > 0)
    {
      //if previous difference is smaller than current one
      if((upackvolt-bc_arr[100-percent_rem]) > (bc_arr[100-percent_rem-1]-upackvolt))
      {
        //use previous one
        percent_rem += 1;
      }
    }
/*
    Serial.println("Found percent: ");
    Serial.print("volt_in_float: ");
    Serial.print(pack_voltage+usrConf.ubat_offset);
    Serial.print("V, _in_uint: ");
    Serial.println(upackvolt);
    Serial.print("index: ");
    Serial.print(100-percent_rem);
    Serial.print(", remain: ");
    Serial.print(percent_rem);
    Serial.print(" @ ");
    Serial.println(bc_arr[100-percent_rem]);*/

    percent_last_val = percent_rem;
    return percent_rem;
  }
  else
  {
    //Serial.println("Skip_timeout");
    return percent_last_val;
  }
}

void blinkErr(int num, uint8_t pin)
{
  for(int i = 0; i < num; i++)
  {
    safeAwWrite(pin, LOW);
    delay(200);
    safeAwWrite(pin, HIGH);
    delay(200);
  }
  delay(500);
  checkSerial();
}

void blinkBind(int num)
{
  for(int i = 0; i < num; i++)
  {
    safeAwWrite(AP_L_BIND, LOW);
    delay(50);
    safeAwWrite(AP_L_BIND, HIGH);
    delay(50);
  } 
}

void checkSerial()
{
  // Check if data is available on the serial port
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');  // Read input until newline

    // Trim leading and trailing spaces
    command.trim();
    
    // Process the command
    if (command.startsWith("?")) {
      if (command == "?conf") {
        serPrintConf();  // Call function for ?conf
      } 
      else if (command.startsWith("?setConf")) {
        String data = command.substring(command.indexOf(":") + 1);  // Extract everything after ":"
        serSetConf(data);  // Call function for ?setconf with the string after ":"
      }
      else if (command.startsWith("?setBC")) {
        String data = command.substring(command.indexOf(":") + 1);  // Extract everything after ":"
        serSetBC(data);  // Call function for ?setconf with the string after ":"
      } 
      else if (command == "?clearSPIFFS") {
        serClearConf();  // Call function for ?clearSPIFFS
      }
      else if (command == "?clearBC") {
        serClearBC();  // Call function for ?clearSPIFFS
      }
      else if (command == "?applyConf") {
        serApplyConf();  // Call function for ?clearSPIFFS
      }
      else if (command == "?reboot")
      {
        Serial.println("Rebooting now...");
        delay(1000);
        ESP.restart();
      }
      else if(command == "?printPWM")
      {
        serPrintPWM();
      }
      else if(command == "?debugPWM")
      {
        Serial.println("=== PWM Debug ===");
        Serial.printf("steering_type: %u (0=single, 1=diff, 2=servo)\n", usrConf.steering_type);
        Serial.printf("PWM0_min: %u µs\n", usrConf.PWM0_min);
        Serial.printf("PWM0_max: %u µs\n", usrConf.PWM0_max);
        Serial.printf("PWM1_min: %u µs\n", usrConf.PWM1_min);
        Serial.printf("PWM1_max: %u µs\n", usrConf.PWM1_max);
        Serial.printf("trim: %d\n", usrConf.trim);
        Serial.printf("steering_influence: %u%%\n", usrConf.steering_influence);
        Serial.printf("steering_inverted: %u\n", usrConf.steering_inverted);
        Serial.printf("Current thr_received: %u\n", thr_received);
        Serial.printf("Current steering_received: %u\n", steering_received);
        Serial.printf("Current PWM0_time: %u µs\n", PWM0_time);
        Serial.printf("Current PWM1_time: %u µs\n", PWM1_time);
        Serial.printf("\n--- Failsafe Debug ---\n");
        Serial.printf("failsafe_time: %u ms\n", usrConf.failsafe_time);
        Serial.printf("last_packet: %lu ms ago\n", millis() - last_packet);
        Serial.printf("PWM_active: %d\n", PWM_active);
        Serial.printf("Failsafe triggered: %s\n", 
                      (millis() - last_packet > usrConf.failsafe_time) ? "YES" : "NO");
      }
      else if(command == "?printRSSI")
      {
        serPrintRSSI();
      }
      else if(command == "?printTasks")
      {
        serPrintTasks();
      }
      else if(command == "?gpsDiag")
      {
        gpsDiagnosticFull();
      }
      //else if(command == "?fmTest")
      else if(command == "?debug")
      {
        //debugMenu();  // Call the debug menu function
      }
      else if(command == "?printBat")
      {
        serPrintBat();
      }
      //else if (command == "?testHITL") {
        
      //else if (command.startsWith("?enableHITL") ||
        
      else if(command == "?testBG")
      {
        readTelemetryUntilQuit();
      }
      else if(command == "?testPercent")
      {
        testPercent();
      }

      else if (command == "?list") {
          listLogFiles();
      } 
      else if (command.startsWith("?download ")) {
          String filename = command.substring(10);
          downloadLogFile(filename.c_str());
      }
      else if (command.startsWith("?delete ")) {
        String filename = command.substring(8);
        deleteLogFile(filename.c_str());
      }
      else if (command == "?start") {
          startLog();
      } 
      else if (command == "?stop") {
          stopLog();
      } 
      else if (command.startsWith("?rate ")) {
          float rate = command.substring(5).toFloat();
          setLogRate(rate);
      }

      else if (command.startsWith("?dbit")) {
          if(command.substring(5).length() == 0)
          {
            Serial.println("--------------------------------");
            Serial.println("Bit | State | Meaning         ");
             Serial.printf(" 0  |   %d   | VESC_Dbg_Output \n", (bool)(usrConf.debug_byte & 1<<0));
             Serial.printf(" 1  |   %d   | GPS_Dbg_Output  \n", (bool)(usrConf.debug_byte & 1<<1));
             Serial.printf(" 2  |   %d   | Log_Dbg_Output  \n", (bool)(usrConf.debug_byte & 1<<2));
             Serial.printf(" 3  |   %d   | Log_Rate_1/0.1Hz\n", (bool)(usrConf.debug_byte & 1<<3));
             Serial.printf(" 4  |   %d   | Log_Auto_Start  \n", (bool)(usrConf.debug_byte & 1<<4));
             Serial.println("--------------------------------");
             Serial.println("Enter ?dbit <bit_no> to flip it");
          }
          else
          {
            int bit = command.substring(5).toInt();
            if(bit >= 0 && bit <= 15)
            {
              usrConf.debug_byte ^= (1<<bit);
              Serial.print("Changed bit "); Serial.println(bit);
              saveConfToSPIFFS(usrConf);
            }
            else
            {
              Serial.println("Bit chosen out of range 0...15!");
            }
          }
      }

      else if (command.startsWith("?setPassword")) {
        String data = command.substring(command.indexOf(":") + 1);  // Extract everything after ":"
        serSetPassword(data);
      }
      else if (command == "?password") {
        serPrintPassword();
      }
      
      else if (command == "?") {
          // List all possible inputs
          Serial.println("Possible commands:");
          Serial.println("?conf - print info, usrConf");
          Serial.println("?setConf:<data> - write B64 to SPIFFS");
          Serial.println("?setBC:<data> - write B64 to SPIFFS");
          Serial.println("?applyConf - read conf from SPIFFS and write to usrConf");
          Serial.println("?clearSPIFFS - Clear usrConf from SPIFFS");
          Serial.println("?clearBC - Clear batcal from SPIFFS");
          Serial.println("?reboot - Reboot the remote");
          Serial.println("?printPWM - Print PWM values until sent 'quit'");
          Serial.println("?printBat - Print analog Bat voltage until sent 'quit'");

          Serial.println("?list - List log files on SPIFFS/SD");
          Serial.println("?download <filename> - Download a log file");
          Serial.println("?delete <filename> - Delete a log file");
          Serial.println("?start - Start logging");
          Serial.println("?stop - Stop logging");
          Serial.println("?rate <hz> - Set log rate");

          Serial.println("?dbit - Show debug_byte bit table");
          Serial.println("?dbit <bit_no> - Flip a debug_byte bit (0-15)");

          Serial.println("?setPassword:xxxx - Set 4-digit hex password (0-9, A-F)");
          Serial.println("?password - Print current password");
      }
      else {
        Serial.println("Unknown command. Type '?' for help.");
      }
    }
    else {
      Serial.println("Unknown command. Type '?' for help.");
    }
  }
}

void serSetPassword(String data) {
  data.trim();

  if (data.length() != 4) {
    Serial.println("The password must consist of 4 characters, from the following set: 0-9, A-F");
    return;
  }

  data.toUpperCase();

  for (int i = 0; i < 4; i++) {
    char c = data[i];
    bool isValidHex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
    if (!isValidHex) {
      Serial.println("The password must consist of 4 characters, from the following set: 0-9, A-F");
      return;
    }
  }

  uint16_t value = (uint16_t)strtoul(data.c_str(), nullptr, 16);
  usrConf.kalman_en = value;

  Serial.print("Password set to: AP-");
  Serial.print(data);
  Serial.println("-KEY");

  saveConfToSPIFFS(usrConf);
}

void serPrintPassword() {
  char buf[5];
  snprintf(buf, sizeof(buf), "%04X", usrConf.kalman_en);
  Serial.print("Password: AP-");
  Serial.print(buf);
  Serial.println("-KEY");
}

void testPercent()
{
  while(1)
  {
    if (Serial.available()) {
      String input = Serial.readStringUntil('\n'); // read until newline
      input.trim(); // remove spaces and newlines

      if (input.equalsIgnoreCase("quit")) {
        Serial.println("Quit command received. Stopping input loop.");
        break;
      }

      // Try to parse float
      float value = input.toFloat();

      // Validate: toFloat returns 0.0 if invalid, so check original string too
      if (input.length() == 0 || (value == 0.0f && !input.startsWith("0"))) {
        Serial.println("Invalid input. Please enter a float or 'quit'.");
      } else if (value >= 0.0f && value <= 100.0f) {
        Serial.println(getUbatPercent(value));
      } else {
        Serial.println("Value out of range (0.0 - 100.0).");
      }
    }
  }
}

void readTelemetryUntilQuit() {
    while (true) {
        if (Serial.available()) {
            String input = Serial.readStringUntil('\n');  // read line
            input.trim();  // remove CR/LF/whitespace

            if (input.equalsIgnoreCase("quit")) {
                Serial.println("Exiting telemetry read loop.");
                break; // stop the function
            }

            // Parse values separated by commas
            int firstComma = input.indexOf(',');
            int secondComma = input.indexOf(',', firstComma + 1);

            if (firstComma < 0 || secondComma < 0) {
                Serial.println("Error: Expected 3 values separated by commas.");
                continue; // wait for next line
            }

            String val1 = input.substring(0, firstComma);
            String val2 = input.substring(firstComma + 1, secondComma);
            String val3 = input.substring(secondComma + 1);

            int bat  = constrain(val1.toInt(), 0, 255);
            int temp = constrain(val2.toInt(), 0, 255);
            int link = constrain(val3.toInt(), 0, 255);

            telemetry.foil_bat     = (uint8_t)bat;
            telemetry.foil_temp    = (uint8_t)temp;
            telemetry.link_quality = (uint8_t)link;

            Serial.print("Updated telemetry -> ");
            Serial.print("Bat: "); Serial.print(telemetry.foil_bat);
            Serial.print(" Temp: "); Serial.print(telemetry.foil_temp);
            Serial.print(" Link: "); Serial.println(telemetry.link_quality);
        }
    }
}

void serPrintGPS()
{
  gpsPrintStatus();
}

void serPrintBat()
{
  while (true) 
  {
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n'); // Read the input command
        input.trim(); // Remove any whitespace or newline characters
        
        // If the received command matches the stop command, exit the loop
        if (input.equals("quit")) {
            Serial.println("Stopping print loop.");
            break;
        }
    }

    if(usrConf.data_src == 1)
    {
      getUbatLoop();
    }
    else if(usrConf.data_src == 2)
    {
      getVescLoop();
    }

    if(usrConf.data_src == 1)
    {
      uint16_t raw = analogRead(P_UBAT_MEAS);
      raw += analogRead(P_UBAT_MEAS);
      raw += analogRead(P_UBAT_MEAS);

      float vActual = (float)raw*usrConf.ubat_cal;
      Serial.print("Measured: ");
      Serial.print(vActual);
      Serial.print("V, offset: ");
      Serial.print(usrConf.ubat_offset);
      Serial.print("V, final: ");
      Serial.println(vActual + usrConf.ubat_offset);
    }
    else if(usrConf.data_src == 2)
    {
      getVescLoop();
      Serial.print("Measured: ");
      Serial.print(fbatVolt);
      Serial.print("V, offset: ");
      Serial.print(usrConf.ubat_offset);
      Serial.print("V, final: ");
      Serial.println(fbatVolt + usrConf.ubat_offset);
    }
    else
    {
      Serial.println("data_src not selected! Exiting...");
      break;
    }
    
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void serApplyConf()
{
  Serial.print("Reading conf from SPIFFS and applying to usrConf");
  readConfFromSPIFFS(usrConf);
}

void serSetConf(String data) {
  Serial.print("Setting configuration to: ");
  Serial.println(data);
  
  uint8_t* encodedData = new uint8_t[data.length()];
  // Call the function to fill the encodedData array
  for (size_t i = 0; i < data.length(); i++) {
    encodedData[i] = data[i];  // Convert each character to uint8_t
  }

  // Save to SPIFFS
  File file = SPIFFS.open(CONF_FILE_PATH, FILE_WRITE);
  if (!file) {
      Serial.println("Failed to open file for writing");
      delete[] encodedData;
      return;
  }
  file.write(encodedData, data.length());
  file.close();
  Serial.println("Struct saved to SPIFFS as Base64");
  delete[] encodedData;
}

void serSetBC(String data) {
  Serial.print("Setting batcal to: ");
  Serial.println(data);
  
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
  Serial.println("Batcal saved to SPIFFS as Base64");
  delete[] encodedData;
}

void serClearConf()
{
  Serial.println("Deleting conf from SPIFFS");
  deleteConfFromSPIFFS();
}

void serClearBC()
{
  Serial.println("Deleting batcal from SPIFFS");
  deleteBCFromSPIFFS();
}

void serPrintTasks()
{
  while (true) 
  {
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n'); // Read the input command
        input.trim(); // Remove any whitespace or newline characters
        
        // If the received command matches the stop command, exit the loop
        if (input.equals("quit")) {
            Serial.println("Stopping print loop.");
            break;
        }
    }

    Serial.println("\n=== Task Stack Usage ===");

    Serial.printf("receive stack left: %u words\n", uxTaskGetStackHighWaterMark(triggeredReceiveHandle));
    Serial.printf("pwm stack left: %u words\n", uxTaskGetStackHighWaterMark(generatePWMHandle));
 //   Serial.printf("followme stack left: %u words\n", uxTaskGetStackHighWaterMark(followMeComputeHandle));
    Serial.printf("check_conn stack left: %u words\n", uxTaskGetStackHighWaterMark(checkConnStatusHandle));
    
    // Add logger stack if task exists
/*    extern TaskHandle_t loggerTaskHandle;
    if (loggerTaskHandle != NULL) {
      Serial.printf("logger stack left: %u words\n", uxTaskGetStackHighWaterMark(loggerTaskHandle));
    } else {
      Serial.println("logger stack: task not created");
    }*/
    
    Serial.printf("loop() stack left: %u words\n", uxTaskGetStackHighWaterMark(loopTaskHandle));

    Serial.println("========================\n");

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void serPrintRSSI()
{
  while (true) 
  {
    //checkForPacket();
    // Check if data is available on Serial
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n'); // Read the input command
        input.trim(); // Remove any whitespace or newline characters
        
        // If the received command matches the stop command, exit the loop
        if (input.equals("quit")) {
            Serial.println("Stopping print loop.");
            break;
        }
    }
    
    // Print the variable
    if(millis() - last_packet < usrConf.failsafe_time)
    {
      Serial.print("RSSI: ");
      Serial.print(radio.getRSSI());
      Serial.print(", SNR: ");
      Serial.println(radio.getSNR());
    }
    else
    {
      Serial.print("Failsafe since (ms) ");
      Serial.println(millis()-last_packet);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void serPrintPWM()
{
  while (true) 
  {
    //checkForPacket();
    // Check if data is available on Serial
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n'); // Read the input command
        input.trim(); // Remove any whitespace or newline characters
        
        // If the received command matches the stop command, exit the loop
        if (input.equals("quit")) {
            Serial.println("Stopping print loop.");
            break;
        }
    }
    
    // Print the variable
    Serial.print("PWM0: ");
    Serial.print(PWM0_time);
    Serial.print(" us, PWM1: ");
    Serial.print(PWM1_time);
    Serial.print(" us | thr_rx: ");
    Serial.print(thr_received);
    Serial.print(", steer_rx: ");
    Serial.println(steering_received);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void serPrintConf()
{
  Serial.println("**************************************");
  Serial.println("**          BREmote V2 RX           **");
  Serial.printf("**        MAC: %012llX         **\n", ESP.getEfuseMac());
  Serial.printf("**          SW Version: %-10d  **\n", SW_VERSION);
  Serial.printf("**  Compiled: %s %s  **\n", __DATE__, __TIME__);
  Serial.println("**************************************");

  // Read file from SPIFFS
  File file = SPIFFS.open(CONF_FILE_PATH, FILE_READ);
  if (!file) {
      Serial.println("Failed to open file for reading");
  }

  String encodedString = file.readString();
  Serial.println("Encoded Data Read: " + encodedString);
  file.close();

  printConfStruct(usrConf);
}

void checkButtons()
{
  if(!safeAwRead(AP_S_BIND))
  {
    if(!safeAwRead(AP_S_AUX))
    {
      delay(10);
      if(!safeAwRead(AP_S_AUX))
      {
        Serial.println("Deleting config and rebooting");
        deleteConfFromSPIFFS();
        delay(1000);
        ESP.restart();
      }
    }
    delay(10);
    if(!safeAwRead(AP_S_BIND))
    {
      //Start pairing
      waitForPairing();
    }
  }
}

void checkConnStatus(void *parameter) 
{
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(200);

  while (1) 
  {
    if(usrConf.paired)
    {
      if(millis() - last_packet < usrConf.failsafe_time)
      {
        if(bind_pin_state != 1)
        {
          bind_pin_state = 1;
          safeAwWrite(AP_L_BIND, LOW);
        }
      }
      else
      {
        if(bind_pin_state)
        {
          bind_pin_state = 0;
          safeAwWrite(AP_L_BIND, HIGH);
        }
        else
        {
          bind_pin_state = 1;
          safeAwWrite(AP_L_BIND, LOW);
        }
      }
    }
    else
    {
      unpairedBlink++;
      if(unpairedBlink == 4)
      {
        unpairedBlink = 0;
        safeAwWrite(AP_L_BIND, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
        safeAwWrite(AP_L_BIND, HIGH);
      }
    }
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

void printConfStruct(const confStruct &data) {
    Serial.println("Configuration Struct Values:");
    Serial.print("Version: "); Serial.println(data.version);

    Serial.print("Radio Preset: "); Serial.println(data.radio_preset);
    Serial.print("RF Power: "); Serial.println(data.rf_power);

    Serial.print("Steering Type: "); Serial.println(data.steering_type);
    Serial.print("Steering Influence: "); Serial.println(data.steering_influence);
    Serial.print("Steering Inverted: "); Serial.println(data.steering_inverted);
    Serial.print("Trim: "); Serial.println(data.trim);

    Serial.print("PWM0 Min: "); Serial.println(data.PWM0_min);
    Serial.print("PWM0 Max: "); Serial.println(data.PWM0_max);
    Serial.print("PWM1 Min: "); Serial.println(data.PWM1_min);
    Serial.print("PWM1 Max: "); Serial.println(data.PWM1_max);

    Serial.print("Failsafe Time: "); Serial.println(data.failsafe_time);

    Serial.print("Foil Battery Number of Cells: "); Serial.println(data.foil_num_cells);

    Serial.print("BMS Detection Active: "); Serial.println(data.bms_det_active);
    Serial.print("Water Detection Active: "); Serial.println(data.wet_det_active);

    Serial.print("Data Source: "); Serial.println(data.data_src);
    Serial.print("GPS Enabled: "); Serial.println(data.gps_en);
    Serial.print("Follow-me Mode: "); Serial.println(data.followme_mode);
    Serial.print("Kalman Filter Enabled: "); Serial.println(data.kalman_en);
    Serial.print("Boogie Max Speed: "); Serial.println(data.boogie_vmax_in_followme_kmh);
    Serial.print("Min Distance: "); Serial.println(data.min_dist_m);
    Serial.print("Follow-me Smoothing Band: "); Serial.println(data.followme_smoothing_band_m);
    Serial.print("Zone Angle Enter: "); Serial.println(data.zone_angle_enter_deg);
    Serial.print("Zone Angle Exit: "); Serial.println(data.zone_angle_exit_deg);
    Serial.print("Near Diagonal Offset: "); Serial.println(data.near_diag_offset_deg);
    Serial.print("TX GPS Stale Timeout (ms): "); Serial.println(data.tx_gps_stale_timeout_ms);
    Serial.print("Battery Calibration (Ubat Cal): "); Serial.println(data.ubat_cal, 15);
    Serial.print("Logger Enabled: "); Serial.println(data.logger_en);
    Serial.print("Paired: "); Serial.println(data.paired);

    Serial.print("Own Address: ");
    for (int i = 0; i < 3; i++) {
        Serial.print(data.own_address[i], HEX);
        Serial.print(i < 2 ? ":" : "\n");
    }

    Serial.print("Destination Address: ");
    for (int i = 0; i < 3; i++) {
        Serial.print(data.dest_address[i], HEX);
        Serial.print(i < 2 ? ":" : "\n");
    }

    Serial.println("----------------------");
}