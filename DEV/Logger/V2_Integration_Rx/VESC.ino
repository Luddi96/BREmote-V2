
void getVescLoop()
{
  setUartMux(0);
  vTaskDelay(pdMS_TO_TICKS(10));
  Serial1.end();
  Serial1.begin(115200, SERIAL_8N1, P_U1_RX, P_U1_TX);
  while(!Serial1) vTaskDelay(pdMS_TO_TICKS(10));

  if( getValuesSelective(&Serial1) )
  {
    vesc.last_packet = millis();
  }
  get_vesc_timer = millis();

  // Check for VESC connection break
  if(millis() - vesc.last_packet > 20000)
  {
    telemetry.foil_bat = 0xFF;
    telemetry.foil_temp = 0xFF;
  }
}

//total 4.5ms
bool getValuesSelective(Stream* interface)
{
  uint8_t vesc_command[5];
  vesc_command[0] = COMM_GET_VALUES_SELECTIVE;

  //Mask is 32 bit, divided in 4 byte, see "commands.c", line 377 in the VESC Firmware
  //Byte 4:
  #define FET_TEMP 0
  #define MotTemp 1
  #define MotCurrent 2
  #define BatCurrent 3
  #define Duty 6
  #define ERPM 7
  //Byte 3:
  #define BatVolt 0
  #define FaultCode 7

  vesc_command[1] = 0;
  vesc_command[2] = 0;
  vesc_command[3] = (1<<BatVolt) + (1<<FaultCode);
  vesc_command[4] = (1<<FET_TEMP) + (1<<MotCurrent) + (1<<BatCurrent) + (1<<Duty) + (1<<ERPM);

  sendToVESC(vesc_command, 5, interface);

  //For the above values, answer should be 29 byte
  uint8_t message[30];
  
  if(receiveFromVESC(message, interface) != 0)
  {

    int32_t cnt = 5; //Dont care about the Mask    
    vesc.fetTemp = buffer_get_int16(message, &cnt); //Temp * 10 (25,4°C = 254)

    vesc.motCur = buffer_get_int32(message, &cnt); //Current * 100 (1.23A = 123)
    vesc.batCur = buffer_get_int32(message, &cnt); //Current * 100 (1.23A = 123)
    vesc.duty = buffer_get_int16(message, &cnt); //Duty * 10 (25.6% = 256)
	  vesc.erpm = buffer_get_int32(message, &cnt); //ERPM * 1

    vesc.batVolt = buffer_get_int16(message, &cnt); //Voltage * 10 (45.6V = 456)
	  vesc.fault_code = (uint8_t)message[cnt++];
    
    fbatVolt = (float)vesc.batVolt / 10.0;
    telemetry.foil_bat = getUbatPercent(fbatVolt);
    telemetry.foil_temp = (uint8_t)(vesc.fetTemp / 10);

    
    //String sprt = "Fet Temp: " + String(vesc.fetTemp) + "°C, Mot. Curr.: " + String(vesc.motCur) + "A, Inp. Curr: " + String(vesc.batCur) + "A, ERPM: " + String(vesc.erpm) + ", Duty: " + String(vesc.duty) + "%, Volt: " + String(vesc.batVolt) + "V, Fault: " + String(vesc.fault_code);
    //Serial.println(sprt);

    if(usrConf.debug_byte & 1<<0)
    {
      String sprt = "Fet Temp: " + String(vesc.fetTemp/10.0) + "°C, Mot. Curr.: " + String(vesc.motCur/100.0) + "A, Inp. Curr: " + String(vesc.batCur/100.0) + "A, ERPM: " + String(vesc.erpm) + ", Duty: " + String(vesc.duty/10.0) + "%, Volt: " + String(vesc.batVolt/10.0) + "V, Fault: " + String(vesc.fault_code);
      Serial.println(sprt);
    }

    return 1;
  }
  else
  {
    return 0;
  }
}

int receiveFromVESC(uint8_t * buf, Stream* interface)
{
  uint8_t cnt = 0;
  uint8_t eom = 55; // Maximum possible for COMM_GET_VALUES: 2(header) + 50(payload) + 2(CRC) + 1(footer)
  static uint8_t raw_message[55]; // Buffer for COMM_GET_VALUES

  unsigned long started = millis();

  //Read until whole payload is received
  while( ((millis() - started) < 200) && cnt < eom)
  {
    if(interface->available())
    {
      raw_message[cnt] = interface->read();

      if(cnt == 0)
      {
        if(raw_message[0] != 2)
        {
          VESC_DEBUG_PRINTLN(".");
          cnt = 0;
          continue;
        }
      }
      if(cnt == 1)
      {
        if(raw_message[0] == COMM_GET_VALUES) {
          eom = 55;
        } else {
          eom = raw_message[1] + 5; //5 byte overhead pour les autres commandes
        }
        if(eom > sizeof(raw_message)) {
          eom = sizeof(raw_message);
        }
      }
      cnt++;
    }
  }
  
#ifdef DEBUG_VESC
  Serial.println();
  Serial.println();

  for(int i = 0; i < 55; i++)
  {
    Serial.print(raw_message[i], HEX);
    Serial.print(" ");
  }

  Serial.println();
  Serial.println();
#endif

  //Check if End is reached
  if(raw_message[eom-1] == 3)
  {
    uint16_t crcMessage = 0;
    uint16_t crcPayload = 0;

    crcMessage = raw_message[eom - 3] << 8;
    crcMessage &= 0xFF00;
    crcMessage += raw_message[eom - 2];

    //Extract payload and also calculate CRC
    memcpy(buf,&raw_message[2],raw_message[1]);

    crcPayload = vesc_crc16(buf, raw_message[1]);

    if(crcPayload == crcMessage)
    {
      return raw_message[1];
    }
    else
    {
      VESC_DEBUG_PRINTLN("CRC NOK");
      return 0;
    }
  }
  else
  {
    VESC_DEBUG_PRINTLN("Message Error");
    return 0;
  }
}

void sendToVESC(uint8_t * content, int len, Stream* interface)
{
  uint16_t crc = vesc_crc16(content, len);
  uint8_t tosend[16];
  int cnt = 0;

  tosend[cnt++] = 2;
  tosend[cnt++] = len;

  memcpy(tosend + cnt, content, len);

  cnt += len;

  tosend[cnt++] = (uint8_t)(crc >> 8);
  tosend[cnt++] = (uint8_t)(crc & 0xFF);
  tosend[cnt++] = 3;

  interface->write(tosend, cnt);
}
