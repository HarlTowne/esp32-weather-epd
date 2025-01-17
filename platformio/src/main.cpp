/* Main program for esp32-weather-epd.
 * Copyright (C) 2022-2023  Luke Marzen
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <time.h>
#include <WiFi.h>
#include <Wire.h>

#include "api_response.h"
#include "client_utils.h"
#include "config.h"
#include "display_utils.h"
#include "renderer.h"

#include "icons/icons_196x196.h"

#include "SD.h"
#include "FS.h"
#include "SPI.h"
#include <SDConfig.h>

char configFile[] = "/settings.txt"; // filename

// too large to allocate locally on stack
static owm_resp_onecall_t       owm_onecall;
static owm_resp_air_pollution_t owm_air_pollution;

Preferences prefs;

/* Put esp32 into ultra low-power deep-sleep (<11μA).
 * Aligns wake time to the minute. Sleep times defined in config.cpp.
 */
void beginDeepSleep(unsigned long &startTime, tm *timeInfo)
{
  if (!getLocalTime(timeInfo))
  {
    Serial.print("Failed to obtain time before deep-sleep");
    Serial.println(", referencing older time.");
  }

  uint64_t sleepDuration = 0;
  int extraHoursUntilWake = 0;
  int curHour = timeInfo->tm_hour;

  if (timeInfo->tm_min >= 58)
  { // if we are within 2 minutes of the next hour, then round up for the
    // purposes of bed time
    curHour = (curHour + 1) % 24;
    extraHoursUntilWake += 1;
  }

  if (BED_TIME < WAKE_TIME && curHour >= BED_TIME && curHour < WAKE_TIME)
  { // 0              B   v  W  24
    // |--------------zzzzZzz---|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour < WAKE_TIME)
  { // 0 v W               B    24
    // |zZz----------------zzzzz|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour >= BED_TIME)
  { // 0   W               B  v 24
    // |zzz----------------zzzZz|
    extraHoursUntilWake += WAKE_TIME - (curHour - 24);
  }
  else // This feature is disabled (BED_TIME == WAKE_TIME)
  {    // OR it is not past BED_TIME
    extraHoursUntilWake = 0;
  }

  if (extraHoursUntilWake == 0)
  { // align wake time to nearest multiple of SLEEP_DURATION
    sleepDuration = SLEEP_DURATION * 60ULL 
                    - ((timeInfo->tm_min % SLEEP_DURATION) * 60ULL
                        + timeInfo->tm_sec);
  }
  else
  { // align wake time to the hour
    sleepDuration = extraHoursUntilWake * 3600ULL
                    - (timeInfo->tm_min * 60ULL + timeInfo->tm_sec);
  }

  // if we are within 2 minutes of the next alignment.
  if (sleepDuration <= 120ULL)
  {
    sleepDuration += SLEEP_DURATION * 60ULL;
  }
  
  // add extra delay to compensate for esp32's with fast RTCs.
  sleepDuration += 10ULL;

  esp_sleep_enable_timer_wakeup(sleepDuration * 1000000ULL);
  Serial.println("Awake for " 
                 + String((millis() - startTime) / 1000.0, 3) + "s");
  Serial.println("Deep-sleep for " + String(sleepDuration) + "s");
  esp_deep_sleep_start();
} // end beginDeepSleep

unsigned int errors;
void check_errors()
{
    if(errors < ERROR_SLEEP_DIV)
    {
      Serial.println("Error no. "+ String(errors));
      Serial.println("Deep-sleep for " 
                    + String(SLEEP_DURATION/ERROR_SLEEP_DIV) + "min");

      errors++;      
      prefs.putUInt("errors", errors);
      esp_sleep_enable_timer_wakeup(SLEEP_DURATION/ERROR_SLEEP_DIV 
                                    * 60ULL * 1000000ULL);
      esp_deep_sleep_start();
    }
}

/* Program entry point.
 */
void setup()
{
  unsigned long startTime = millis();
  Serial.begin(115200);

  // enable power to the screen
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  // ///////////////////////////////////////////////////////////////

  // // WIFI_SSID     = strdup(DEFAULT_WIFI_SSID);
  // // WIFI_PASSWORD = strdup(DEFAULT_WIFI_PASSWORD);
  if(!SD.begin(D3)){
    Serial.println("Card Mount Failed");
  }
  else
  {
    int maxLineLength = 127;
    SDConfig cfg;
    // Open the configuration file.
    if (!cfg.begin(configFile, maxLineLength)) 
    {
      Serial.print("Failed to open configuration file: ");
      Serial.println(configFile);
    }
    else
    {
      // Read each setting from the file.
      while (cfg.readNextSetting()) 
      {
        // Put a nameIs() block here for each setting you have.
        // doDelay
        if (cfg.nameIs("WIFI_SSID"))
        {
          WIFI_SSID = cfg.copyValue();
          Serial.print("WIFI_SSID set to ");
          Serial.println(WIFI_SSID);
        } 
        else if (cfg.nameIs("WIFI_PASSWORD")) 
        {
          WIFI_PASSWORD = cfg.copyValue();
          Serial.print("WIFI_PASSWORD set to ");
          Serial.println(WIFI_PASSWORD);
        } 
        else if (cfg.nameIs("OWM_APIKEY")) 
        {
          OWM_APIKEY = cfg.copyValue();
          Serial.print("OWM_APIKEY set to ");
          Serial.println(OWM_APIKEY);
        } 
        else if (cfg.nameIs("LAT")) 
        {
          LAT = cfg.copyValue();
          Serial.print("LAT set to ");
          Serial.println(LAT);
        } 
        else if (cfg.nameIs("LON")) 
        {
          LON = cfg.copyValue();
          Serial.print("LON set to ");
          Serial.println(LON);
        } 
        else if (cfg.nameIs("CITY_STRING")) 
        {
          CITY_STRING = cfg.copyValue();
          Serial.print("CITY_STRING set to ");
          Serial.println(CITY_STRING);
        } 
        else if (cfg.nameIs("TIMEZONE")) 
        {
          TIMEZONE = cfg.copyValue();
          Serial.print("TIMEZONE set to ");
          Serial.println(TIMEZONE);
        } 
        else if (cfg.nameIs("SLEEP_DURATION")) 
        {
          SLEEP_DURATION = cfg.getIntValue();
          Serial.print("SLEEP_DURATION set to ");
          Serial.println(SLEEP_DURATION);
        } 
        else if (cfg.nameIs("HOURLY_GRAPH_MAX")) 
        {
          HOURLY_GRAPH_MAX = cfg.getIntValue();
          Serial.print("HOURLY_GRAPH_MAX set to ");
          Serial.println(HOURLY_GRAPH_MAX);
        } 
        else if (cfg.nameIs("ERROR_SLEEP_DIV")) 
        {
          ERROR_SLEEP_DIV = cfg.getIntValue();
          Serial.print("ERROR_SLEEP_DIV set to ");
          Serial.println(ERROR_SLEEP_DIV);
        } 
        else 
        {
          // report unrecognized names.
          Serial.print("Unknown name in config: ");
          Serial.println(cfg.getName());
        }
      }
      // clean up
      cfg.end();   
    }
  }
  
  // ///////////////////////////////////////////////////////////////
  
  // GET BATTERY VOLTAGE
  // DFRobot FireBeetle Esp32-E V1.0 has voltage divider (1M+1M), so readings 
  // are multiplied by 2. Readings are divided by 1000 to convert mV to V.
  double batteryVoltage = 
            static_cast<double>(analogRead(PIN_BAT_ADC)) / 1000.0 * (3.5 / 2.0);
            // use / 1000.0 * (3.3 / 2.0) multiplier above for firebeetle esp32
            // use / 1000.0 * (3.5 / 2.0) for firebeetle esp32-E
  Serial.println("Battery voltage: " + String(batteryVoltage,2));

  // When the battery is low, the display should be updated to reflect that, but
  // only the first time we detect low voltage. The next time the display will
  // refresh is when voltage is no longer low. To keep track of that we will 
  // make use of non-volatile storage.
  // Open namespace for read/write to non-volatile storage
  prefs.begin("lowBat", false);
  bool lowBat = prefs.getBool("lowBat", false);
  errors = prefs.getUInt("errors", 0);

  // low battery, deep-sleep now
  if (batteryVoltage <= LOW_BATTERY_VOLTAGE)
  {
    if (lowBat == false)
    { // battery is now low for the first time
      prefs.putBool("lowBat", true);
      initDisplay();
      do
      {
        drawError(battery_alert_0deg_196x196, "Low Battery", "");
      } while (display.nextPage());
      display.powerOff();
    }

    if (batteryVoltage <= CRIT_LOW_BATTERY_VOLTAGE)
    { // critically low battery
      // don't set esp_sleep_enable_timer_wakeup();
      // We won't wake up again until someone manually presses the RST button.
      Serial.println("Critically low battery voltage!");
      Serial.println("Hibernating without wake time!");
    }
    else if (batteryVoltage <= VERY_LOW_BATTERY_VOLTAGE)
    { // very low battery
      esp_sleep_enable_timer_wakeup(VERY_LOW_BATTERY_SLEEP_INTERVAL 
                                    * 60ULL * 1000000ULL);
      Serial.println("Very low battery voltage!");
      Serial.println("Deep-sleep for " 
                     + String(VERY_LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    else
    { // low battery
      esp_sleep_enable_timer_wakeup(LOW_BATTERY_SLEEP_INTERVAL
                                    * 60ULL * 1000000ULL);
      Serial.println("Low battery voltage!");
      Serial.println("Deep-sleep for " 
                    + String(LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    esp_deep_sleep_start();
  }
  // battery is no longer low, reset variable in non-volatile storage
  if (lowBat == true)
  {
    prefs.putBool("lowBat", false);
  }

  String statusStr = {};
  String tmpStr = {};
  tm timeInfo = {};

  // START WIFI
  int wifiRSSI = 0; // “Received Signal Strength Indicator"
  wl_status_t wifiStatus = startWiFi(wifiRSSI);
  if (wifiStatus != WL_CONNECTED)
  { // WiFi Connection Failed
    killWiFi();
    check_errors();
    initDisplay();
    if (wifiStatus == WL_NO_SSID_AVAIL)
    {
      Serial.println("SSID Not Available");
      do
      {
        drawError(wifi_x_196x196, "SSID Not Available", "");
      } while (display.nextPage());
    }
    else
    {
      Serial.println("WiFi Connection Failed");
      do
      {
        drawError(wifi_x_196x196, "WiFi Connection", "Failed");
      } while (display.nextPage());
    }
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }

  // FETCH TIME
  bool timeConfigured = false;
  timeConfigured = setupTime(&timeInfo);
  if (!timeConfigured)
  { // Failed To Fetch The Time
    Serial.println("Failed To Fetch The Time");
    killWiFi();
    check_errors();
    initDisplay();
    do
    {
      drawError(wi_time_4_196x196, "Failed To Fetch", "The Time");
    } while (display.nextPage());
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }
  String refreshTimeStr;
  getRefreshTimeStr(refreshTimeStr, timeConfigured, &timeInfo);

  // MAKE API REQUESTS
  int rxOWM[2] = {};
  WiFiClient client;
  rxOWM[0] = getOWMonecall(client, owm_onecall);
  if (rxOWM[0] != HTTP_CODE_OK)
  {
    statusStr = "One Call " + OWM_ONECALL_VERSION + " API";
    tmpStr = String(rxOWM[0], DEC) + ": " + getHttpResponsePhrase(rxOWM[0]);
    killWiFi();
    check_errors();
    initDisplay();
    do
    {
      drawError(wi_cloud_down_196x196, statusStr, tmpStr);
    } while (display.nextPage());
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }
  rxOWM[1] = getOWMairpollution(client, owm_air_pollution);
  killWiFi(); // WiFi no longer needed
  if (rxOWM[1] != HTTP_CODE_OK)
  {
    statusStr = "Air Pollution API";
    tmpStr = String(rxOWM[1], DEC) + ": " + getHttpResponsePhrase(rxOWM[1]);
    check_errors();
    initDisplay();
    do
    {
      drawError(wi_cloud_down_196x196, statusStr, tmpStr);
    } while (display.nextPage());
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }

  // GET INDOOR TEMPERATURE AND HUMIDITY, start BME280...
  float inTemp     = NAN;
  float inHumidity = NAN;
  Serial.print("Reading from BME280... ");
  TwoWire I2C_bme = TwoWire(0);
  Adafruit_BME280 bme;

  I2C_bme.begin(PIN_BME_SDA, PIN_BME_SCL, 100000); // 100kHz
  if(bme.begin(BME_ADDRESS, &I2C_bme))
  {
    inTemp     = bme.readTemperature(); // Celsius
    inHumidity = bme.readHumidity();    // %

    // check if BME readings are valid
    // note: readings are checked again before drawing to screen. If a reading
    //       is not a number (NAN) then an error occurred, a dash '-' will be
    //       displayed.
    if (isnan(inTemp) || isnan(inHumidity)) {
      statusStr = "BME read failed";
      Serial.println(statusStr);
    }
    else
    {
      Serial.println("Success");
    }
  }
  else
  {
    statusStr = "BME not found"; // check wiring
    Serial.println(statusStr);
  }

  String dateStr;
  getDateStr(dateStr, &timeInfo);

  // RENDER FULL REFRESH
  initDisplay();
  do
  {
    drawCurrentConditions(owm_onecall.current, owm_onecall.daily[0],
                          owm_air_pollution, inTemp, inHumidity);
    drawForecast(owm_onecall.daily, timeInfo);
    drawLocationDate(CITY_STRING, dateStr);
    drawOutlookGraph(owm_onecall.hourly, timeInfo);
#ifndef DISABLE_ALERTS
    drawAlerts(owm_onecall.alerts, CITY_STRING, dateStr);
#endif
    drawStatusBar(statusStr, refreshTimeStr, wifiRSSI, batteryVoltage);
  } while (display.nextPage());
  display.powerOff();

  // Clear error count
  Serial.println("Cleared Errors");
  errors = 0;      
  prefs.putUInt("errors", errors);

  // disable screen power
  delay(1000);
  digitalWrite(21, LOW);
  // DEEP-SLEEP
  beginDeepSleep(startTime, &timeInfo);
} // end setup

/* This will never run
 */
void loop()
{
} // end loop

