void startupRadio()
{
  Serial.print("Starting Radio...");

  SPI.begin(P_SPI_SCK, P_SPI_MISO, P_SPI_MOSI);

  //Off for begin(): entering XOSC standby needs a running oscillator, but the
  //TCXO is only powered by setTCXO(), which RadioLib calls afterwards. The
  //rejected command does not always surface, so this works until anything
  //else is added to the sketch and the code layout shifts. Switched back on
  //below, once begin() is through.
  radio.standbyXOSC = false;

  if(usrConf.rf_power < -9 || usrConf.rf_power > 22)
  {
    Serial.println("Error, invalid transmit power");
    while(1) blinkErr(2, AP_L_BIND);
  }

  Serial.print(" Power: ");
  Serial.print(usrConf.rf_power);

  int state;

  if(usrConf.radio_preset == 1)
  {
    Serial.print(" Region: EU868");
                        //          5..12 5..8                                  -9..22             >=1
                        //fc     bw    sf cr                                      pwr              pre tcxo  ldo
    state = radio.begin(869.525, 250.0, 6, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, usrConf.rf_power, 8, 1.8, false);
  }
  else if(usrConf.radio_preset == 2)
  {
    //Reserved for US
    Serial.print(" Region: US/AU915");
                        //          5..12 5..8                                  -9..22             >=1
                        //fc     bw    sf cr                                      pwr              pre tcxo  ldo
    state = radio.begin(915.0, 250.0, 6, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, usrConf.rf_power, 8, 1.8, false);
  }
  else if (usrConf.radio_preset == 3)
  {
    //Reserverd for other country
    Serial.println("Error, unsupported HF setting");
    while(1) blinkErr(3, AP_L_BIND);
  }
  else
  {
    //Invalid
    Serial.println("Error, unsupported HF setting");
    while(1) blinkErr(3, AP_L_BIND);
  }

  //radio.setCurrentLimit(60.0);
  radio.setDio2AsRfSwitch(true);
  radio.implicitHeader(4);
  radio.setCRC(0);
  radio.setRxBandwidth(250);

  //TCXO is up now, so put the fallback mode back to XOSC standby. Without
  //this the chip stops the oscillator after every packet and the Tx misses
  //the reply from the Rx.
  radio.standbyXOSC = true;
  radio.fallbackToXOSC();

  Serial.print(" TOA: ");
  Serial.print(radio.getTimeOnAir(4));

  if (state == RADIOLIB_ERR_NONE) 
  {
    Serial.println(" Done");
  } 
  else 
  {
    Serial.print(" Failed, code: ");
    Serial.println(state);
    while(1) blinkErr(3, AP_L_BIND);
  }
}

