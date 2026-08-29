#include "BREmote_V2_Rx.h"

SX1262Fallback radio = new Module(P_LORA_NSS, P_LORA_DIO, P_LORA_RST, P_LORA_BUSY);
Adafruit_AW9523 aw;
Ticker ticksrc;
WebServer server(80);

void setup()
{
  enterSetup();

  Serial.print("Reset reason: ");
  Serial.println(esp_reset_reason());

  Wire.begin(P_I2C_SDA, P_I2C_SCL); //SDA, SCL
  Wire.setClock(400000); // Set to 400 kHz
  startupAW();

  initSPIFFS();
  getConfFromSPIFFS();
  getBCFromSPIFFS();

  startupRadio();
  radio.setPacketReceivedAction(packetReceived);
  radio.implicitHeader(6);
  radio.startReceive();

  delay(100);

  checkButtons();
  if(usrConf.paired == 2) waitForPairing();

  initRMT();

  Serial1.begin(115200, SERIAL_8N1, P_U1_RX, P_U1_TX);

  gpsConfigure();

  safeAwWrite(AP_EN_PWM0, 1);
  safeAwWrite(AP_EN_PWM1, 1);

  triggerReceiveSemaphore = xSemaphoreCreateBinary();
  loopTaskHandle = xTaskGetCurrentTaskHandle();
  
  //Runs every 10ms to generate both PWM signals, high prio
  xTaskCreatePinnedToCore(generatePWM, "Generate_PWM_10ms", 2048, NULL, 10, &generatePWMHandle, 0);
  //Runs upon RF interrupt and reads packet & responds, medium-high prio
  xTaskCreatePinnedToCore(triggeredReceive, "RF_ReceiveTask_triggered", 2048, NULL, 5, &triggeredReceiveHandle, 0);

  //Checks if there is connection and blinks LED, low prio
  xTaskCreatePinnedToCore(checkConnStatus, "Check_conn_staus_200ms", 2048, NULL, 2, &checkConnStatusHandle, 0);

  rxIsrState = 1;

  initLogger();
  if(usrConf.debug_byte & 1<<3)
  {
    Serial.println("Lograte 0.1Hz");
    setLogRate(0.1);
  }
  if(usrConf.debug_byte & 1<<4 && usrConf.gps_en)
  {
    Serial.println("Autostart logging");
    startLog();
  }

  exitSetup();
  PWM_active = 1;
}

unsigned long loop_timer = 0;
int wetness_counter = 0;

void loop()
{
  checkSerial();

  if(millis()-last_packet > 500 && millis() - radioBufferResetTimeout > 5000)
  {
    rxprintln("Emergency Radio Buffer Reset!");
    radioBufferResetTimeout = millis();
    radio.startReceive();
    rfInterrupt = false;
  }

  if(millis()-loop_timer > 1000)
  {
    loop_timer = millis();
    if(usrConf.wet_det_active)
    {
      wetness_counter++;
      if(wetness_counter >= 10)
      {
        checkWetness();
        wetness_counter = 0;
      }
    }
    if(usrConf.bms_det_active)
    {
      if(!safeAwRead(AP_BMS_MEAS))
      {
        if(telemetry.error_code == 0)
        {
        telemetry.error_code = 8;
        }
      }
      else
      {
        if(telemetry.error_code == 8)
        {
        telemetry.error_code = 0;
        }
      }
    }

    if(usrConf.data_src == 1)
    {
      getUbatLoop();
    }
    else if(usrConf.data_src == 2)
    {
      if(!webUiEnabled) getVescLoop();
    }

    if(usrConf.gps_en)
    {
      //Delays 300ms non-blocking
      if(!webUiEnabled) gpsPoll();
    }
  }

  bool shouldEnable = (millis() - last_packet > 10000);

  if (shouldEnable && !webUiEnabled) {
    enableWebUi();
  } else if (!shouldEnable && webUiEnabled) {
    disableWebUi();
  }

  if (webUiEnabled) {
    server.handleClient();
    vescBridgeLoop("BREmoteVESCBridge");
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
