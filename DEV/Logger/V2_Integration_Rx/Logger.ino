#include <FS.h>
#include <SPIFFS.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// User-configurable scaling factors
#define MIN_FREE_SPACE_KB 500  // Minimum free space required in KB

// Task handle and configuration
static TaskHandle_t loggerTaskHandle = NULL;
static SemaphoreHandle_t fileMutex = NULL;
static bool logging_active = false;
static uint32_t log_interval_ms = 1000;  // Default 1 Hz
static File currentLogFile;
static String currentLogFileName = "";
static uint32_t last_space_check = 0;
static const uint32_t SPACE_CHECK_INTERVAL = 60000;  // Check every 60 seconds

// Forward declaration of task
void loggerTask(void* parameter);

// Scale and convert data
VescLogData convertToLogData() {
  VescLogData data;

  data.timestamp = millis();
  
  //-300...300A
  data.current_motor = (int16_t)constrain(vesc.motCur, -30000, 30000);
  data.current_battery = (int16_t)constrain(vesc.batCur, -30000, 30000);

  //-100 bis 100%
  int32_t scaled_duty = vesc.duty / 10;
  data.duty_cycle = (int8_t)constrain(scaled_duty, -101, 101);

  
  data.voltage = (uint16_t)(abs(vesc.batVolt));

  //-300kERPM... 300kERPM
  data.ERPM = (int16_t)constrain(vesc.erpm / 10, -30000, 30000);

  //-120°C...120°C
  data.temp_mos = (int8_t)constrain(vesc.fetTemp / 10, -120, 120);
  data.fault_code = vesc.fault_code;

  data.speed = (uint16_t)(gps.speed * 10);  // 0.1 km/h
  data.latitude = gps.latitude;
  data.longitude = gps.longitude;
  data.datetime = gps.datetime;

  return data;
}

// Check and manage SPIFFS space
bool ensureFreeSpace() {
  size_t totalBytes = SPIFFS.totalBytes();
  size_t freeBytes = totalBytes - SPIFFS.usedBytes();

  if(usrConf.debug_byte & 1<<2)
  {
    Serial.printf("Logger running, %u KB free\n", freeBytes / 1024);
  }

  if (freeBytes > (MIN_FREE_SPACE_KB * 1024)) {
    return true;
  }

  Serial.printf("Space low: %u KB free (need %d)\n", freeBytes / 1024, MIN_FREE_SPACE_KB);

  // Close current log file safely
  if (xSemaphoreTake(fileMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (currentLogFile) {
      currentLogFile.close();
      currentLogFileName = "";
    }
    xSemaphoreGive(fileMutex);
  }

  // PASS 1: Collect candidates
  String deleteCandidates[20];
  int candidateCount = 0;

  File root = SPIFFS.open("/");
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file && candidateCount < 20) {
      String filename = String(file.name());
      String fullPath = "/" + filename;

      if (fullPath.endsWith(".log")) {
        String cleanName = filename;
        if (cleanName.startsWith("/")) {
          cleanName = cleanName.substring(1);
        }
        int dotPos = cleanName.indexOf('.');
        if (dotPos > 0) {
          String timestampStr = cleanName.substring(0, dotPos);
          uint32_t timestamp = timestampStr.toInt();
          if (timestamp > 1000000000) {
            deleteCandidates[candidateCount++] = fullPath;
          }
        }
      }
      file.close();
      file = root.openNextFile();
    }
    root.close();
  }

  if (candidateCount == 0) {
    Serial.println("No log files to delete");
    return false;
  }

  Serial.printf("Deleting %d oldest files...\n", candidateCount);

  // PASS 2: Delete oldest first
  int deleted = 0;
  for (int i = 0; i < candidateCount && (SPIFFS.totalBytes() - SPIFFS.usedBytes()) < (MIN_FREE_SPACE_KB * 1024); i++) {
    String targetFile = deleteCandidates[i];

    for (int retry = 0; retry < 5; retry++) {
      vTaskDelay(pdMS_TO_TICKS(100 * (retry + 1)));
      if (SPIFFS.remove(targetFile)) {
        deleted++;
        break;
      }
    }
  }

  freeBytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
  bool success = freeBytes >= (MIN_FREE_SPACE_KB * 1024);
  Serial.printf("Delete done: %d/%d files, %u KB free - %s\n",
                deleted, candidateCount, freeBytes / 1024, success ? "OK" : "FAIL");

  return success;
}

