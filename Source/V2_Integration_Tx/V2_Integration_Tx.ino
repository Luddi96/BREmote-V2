#include "BREmote_V2_Tx.h"

SX1262Fallback radio = new Module(P_LORA_NSS, P_LORA_DIO, P_LORA_RST, P_LORA_BUSY);
Adafruit_ADS1115 ads;
Ticker ticksrc;

void setup()
{
  in_setup = true;
  enterSetup();

  Wire.begin(P_I2C_SDA, P_I2C_SCL); //SDA, SCL
  Wire.setClock(400000); // Set to 400 kHz
  startupDisplay();

  initSPIFFS();
  getConfFromSPIFFS();

  if(usrConf.max_gears <= 0) usrConf.max_gears = 1;

  startupADS();

  checkCharger();

  startupRadio();
  radio.setDio1Action(packetReceived);

  //Triggered by senData, waits from telem from Rx after sending data to it
  xTaskCreatePinnedToCore(waitForTelemetry, "wait_for_telem_triggered", 2048, NULL, 4, &triggeredWaitForTelemetryHandle, 0);
  //Runs every 100ms and sends data to Rx, medium prio
  xTaskCreatePinnedToCore(sendData, "Send_Data_100ms", 2048, NULL, 5, &sendDataHandle, 0);
  //Measure, Buffer and calculate inputs
  xTaskCreatePinnedToCore(measBufCalc, "meas_buf_calc_10ms", 2048, NULL, 6, &measBufCalcHandle, 0);
  //Update bargraphs
  xTaskCreatePinnedToCore(updateBargraphs, "update_bargraphs_200ms", 2048, NULL, 6, &updateBargraphsHandle, 0);
  //Vib motor
  xTaskCreatePinnedToCore(vibrationMotor, "vibration_motor_100ms", 2048, NULL, 6, &vibrationMotorHandle, 0);

  //Show boot animation and display internal battery voltage (needed for buffers)
  bootAnimation();
  //Check if calibration is valid, otherwise calibrate
  checkCal();
  //Check if a button combination is input
  checkStartupButtons();
  //Check if system is paired
  checkPairing();

  if(usrConf.no_lock)
  {
    while(thr_scaled > 10)
    {
      advanceArrow();
      delay(100);
    }
    system_locked = 0;
  }

  if(usrConf.no_gear)
  {
    gear = usrConf.max_gears-1;
  }
  else
  {
    if(usrConf.startgear >= usrConf.max_gears) usrConf.startgear = usrConf.max_gears-1;
    gear = usrConf.startgear;
  }

  exitSetup();
  in_setup = false;

  delay(100);
  if(serialOff)
  {
    Serial.end();
    digitalWrite(20, LOW); pinMode(20, OUTPUT);
    digitalWrite(21, LOW); pinMode(21, OUTPUT);
    digitalWrite(9, LOW); pinMode(9, OUTPUT);
  }
}

void loop()
{
  runMenu();
  if(in_menu > 0) in_menu--;

  checkSerial();

  if(millis() - last_packet > 300000)
  {
    deepSleep();
  }

  if(remote_error == 0)
  {
    if(system_locked)
    {
      displayLock();
    }
    else
    {
      if(usrConf.speed_src != 4)
      {
        if(telemetry.foil_speed != 0xFF)
        {
          displayDigits(telemetry.foil_speed/10,telemetry.foil_speed-10*(telemetry.foil_speed/10));
        }
        else
        {
          displayDigits(DASH, DASH);
        }
      }
      else
      {
        uint8_t bat_pct = constrain(telemetry.foil_bat,0,99);
        displayDigits(bat_pct/10,bat_pct-10*(bat_pct/10));
      }
      updateDisplay();
    }
  }
  else
  {
    displayDigits(LET_E, remote_error);
  }
  
  delay(110);
  
} //End of loop()