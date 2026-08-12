/*
 * KISS ESC telemetry (115200 baud, 8N1)
 *
 * Frame layout (big endian):
 *   0      temperature in degrees Celsius
 *   1..2   pack voltage in 0.01 V
 *   3..4   current in 0.01 A
 *   5..6   consumed capacity in mAh
 *   7..8   eRPM
 *   9      CRC-8, polynomial 0x07, initial value 0
 *
 * Only temperature and voltage are needed by the BREmote telemetry packet.
 */

static const uint8_t KISS_FRAME_SIZE = 10;
static const unsigned long KISS_PARTIAL_FRAME_TIMEOUT_MS = 100;
static const unsigned long KISS_CONNECTION_TIMEOUT_MS = 3000;

static uint8_t kiss_frame[KISS_FRAME_SIZE];
static uint8_t kiss_frame_length = 0;
static unsigned long kiss_last_byte = 0;
static uint16_t kiss_voltage_cV = 0;
static uint16_t kiss_current_cA = 0;
static uint16_t kiss_consumption_mAh = 0;
static uint16_t kiss_erpm = 0;
static uint32_t kiss_bytes_received = 0;
static uint32_t kiss_valid_frames = 0;
static uint32_t kiss_crc_errors = 0;

uint8_t kissCrc8(const uint8_t *data, size_t length)
{
  uint8_t crc = 0;

  for(size_t i = 0; i < length; i++)
  {
    crc ^= data[i];
    for(uint8_t bit = 0; bit < 8; bit++)
    {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }

  return crc;
}

static bool decodeKissFrame()
{
  if(kissCrc8(kiss_frame, KISS_FRAME_SIZE - 1) != kiss_frame[KISS_FRAME_SIZE - 1])
  {
    kiss_crc_errors++;
    return false;
  }

  kiss_voltage_cV = ((uint16_t)kiss_frame[1] << 8) | kiss_frame[2];
  kiss_current_cA = ((uint16_t)kiss_frame[3] << 8) | kiss_frame[4];
  kiss_consumption_mAh = ((uint16_t)kiss_frame[5] << 8) | kiss_frame[6];
  kiss_erpm = ((uint16_t)kiss_frame[7] << 8) | kiss_frame[8];

  fbatVolt = (float)kiss_voltage_cV / 100.0f;
  telemetry.foil_bat = getUbatPercent(fbatVolt);
  telemetry.foil_temp = kiss_frame[0];
  last_kiss_packet = millis();
  kiss_valid_frames++;

  return true;
}

static void processKissByte(uint8_t value)
{
  kiss_frame[kiss_frame_length++] = value;

  if(kiss_frame_length < KISS_FRAME_SIZE)
  {
    return;
  }

  if(decodeKissFrame())
  {
    kiss_frame_length = 0;
    return;
  }

  // KISS has no sync byte. Shift one byte so a continuous stream re-synchronizes.
  memmove(kiss_frame, kiss_frame + 1, KISS_FRAME_SIZE - 1);
  kiss_frame_length = KISS_FRAME_SIZE - 1;
}

void getKissTelemetryLoop()
{
  setUartMux(0);

  while(Serial1.available())
  {
    const unsigned long now = millis();
    if(kiss_frame_length > 0 && now - kiss_last_byte > KISS_PARTIAL_FRAME_TIMEOUT_MS)
    {
      kiss_frame_length = 0;
    }

    processKissByte((uint8_t)Serial1.read());
    kiss_bytes_received++;
    kiss_last_byte = now;
  }

  if(millis() > KISS_CONNECTION_TIMEOUT_MS &&
     (last_kiss_packet == 0 || millis() - last_kiss_packet > KISS_CONNECTION_TIMEOUT_MS))
  {
    telemetry.foil_bat = 0xFF;
    telemetry.foil_temp = 0xFF;
  }
}

void serPrintKISS()
{
  Serial.println("KISS telemetry monitor (send 'quit' to stop)");
  if(usrConf.data_src != DATA_SRC_KISS_TELEMETRY)
  {
    Serial.println("Warning: data_src is not set to 3; reading RX UART temporarily.");
  }

  unsigned long last_print = 0;

  while(true)
  {
    if(Serial.available() > 0)
    {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if(input.equalsIgnoreCase("quit"))
      {
        Serial.println("Stopping KISS telemetry monitor.");
        break;
      }
    }

    getKissTelemetryLoop();

    const unsigned long now = millis();
    if(now - last_print >= 1000)
    {
      last_print = now;

      Serial.print("KISS: bytes=");
      Serial.print(kiss_bytes_received);
      Serial.print(", frames=");
      Serial.print(kiss_valid_frames);
      Serial.print(", crc_errors=");
      Serial.print(kiss_crc_errors);

      if(last_kiss_packet == 0 || now - last_kiss_packet > KISS_CONNECTION_TIMEOUT_MS)
      {
        Serial.println(", status=no valid frame");
      }
      else
      {
        Serial.print(", voltage=");
        Serial.print((float)kiss_voltage_cV / 100.0f, 2);
        Serial.print(" V, temperature=");
        Serial.print(telemetry.foil_temp);
        Serial.print(" C, current=");
        Serial.print((float)kiss_current_cA / 100.0f, 2);
        Serial.print(" A, consumption=");
        Serial.print(kiss_consumption_mAh);
        Serial.print(" mAh, eRPM=");
        Serial.print(kiss_erpm);
        Serial.print(", age=");
        Serial.print(now - last_kiss_packet);
        Serial.println(" ms");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
