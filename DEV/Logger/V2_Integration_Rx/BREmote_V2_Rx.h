#ifndef BREMOTE_V2_RX_H
#define BREMOTE_V2_RX_H

/*
** Includes
*/
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <RadioLib.h> //V7.1.2
#include <Wire.h>
#include <Adafruit_AW9523.h> //V1.0.5, BusIO 1.17.0
#include "driver/rmt_tx.h"
#define RMT_TX_GPIO_NUM  GPIO_NUM_9
#include <Ticker.h>
#include "FS.h"
#include "SPIFFS.h"
#include "mbedtls/base64.h"

#include "vesc_datatypes.h"
#include "vesc_buffer.h"
#include "vesc_crc.h"

#include <WiFi.h>
#include <WebServer.h>

/*
** begin() writes the Rx/Tx fallback mode from standbyXOSC and there is no
** setter for it afterwards, so it is written again once begin() is through.
** See startupRadio() for why begin() itself has to run with the flag off.
*/
class SX1262Fallback : public SX1262 {
  public:
    SX1262Fallback(Module* m) : SX1262(m) {}
    int16_t fallbackToXOSC() {
      uint8_t data = RADIOLIB_SX126X_RX_TX_FALLBACK_MODE_STDBY_XOSC;
      return this->getMod()->SPIwriteStream(RADIOLIB_SX126X_CMD_SET_RX_TX_FALLBACK_MODE, &data, 1);
    }
};

#define SW_VERSION 2
const char* CONF_FILE_PATH = "/data.txt";
const char* BC_FILE_PATH = "/batconf.txt";

/*
** Structs
*/
struct confStruct {
  //Version
  uint16_t version;
  
  uint16_t radio_preset; //1: 868MHz (EU), 2: 915MHz (US/AU)
  int16_t rf_power; //Tx power from -9 to 22

  uint16_t steering_type; //0: single motor, 1: diff motor, 2: servo
  uint16_t steering_influence; //How much (percentually) the steering influences the motor speeds
  uint16_t steering_inverted; //If steering is inverted or not
  int16_t trim; //Trim the steering

  //PWM min and max
  uint16_t PWM0_min;
  uint16_t PWM0_max;
  uint16_t PWM1_min;
  uint16_t PWM1_max;

  uint16_t failsafe_time; //Time after last packet until failsafe

  //Foil battery voltage settings
  uint16_t foil_num_cells; //Amount of cells in series e.g. 14 for a "14SxP" pack

  //Sensors
  uint16_t bms_det_active;
  uint16_t wet_det_active;

  //For now: Debug
  //Bit   Use             States
  // 7
  // 6
  // 5
  // 4  Log_Auto       0:no, 1:autostart
  // 3  Log_Rate       0:1Hz, 1: 0.1Hz
  // 2  Log_Dbg        0:off, 1:on
  // 1  GPS_Dbg        0:off, 1:on
  // 0  VESC_Dbg       0:off, 1:on
  // -> GPS, Log_Dbg @ 1Hz: 6, GPS, Log_Dbg @ 0.1Hz: 14
  uint16_t debug_byte;

  //UART config
  uint16_t data_src; //0: off, 1:analog, 2: VESC UART

  // GPS features related flags
  uint16_t gps_en;         // GPS runtime enable flag (0=disabled, 1=enabled)
  uint16_t followme_mode;  // Follow-me runtime mode flag (0=disabled, 1=behind, 2=near_right, 3=near_left)
  uint16_t kalman_en;      // Kalman filter runtime enable flag (0=disabled, 1=enabled)

  //Follow-me
  float boogie_vmax_in_followme_kmh; // Maximum boogie speed in follow-me mode (km/h)
  float min_dist_m; // minimum allowed distance to the foiler
  float followme_smoothing_band_m; // smoothing band above min distance
  float foiler_low_speed_kmh; // low-speed threshold for safety stop (hysteresis)
  float zone_angle_enter_deg; // Half-angle for zone entry (deg)
  float zone_angle_exit_deg;  // Half-angle for zone exit (deg)
  float near_diag_offset_deg; // Offset from behind for NEAR modes (deg)
  
  //System parameters
  float ubat_cal; //ADC to volt cal for bat meas
  float ubat_offset; //Offset to add to analog/vesc measurement

  uint16_t tx_gps_stale_timeout_ms; // TX GPS data stale timeout (ms)

  //Logger
  uint16_t logger_en; // BREmote Logger runtime enable flag (0=disabled, 1=enabled)

  //Comms
  uint16_t paired;
  uint8_t own_address[3];
  uint8_t dest_address[3];

};

confStruct usrConf;
confStruct defaultConf = {SW_VERSION, 1, 0, 0, 50, 0, 0, 1500, 2000, 1500, 2000, 1000, 10, 0, 1, 0, 0, 0, 0, 0, 25.0f, 10.0f, 10.0f, 5.0f, 35.0f, 45.0f, 45.0f, 0.0095554f, 0.0, 1000, 1, 0,{0, 0, 0}, {0, 0, 0}};

//Telemetry to send, MUST BE 8-bit!!
struct __attribute__((packed)) TelemetryPacket {
    uint8_t foil_bat = 0xFF;
    uint8_t foil_temp = 0xFF;
    uint8_t foil_speed = 0xFF;
    uint8_t error_code = 0;

    //This must be the last entry
    uint8_t link_quality = 0;
} telemetry;

// Forward packet sizing
#define RX_FORWARD_LEN 6

// Structure GPS for logging
struct gps_data {
    float speed;
    float latitude;
    float longitude;
    float heading;
    float hdop;
    uint32_t datetime;
    uint8_t satellites;
    uint8_t fix_quality;
};