void ICACHE_RAM_ATTR packetReceived(void)
{
  if(rxIsrState && !rfInterrupt)
  {
    rfInterrupt = true;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(triggerReceiveSemaphore, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
  else
  {
    rfInterrupt = true;
  }
}

// Function for waiting node to pair
bool waitForPairing() 
{
  usrConf.paired = false;
  
  // First check for address conflicts
  if (0)//!checkAndAdjustAddress()) 
  {
    return false;  // Too many conflicts
  }
  
  uint8_t responsePacket[8];
  unsigned long startTime = millis();

  while (millis() - startTime < PAIRING_TIMEOUT) 
  {
    radio.implicitHeader(5);
    radio.startReceive();
    rxprintln("Waiting for pairing packet...");
    uint8_t buffer[15];
    
    while(!rfInterrupt && millis() - startTime < PAIRING_TIMEOUT) blinkBind(2);
    delay(10);
    if (rfInterrupt && radio.readData(buffer, 15) == RADIOLIB_ERR_NONE) 
    {
      rfInterrupt = false;
      rxprintln("Received response");
      #ifdef DEBUG_RX
      printHexArray(buffer, 15);
      #endif

      if (buffer[0] == 0xAB) 
      {
        rxprintln("1st byte matches");
        // Verify CRC of received packet directly
        uint8_t receivedCRC = buffer[4];
        uint8_t calculatedCRC = esp_crc8(buffer, 4);
        
        if (receivedCRC == calculatedCRC) 
        {
          rxprintln("CRC ok");
          // Store potential partner's address
          uint8_t temp_addr[3];
          memcpy(temp_addr, buffer + 1, 3);
          
          // Prepare response packet
          responsePacket[0] = 0xBA;
          memcpy(responsePacket + 1, temp_addr, 3);
          memcpy(responsePacket + 4, usrConf.own_address, 3);
          
          // Calculate CRC;
          responsePacket[7] = esp_crc8(responsePacket, 7);
          
          delay(100);
          rxprintln("Sending response: ");
          #ifdef DEBUG_RX
          printHexArray(responsePacket, 8);
          #endif
          // Send response
          radio.implicitHeader(8);
          radio.startTransmit(responsePacket, 8);
          delay(10);
          radio.startReceive();
          rfInterrupt = false;

          while (millis() - startTime < PAIRING_TIMEOUT) 
          {
            while(!rfInterrupt && millis() - startTime < PAIRING_TIMEOUT) delay(10);
            delay(10);
            if (rfInterrupt && radio.readData(buffer, 15) == RADIOLIB_ERR_NONE) 
            {
              rfInterrupt = false;
              rxprintln("Received response");
              if (buffer[0] == 0xAC && memcmp(buffer + 1, usrConf.own_address, 3) == 0) 
              {
                rxprintln("Address matches");
                // Verify CRC of final packet directly
                receivedCRC = buffer[7];
                calculatedCRC = esp_crc8(buffer, 7);
                
                if (receivedCRC == calculatedCRC) 
                {
                  rxprintln("CRC ok, pairing success");
                  // Save partner's address
                  memcpy(usrConf.dest_address, buffer + 4, 3);
                  usrConf.paired = true;
                  saveConfToSPIFFS(usrConf);
                  safeAwWrite(AP_L_BIND, LOW);
                  return true;
                }
              }
            }
            else rfInterrupt = false;
          }
        }
      }
    }
    else rfInterrupt = false;
  }  
  return false;
}

void triggeredReceive(void *parameter) {
  while (1)
  {
    // Wait for semaphore from ISR
    if (xSemaphoreTake(triggerReceiveSemaphore, portMAX_DELAY) == pdTRUE) 
    {
      uint8_t rcvArray[6];
      int readLen = -1;
      if (radio.readData(rcvArray, 6) == RADIOLIB_ERR_NONE) {
        readLen = 6;
      }
      if (readLen > 0)
      {
        rxprint("Received packet: ");
        #ifdef DEBUG_RX
        printHexArray(rcvArray,readLen);
        #endif

        if (memcmp(rcvArray, usrConf.own_address, 3) == 0) 
        {
          rxprintln("Address matches");
          bool crcOk = (rcvArray[5] == esp_crc8(rcvArray, 5));
          if (crcOk) 
          {
            last_packet = millis();
            rxprintln("CRC ok");
            rxprint("RSSI: ");
            rxprint(radio.getRSSI());
            rxprint(", SNR: ");
            rxprintln(radio.getSNR());
            // Format (fixed 6B): dest[3], thr8, steer8, crc
            uint8_t thr8 = rcvArray[3];
            uint8_t steer8 = rcvArray[4];
              thr_received = thr8;
              steering_received = (uint8_t)(steer8);

            telemetry.link_quality = getLinkQuality(radio.getRSSI(), radio.getSNR());

            rxprintln("Sending response");
            uint8_t sendArray[6];
            memcpy(sendArray, usrConf.dest_address, 3);
            uint8_t* ptr = (uint8_t*)&telemetry;
            sendArray[3] = telemetry_index;
            sendArray[4] = ptr[telemetry_index];
            telemetry_index++;
            if(telemetry_index >= sizeof(TelemetryPacket))
            {
              telemetry_index = 0;
            }

            sendArray[5] = esp_crc8(sendArray, 5);

            #ifdef DEBUG_RX
            printHexArray(sendArray, 6);
            #endif

            vTaskDelay(pdMS_TO_TICKS(2));
            //digitalWrite(P_UBAT_MEAS, HIGH);

            radio.implicitHeader(6);
            radio.startTransmit(sendArray, 6);
            //digitalWrite(P_UBAT_MEAS, LOW);
            
            //Wait for packet to be fully sent
            uint8_t txWaitTimeout = 0;
            while((!(radio.getIrqFlags() & RADIOLIB_SX126X_IRQ_TX_DONE)) && txWaitTimeout < 100)
            {
              delayMicroseconds(200);
              txWaitTimeout++;
            }
            radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_TX_DONE);
          }
        }
      }
      else
      {
        rxprintln("Rx err");
      }
      
      radio.startReceive();
      rfInterrupt = false;
      //digitalWrite(P_UBAT_MEAS, LOW);
    }
  }
}

int getLinkQuality(float rssi, float snr) {
  // Normalize RSSI: Expected range (-130 dBm to -50 dBm)
  int rssiScore = constrain(map(rssi, -100, -50, 0, 10), 0, 10);

  // Normalize SNR: Typical range (-20 dB to +10 dB)
  int snrScore = constrain(map(snr, -10, 10, 0, 10), 0, 10);

  // Weighted average (adjust weights as needed)
  float combinedScore = (0.7 * rssiScore) + (0.3 * snrScore);

  // Convert to integer and ensure it's in range 0-10
  return constrain(round(combinedScore), 0, 10);
}