// Create new log file
bool createNewLogFile() {
  // Wait for GPS fix and valid time
  Serial.println("Waiting for GPS fix and valid time...");
  uint32_t startWait = millis();
  while (gps.fix_quality == 0 || gps.datetime < 1000000) {
    vTaskDelay(pdMS_TO_TICKS(500));

    /*if (millis() - startWait > 3600000) {  // 60 minute timeout
      Serial.println("GPS fix timeout!");
      return false;
    }*/

    if (!logging_active) {  // Check if logging was stopped
      return false;
    }
  }

  Serial.println("GPS fix acquired!");

  // Ensure enough space
  if (!ensureFreeSpace()) {
    Serial.println("Failed to ensure free space");
    return false;
  }

  // Create filename from GPS unix timestamp
  currentLogFileName = "/" + String(gps.datetime) + ".log";

  if (xSemaphoreTake(fileMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    currentLogFile = SPIFFS.open(currentLogFileName, FILE_WRITE);
    xSemaphoreGive(fileMutex);

    if (!currentLogFile) {
      Serial.printf("Failed to create log file: %s\n", currentLogFileName.c_str());
      return false;
    }

    Serial.printf("Created log file: %s\n", currentLogFileName.c_str());
    return true;
  }

  return false;
}

// Logger task (low priority)
void loggerTask(void* parameter) {
  Serial.println("Logger task started");
  TickType_t xLastWakeTime;

  while (true) 
  {
    if (logging_active) 
    {
      // Create new log file if needed
      if (!currentLogFile || currentLogFileName.length() == 0) 
      {
        if (!createNewLogFile()) 
        {
          Serial.println("Failed to create log file, retrying...");
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
      }

      if (millis() - last_space_check > SPACE_CHECK_INTERVAL) 
      {
        last_space_check = millis();
        if (!ensureFreeSpace()) 
        {
          Serial.println("Space check failed, cannot continue logging");
          continue;
        }
      }

      // Convert and log data
      VescLogData logData = convertToLogData();

      if (xSemaphoreTake(fileMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (currentLogFile) {
          size_t written = currentLogFile.write((uint8_t*)&logData, sizeof(VescLogData));
          currentLogFile.flush();

          if (written != sizeof(VescLogData)) {
            Serial.printf("Write error: wrote %u of %u bytes\n", written, sizeof(VescLogData));
          }
          else
          {
            last_successful_log = millis();
          }
        }
        xSemaphoreGive(fileMutex);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(log_interval_ms));
  }
}


// Initialize logger
void initLogger() {
  Serial.println("Initializing data logger...");

  // Mount SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  Serial.printf("SPIFFS Total: %u KB, Used: %u KB\n",
                SPIFFS.totalBytes() / 1024,
                SPIFFS.usedBytes() / 1024);

  // Create mutex for file access
  fileMutex = xSemaphoreCreateMutex();
  if (fileMutex == NULL) {
    Serial.println("Failed to create file mutex");
    return;
  }

  // Create low-priority task (priority 1, idle is 0)
  BaseType_t result = xTaskCreatePinnedToCore(
    loggerTask,         // Task function
    "DataLogger",       // Task name
    4096,               // Stack size
    NULL,               // Parameters
    1,                  // Priority (low)
    &loggerTaskHandle,  // Task handle
    0                   // Core 0
  );

  if (result != pdPASS) {
    Serial.println("Failed to create logger task");
    return;
  }

  Serial.println("Data logger initialized successfully");
}

// Start logging
void startLog() {
  if (logging_active) {
    Serial.println("Logging already active");
    return;
  }

  Serial.println("Starting data logging...");
  logging_active = true;
  last_space_check = millis();
}

// Stop logging
void stopLog() {
  if (!logging_active) {
    Serial.println("Logging not active");
    return;
  }

  Serial.println("Stopping data logging...");
  logging_active = false;

  // Close current log file
  if (xSemaphoreTake(fileMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    if (currentLogFile) {
      currentLogFile.close();
      Serial.printf("Closed log file: %s\n", currentLogFileName.c_str());
    }
    currentLogFileName = "";
    xSemaphoreGive(fileMutex);
  }
}

// Set logging rate
void setLogRate(float log_rate_Hz) {
  if (log_rate_Hz <= 0 || log_rate_Hz > 1000) {
    Serial.println("Invalid log rate (must be 0-1000 Hz)");
    return;
  }

  log_interval_ms = (uint32_t)(1000.0 / log_rate_Hz);
  Serial.printf("Log rate set to %.2f Hz (interval: %u ms)\n", log_rate_Hz, log_interval_ms);
}

// List all log files
void listLogFiles() {
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println("Failed to open SPIFFS root");
    return;
  }

  Serial.println("\n=== Available Log Files ===");
  Serial.println("Filename\t\tSize (KB)\tDate");
  Serial.println("--------------------------------------------");

  File file = root.openNextFile();
  int fileCount = 0;

  while (file) {
    String filename = String(file.name());
    if (filename.endsWith(".log")) {
      size_t fileSize = file.size();

      // Extract timestamp from FULL PATH correctly
      int dotPos = filename.lastIndexOf('.');        // Last dot position
      int slashPos = filename.lastIndexOf('/') + 1;  // After last slash
      String timestampStr = filename.substring(slashPos, dotPos);
      uint32_t timestamp = timestampStr.toInt();

      // Convert Unix timestamp to DDMMYYYY - HHMMSS
      time_t rawtime = (time_t)timestamp;
      struct tm* timeinfo = gmtime(&rawtime);
      char dateStr[20];

      if (timeinfo != NULL) {
        // DD-MM-YYYY HH:MM:SS
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

      Serial.printf("%s\t%.2f\t%s\n",
                    filename.c_str(),
                    fileSize / 1024.0,
                    dateStr);
      fileCount++;
    }
    file = root.openNextFile();
  }

  Serial.printf("\nTotal log files: %d\n", fileCount);
  Serial.printf("Free space: %u KB\n\n", (SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024);
}


// Download log file as CSV via Serial
void downloadLogFile(const char* filename) {
  // Normalize path: allow both "1768513141.log" and "/1768513141.log"
  String fullPath = String(filename);
  fullPath.trim();  // Remove whitespace

  // Remove any leading slash first
  while (fullPath.startsWith("/")) {
    fullPath.remove(0, 1);
  }
  // Now add exactly one leading slash
  fullPath = "/" + fullPath;

  Serial.printf("Downloading file: '%s'\n", fullPath.c_str());

  if (!SPIFFS.exists(fullPath)) {
    Serial.printf("File not found: %s\n", fullPath.c_str());
    return;
  }

  File file = SPIFFS.open(fullPath, FILE_READ);
  if (!file) {
    Serial.printf("Failed to open file: %s\n", fullPath.c_str());
    return;
  }

  Serial.println("\n=== BEGIN CSV DATA ===");
  Serial.println("timestamp_ms,motor_current_A,battery_current_A,duty_cycle_%,voltage_V,ERPM,temp_mos_C,fault_code,speed_kmh,latitude,longitude,datetime_unix");

  VescLogData logData;
  uint32_t recordCount = 0;

  while (file.available()) {
    size_t bytesRead = file.read((uint8_t*)&logData, sizeof(VescLogData));

    if (bytesRead == sizeof(VescLogData)) {
      Serial.printf("%u,%.2f,%.2f,%d,%.1f,%d,%u,%u,%.1f,%.6f,%.6f,%u\n",
                    logData.timestamp,
                    logData.current_motor / 100.0f,
                    logData.current_battery / 100.0f,
                    (int16_t)logData.duty_cycle,  // %d for signed int8_t
                    logData.voltage / 10.0f,
                    (int32_t)logData.ERPM * 10,  // %d for signed int16_t
                    logData.temp_mos,
                    logData.fault_code,
                    logData.speed / 10.0f,  // 0.1 km/h scaling
                    logData.latitude,
                    logData.longitude,
                    logData.datetime);
      recordCount++;
    } else {
      Serial.printf("\nWarning: Incomplete record at position %u\n", file.position());
      break;
    }
  }

  file.close();
  Serial.println("=== END CSV DATA ===");
  Serial.printf("Total records: %u\n\n", recordCount);
}

void deleteLogFile(const char* filename) {
  // Same path normalization as download
  String fullPath = String(filename);
  fullPath.trim();

  // Remove any leading slash first
  while (fullPath.startsWith("/")) {
    fullPath.remove(0, 1);
  }
  fullPath = "/" + fullPath;

  Serial.printf("Delete request: '%s'\n", fullPath.c_str());

  if (!SPIFFS.exists(fullPath)) {
    Serial.printf("File not found: %s\n", fullPath.c_str());
    return;
  }

  size_t fileSize = SPIFFS.open(fullPath, FILE_READ).size();

  if (SPIFFS.remove(fullPath)) {
    Serial.printf("SUCCESS: Deleted %s (%.2f KB)\n",
                  fullPath.c_str(), fileSize / 1024.0);
    Serial.printf("New free space: %u KB\n",
                  (SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024);
  } else {
    Serial.printf("FAILED to delete: %s\n", fullPath.c_str());
  }
}