// Global GPS structure (defined in GPS.ino)
struct gps_struct {
  float speed = 0;
  float latitude = 0;
  float longitude = 0;
  float heading = 0;
  float hdop = 99.9;
  uint32_t datetime = 0;
  uint8_t satellites = 0;
  uint8_t fix_quality = 0;
};

extern struct gps_struct gps;

// Global VESC structure
struct vesc_struct {
  int16_t fetTemp = 0;
  int32_t motCur = 0;
  int32_t batCur = 0;
  int16_t duty = 0;
  int32_t erpm = 0;
  int16_t batVolt = 0;
  uint8_t fault_code = 0;
  unsigned long last_packet = 0;
};

vesc_struct vesc;

// Packed log data structure
struct __attribute__((packed)) VescLogData {
    uint32_t timestamp;           // Local Timestamp in ms
    int16_t current_motor;       // Motor Current in 0.01A
    int16_t current_battery;     // Battery Current in 0.01A
    int8_t duty_cycle;           // Duty cycle in %
    uint16_t voltage;             // Voltage in 0.1V
    int16_t ERPM;                // ERPM / 10
    int8_t temp_mos;             // MOSFET temperature in °C
    uint8_t fault_code;           // Error code
    uint16_t speed;               // Speed in 0.1 km/h
    float latitude;               // Latitude in degrees
    float longitude;              // Longitude in degrees
    uint32_t datetime;            // UTC datetime as unix timestamp
};

// Runtime toggle for RX GPS debug prints (replaces compile-time DEBUG_GPS_TELEMETRY)
extern volatile bool rx_gps_debug_enabled;

/*
** FreeROTS/Task handles
*/
const int maxTasks = 10;
TaskStatus_t taskStats[maxTasks];

// Task handles
TaskHandle_t generatePWMHandle = NULL;
TaskHandle_t triggeredReceiveHandle = NULL;
TaskHandle_t checkConnStatusHandle = NULL;
extern TaskHandle_t loopTaskHandle;

// Semaphore for triggered task
SemaphoreHandle_t triggerReceiveSemaphore;
SemaphoreHandle_t i2cMutex;

/*
** Variables
*/
bool webUiEnabled = false;

volatile bool rfInterrupt = false;
volatile bool rxIsrState = 0;

volatile int unpairedBlink = 0;

volatile unsigned long last_successful_log = 0;

volatile unsigned long last_packet = 0;
volatile unsigned long radioBufferResetTimeout = 0;

volatile uint8_t telemetry_index = 0;
// Buffer for received data
volatile uint8_t payload_buffer[10];                // Maximum payload size is 10 bytes
volatile uint8_t payload_received = 0;              // Length of received payload

// Pairing timeout in milliseconds
const unsigned long PAIRING_TIMEOUT = 10000;
const uint8_t MAX_ADDRESS_CONFLICTS = 5;            // Maximum number of address conflicts before giving up


// Configuration and handles
rmt_channel_handle_t tx_channel = NULL;
rmt_encoder_handle_t copy_encoder = NULL;
// RMT items to encode the pulses
rmt_symbol_word_t pulse_symbol;

volatile int alternatePWMChannel = 0;
volatile bool PWM_active = 0;
volatile uint16_t PWM0_time = 0;
volatile uint16_t PWM1_time = 0;

volatile uint8_t thr_received = 0;
volatile uint8_t steering_received = 127;

volatile unsigned long get_vesc_timer = 0;

volatile uint8_t bind_pin_state = 0;

float fbatVolt = 0.0;
float noload_offset = 0.0;
uint8_t bc_arr[101];
uint8_t percent_last_val = 0xFF;
uint8_t percent_last_thr = 1;
unsigned long percent_last_thr_change = 0;

/* 
** Defines
*/

//SPI Pins
#define P_SPI_MISO 6
#define P_SPI_MOSI 7
#define P_SPI_SCK 10
//LORA Pins
#define P_LORA_DIO 3
#define P_LORA_BUSY 4
#define P_LORA_RST 5
#define P_LORA_NSS 8
//Misc Pins
#define P_PWM_OUT 9
#define P_U1_TX 18
#define P_U1_RX 19
#define P_UBAT_MEAS 0
#define P_I2C_SCL 1
#define P_I2C_SDA 2

//AW9523 Pins
#define AP_U1_MUX_0 8
#define AP_U1_MUX_1 9
#define AP_S_BIND 0
#define AP_S_AUX 10
#define AP_L_BIND 1
#define AP_L_AUX 11
#define AP_EN_BMS_MEAS 4
#define AP_BMS_MEAS 7
#define AP_EN_PWM0 13
#define AP_EN_PWM1 12
#define AP_EN_WET_MEAS 14
#define AP_WET_MEAS 15

// LED Blink interval for logging status (in milliseconds)
#define LED_BLINK_INTERVAL 500

//Debug options
//#define DEBUG_RX
//#define DEBUG_GPS_TELEMETRY
//#define DEBUG_GPS  // Enable detailed GPS initialization debugging
//#define DEBUG_VESC
//#define DEBUG_LOGGER  // Uncomment to enable logger debug messages
#define AUTO_START_LOGGING  // Uncomment for autostart at launch

#if defined DEBUG_RX
   #define rxprint(x)    Serial.print(x)
   #define rxprintln(x)  Serial.println(x)
#else
   #define rxprint(x)
   #define rxprintln(x)
#endif

#ifdef DEBUG_VESC
   #define VESC_DEBUG_PRINT(x) Serial.print(x)
   #define VESC_DEBUG_PRINTLN(x) Serial.println(x)
#else
   #define VESC_DEBUG_PRINT(x)
   #define VESC_DEBUG_PRINTLN(x)
#endif

#endif // BREMOTE_V2_RX_H