void generatePWM(void *parameter) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(10);

  while (1) 
  {
    calcPWM();

    if(PWM_active && millis()-last_packet < usrConf.failsafe_time)
    {

      if(alternatePWMChannel)
      {
        alternatePWMChannel = 0;
        generate_pulse(PWM0_time);
        vTaskDelay(pdMS_TO_TICKS(2));
        if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
          aw.digitalWrite(AP_EN_PWM0, LOW);
          aw.digitalWrite(AP_EN_PWM1, HIGH);
          xSemaphoreGive(i2cMutex);
        }
      }
      else
      {
        alternatePWMChannel = 1;
        generate_pulse(PWM1_time);
        vTaskDelay(pdMS_TO_TICKS(2));
        if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
          aw.digitalWrite(AP_EN_PWM0, HIGH);
          aw.digitalWrite(AP_EN_PWM1, LOW);
          xSemaphoreGive(i2cMutex);
        }
      }
    }
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

void calcPWM()
{
  if(usrConf.steering_type == 0)
  {
    //Efoil mode
    PWM0_time = constrain(map(thr_received, 0, 255, usrConf.PWM0_min, usrConf.PWM0_max) + usrConf.trim, usrConf.PWM0_min, usrConf.PWM0_max);
    PWM1_time = constrain(map(thr_received, 0, 255, usrConf.PWM1_min, usrConf.PWM1_max) - usrConf.trim, usrConf.PWM1_min, usrConf.PWM1_max);
  }
  else if(usrConf.steering_type == 1)
  {
    //Diff
    // Map throttle input to PWM range for each motor
    uint16_t throttle_0 = map(thr_received, 0, 255, usrConf.PWM0_min, usrConf.PWM0_max);
    uint16_t throttle_1 = map(thr_received, 0, 255, usrConf.PWM1_min, usrConf.PWM1_max);
    
    // Compute differential steering adjustment with influence factor
    int max_steering_offset_0 = map(usrConf.steering_influence, 0, 100, 0, (usrConf.PWM0_max - usrConf.PWM0_min));
    int max_steering_offset_1 = map(usrConf.steering_influence, 0, 100, 0, (usrConf.PWM1_max - usrConf.PWM1_min));
    
    int steering_offset_0 = map(steering_received, 0, 255, -max_steering_offset_0, max_steering_offset_0)+1;
    int steering_offset_1 = map(steering_received, 0, 255, -max_steering_offset_1, max_steering_offset_1)+1;

    if(usrConf.steering_inverted)
    {
      PWM0_time = constrain(throttle_0 + usrConf.trim + steering_offset_0, usrConf.PWM0_min, usrConf.PWM0_max);
      PWM1_time = constrain(throttle_1 - usrConf.trim - steering_offset_1, usrConf.PWM1_min, usrConf.PWM1_max);
    }
    else
    {
      PWM0_time = constrain(throttle_0 + usrConf.trim - steering_offset_0, usrConf.PWM0_min, usrConf.PWM0_max);
      PWM1_time = constrain(throttle_1 - usrConf.trim + steering_offset_1, usrConf.PWM1_min, usrConf.PWM1_max);
    }
  }
  else if(usrConf.steering_type == 2)
  {
    //Servo
    PWM0_time = map(thr_received, 0, 255, usrConf.PWM0_min, usrConf.PWM0_max);
    if(usrConf.steering_inverted)
    {  
      PWM1_time = constrain(map(steering_received, 0, 255, usrConf.PWM1_min, usrConf.PWM1_max)+usrConf.trim, usrConf.PWM1_min, usrConf.PWM1_max);
    }
    else
    {
      PWM1_time = constrain(map(steering_received, 255, 0, usrConf.PWM1_min, usrConf.PWM1_max)+usrConf.trim, usrConf.PWM1_min, usrConf.PWM1_max);
    }
  }
  else
  {
    PWM_active = 0;
  }
}

void initRMT()
{
  // Initialize RMT TX channel
  rmt_tx_channel_config_t tx_chan_config = {
    .gpio_num = RMT_TX_GPIO_NUM,
    .clk_src = RMT_CLK_SRC_DEFAULT,  // Select APB clock (80MHz)
    .resolution_hz = 1000000,         // 1MHz, 1 tick = 1μs
    .mem_block_symbols = 64,
    .trans_queue_depth = 4,
  };

  tx_chan_config.flags.io_od_mode = 0; //open-drain
  // Create RMT TX channel
  ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &tx_channel));
  // Create RMT encoder
  rmt_copy_encoder_config_t copy_encoder_config = {};
  ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &copy_encoder));
  // Enable RMT TX channel
  ESP_ERROR_CHECK(rmt_enable(tx_channel));
}

void generate_pulse(uint16_t pulse_width_us) 
{
  pulse_symbol.level0 = 1;
  pulse_symbol.duration0 = pulse_width_us;  // High time in microseconds
  pulse_symbol.level1 = 0;
  pulse_symbol.duration1 = 1;  // Low time in microseconds
  // Create a transmission that loops the same pattern (creates a continuous signal)
  rmt_transmit_config_t tx_config = {
    .loop_count = 1,  // Infinite loop
  };
  tx_config.flags.eot_level = 0; // End-of-transmission level (LOW)
  // Send the pulse pattern
  ESP_ERROR_CHECK(rmt_transmit(tx_channel, copy_encoder, &pulse_symbol, 
                              sizeof(pulse_symbol), &tx_config));
}