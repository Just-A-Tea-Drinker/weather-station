
#include <Arduino.h>
#include <Wire.h>
#include "RTClib.h"
#include <SPI.h>
#include <SD.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>

// ==================================================
// DS3231 RTC SETTINGS
// ==================================================

#define RTC_SDA_PIN 0
#define RTC_SCL_PIN 1

RTC_DS3231 rtc;

bool rtcReady = false;


// ==================================================
// E220 LORA UART SETTINGS
// ==================================================

#define LORA_RX_PIN 20
#define LORA_TX_PIN 21
#define LORA_BAUD 115200

HardwareSerial LoRaSerial(1);


// ==================================================
// SD CARD SETTINGS
// ==================================================

#define SD_CS_PIN   7
#define SD_SCK_PIN  4
#define SD_MISO_PIN 5
#define SD_MOSI_PIN 6

#define CSV_FILENAME "/weather.csv"

bool sdReady = false;


// ==================================================
// PACKET SETTINGS
// ==================================================

#define MAX_PACKET_LENGTH 250

String receivedPacket = "";

bool discardPacket = false;


// ==================================================
// WEATHER DATA
// ==================================================

struct WeatherData
{
  uint32_t sequence;

  float temperature;
  float humidity;
  float airPressure;
  float light;

  int bucketTips;

  float windSpeed;
  float windDirection;
};


// ==================================================
// DUPLICATE PROTECTION
// ==================================================

bool previousSequenceValid = false;

uint32_t previousSequence = 0;


// ==================================================
// PARSE FLOAT FIELD
// ==================================================

bool parseFloatField(const String &field, float &value)
{
  if (field.length() == 0)
    return false;

  char *endPtr = nullptr;

  errno = 0;

  value = strtof(field.c_str(), &endPtr);

  if (endPtr == field.c_str() ||
      *endPtr != '\0' ||
      errno == ERANGE ||
      !isfinite(value))
  {
    return false;
  }

  return true;
}


// ==================================================
// PARSE INTEGER FIELD
// ==================================================

bool parseIntField(const String &field, int &value)
{
  if (field.length() == 0)
    return false;

  char *endPtr = nullptr;

  errno = 0;

  long parsed = strtol(
    field.c_str(),
    &endPtr,
    10
  );

  if (endPtr == field.c_str() ||
      *endPtr != '\0' ||
      errno == ERANGE ||
      parsed < INT32_MIN ||
      parsed > INT32_MAX)
  {
    return false;
  }

  value = (int)parsed;

  return true;
}


// ==================================================
// PARSE SEQUENCE NUMBER
// ==================================================

bool parseSequenceNumber(
  const String &field,
  uint32_t &sequence
)
{
  if (field.length() == 0)
    return false;

  for (unsigned int i = 0; i < field.length(); i++)
  {
    if (field[i] < '0' || field[i] > '9')
      return false;
  }

  errno = 0;

  char *endPtr = nullptr;

  unsigned long parsed = strtoul(
    field.c_str(),
    &endPtr,
    10
  );

  if (endPtr == field.c_str() ||
      *endPtr != '\0' ||
      errno == ERANGE ||
      parsed > UINT32_MAX)
  {
    return false;
  }

  sequence = (uint32_t)parsed;

  return true;
}


// ==================================================
// PARSE WEATHER PACKET
//
// Expected format:
//
// WX,sequence,temperature,humidity,airPressure,
// light,bucketTips,windSpeed,windDirection
//
// ==================================================

bool parseWeatherPacket(
  String packet,
  WeatherData &data
)
{
  packet.trim();

  String fields[9];

  int fieldCount = 0;

  int start = 0;

  while (start <= packet.length())
  {
    int comma = packet.indexOf(',', start);

    String field;

    if (comma == -1)
      field = packet.substring(start);
    else
      field = packet.substring(start, comma);

    field.trim();

    if (fieldCount >= 9)
      return false;

    fields[fieldCount++] = field;

    if (comma == -1)
      break;

    start = comma + 1;
  }

  if (fieldCount != 9 || fields[0] != "WX")
    return false;

  // Sequence
  if (!parseSequenceNumber(fields[1], data.sequence))
    return false;

  // Temperature
  if (!parseFloatField(fields[2], data.temperature))
    return false;

  // Humidity
  if (!parseFloatField(fields[3], data.humidity))
    return false;

  // Air pressure
  if (!parseFloatField(fields[4], data.airPressure))
    return false;

  // Light
  if (!parseFloatField(fields[5], data.light))
    return false;

  // Rain bucket tips
  if (!parseIntField(fields[6], data.bucketTips))
    return false;

  // Wind speed
  if (!parseFloatField(fields[7], data.windSpeed))
    return false;

  // Wind direction
  if (!parseFloatField(fields[8], data.windDirection))
    return false;

  return true;
}


// ==================================================
// INITIALISE RTC
// ==================================================

void initialiseRTC()
{
  Serial.println();
  Serial.println("Initialising DS3231 RTC...");

  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

  if (!rtc.begin())
  {
    Serial.println("ERROR: DS3231 RTC not detected!");
    Serial.println("Check SDA/SCL wiring.");

    rtcReady = false;

    return;
  }

  Serial.println("DS3231 RTC detected!");

  if (rtc.lostPower())
  {
    Serial.println("RTC lost power!");
    Serial.println("Setting RTC to compilation date/time...");

    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

    Serial.println("RTC time configured.");
  }

  rtcReady = true;

  DateTime now = rtc.now();

  Serial.println("RTC ready.");

  Serial.print("Current RTC date: ");

  if (now.day() < 10)
    Serial.print("0");

  Serial.print(now.day());
  Serial.print("/");

  if (now.month() < 10)
    Serial.print("0");

  Serial.print(now.month());
  Serial.print("/");

  Serial.print(now.year());

  Serial.print("  ");

  if (now.hour() < 10)
    Serial.print("0");

  Serial.print(now.hour());
  Serial.print(":");

  if (now.minute() < 10)
    Serial.print("0");

  Serial.print(now.minute());
  Serial.print(":");

  if (now.second() < 10)
    Serial.print("0");

  Serial.println(now.second());
}


// ==================================================
// INITIALISE SD CARD
// ==================================================

void initialiseSD()
{
  Serial.println();
  Serial.println("Initialising SD card...");

  SPI.begin(
    SD_SCK_PIN,
    SD_MISO_PIN,
    SD_MOSI_PIN,
    SD_CS_PIN
  );

  if (!SD.begin(SD_CS_PIN, SPI))
  {
    Serial.println("ERROR: SD card initialisation failed!");

    sdReady = false;

    return;
  }

  sdReady = true;

  Serial.println("SD card initialised successfully!");

  uint64_t cardSize =
    SD.cardSize() / (1024ULL * 1024ULL);

  Serial.print("SD card capacity: ");
  Serial.print(cardSize);
  Serial.println(" MB");

  // Create CSV file if it does not exist

  if (!SD.exists(CSV_FILENAME))
  {
    File file = SD.open(
      CSV_FILENAME,
      FILE_WRITE
    );

    if (file)
    {
      file.println(
        "Date,Time,Temperature,Humidity,AirPressure,Light,"
        "BucketTips,WindSpeed,WindDirection"
      );

      file.close();

      Serial.println("CSV file created!");
    }
    else
    {
      Serial.println("ERROR creating CSV file!");

      sdReady = false;
    }
  }
  else
  {
    Serial.println("Existing weather.csv found.");
  }
}


// ==================================================
// SAVE WEATHER DATA TO CSV
// ==================================================

bool saveWeatherData(const WeatherData &data)
{
  if (!sdReady)
  {
    Serial.println("SD card unavailable!");

    return false;
  }

  if (!rtcReady)
  {
    Serial.println("RTC unavailable!");

    return false;
  }

  DateTime now = rtc.now();

  File file = SD.open(
    CSV_FILENAME,
    FILE_APPEND
  );

  if (!file)
  {
    Serial.println("ERROR opening CSV file!");

    return false;
  }

  char dateBuffer[11];
  char timeBuffer[9];

  snprintf(
    dateBuffer,
    sizeof(dateBuffer),
    "%04d-%02d-%02d",
    now.year(),
    now.month(),
    now.day()
  );

  snprintf(
    timeBuffer,
    sizeof(timeBuffer),
    "%02d:%02d:%02d",
    now.hour(),
    now.minute(),
    now.second()
  );

  size_t written = file.printf(
    "%s,%s,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%.1f\n",

    dateBuffer,
    timeBuffer,

    data.temperature,
    data.humidity,
    data.airPressure,
    data.light,
    data.bucketTips,
    data.windSpeed,
    data.windDirection
  );

  file.flush();

  file.close();

  if (written == 0)
  {
    Serial.println("ERROR writing CSV data!");

    return false;
  }

  Serial.println("Weather data saved to SD card.");

  return true;
}


// ==================================================
// PRINT WEATHER DATA
// ==================================================

void printWeatherData(const WeatherData &data)
{
  Serial.println();

  Serial.println("========== WEATHER DATA ==========");

  // RTC timestamp

  if (rtcReady)
  {
    DateTime now = rtc.now();

    Serial.print("Timestamp: ");

    if (now.day() < 10)
      Serial.print("0");

    Serial.print(now.day());
    Serial.print("/");

    if (now.month() < 10)
      Serial.print("0");

    Serial.print(now.month());
    Serial.print("/");

    Serial.print(now.year());

    Serial.print(" ");

    if (now.hour() < 10)
      Serial.print("0");

    Serial.print(now.hour());
    Serial.print(":");

    if (now.minute() < 10)
      Serial.print("0");

    Serial.print(now.minute());
    Serial.print(":");

    if (now.second() < 10)
      Serial.print("0");

    Serial.println(now.second());
  }

  Serial.print("Sequence: ");
  Serial.println((unsigned long)data.sequence);

  Serial.print("Temperature: ");
  Serial.print(data.temperature, 2);
  Serial.println(" C");

  Serial.print("Humidity: ");
  Serial.print(data.humidity, 2);
  Serial.println(" %");

  Serial.print("Air pressure: ");
  Serial.print(data.airPressure, 2);
  Serial.println(" hPa");

  Serial.print("Light: ");
  Serial.print(data.light, 2);
  Serial.println(" lux");

  Serial.print("Rain bucket tips: ");
  Serial.println(data.bucketTips);

  Serial.print("Wind speed: ");

  if (data.windSpeed < 0)
    Serial.println("Unavailable");
  else
  {
    Serial.print(data.windSpeed, 2);
    Serial.println(" m/s");
  }

  Serial.print("Wind direction: ");

  if (data.windDirection < 0)
    Serial.println("Unavailable");
  else
  {
    Serial.print(data.windDirection, 1);
    Serial.println(" degrees");
  }

  Serial.println("==================================");
}


// ==================================================
// SEND ACKNOWLEDGEMENT
// ==================================================

void sendAcknowledgement(uint32_t sequence)
{
  String ack = "ACK,";

  ack += String((unsigned long)sequence);

  Serial.print("Sending ACK: ");

  Serial.println(ack);

  LoRaSerial.println(ack);

  LoRaSerial.flush();

  Serial.println("Acknowledgement sent.");
}


// ==================================================
// PROCESS RECEIVED PACKET
// ==================================================

void processPacket(String packet)
{
  packet.trim();

  if (packet.length() == 0)
    return;

  Serial.println();

  Serial.print("Received packet: ");

  Serial.println(packet);

  if (!packet.startsWith("WX,"))
  {
    Serial.println("Unknown packet type.");

    return;
  }

  WeatherData data;

  if (!parseWeatherPacket(packet, data))
  {
    Serial.println("Invalid weather packet!");

    Serial.println("ACK not sent.");

    return;
  }

  // Duplicate detection

  if (previousSequenceValid &&
      data.sequence == previousSequence)
  {
    Serial.println("Duplicate packet detected.");

    // ACK previously saved packet
    sendAcknowledgement(data.sequence);

    return;
  }

  // Print measurements

  printWeatherData(data);

  // Save data before sending ACK

  if (saveWeatherData(data))
  {
    previousSequence = data.sequence;

    previousSequenceValid = true;

    sendAcknowledgement(data.sequence);

    Serial.println("Packet successfully processed.");
  }
  else
  {
    Serial.println("SD write failed.");

    Serial.println("ACK not sent.");
  }
}


// ==================================================
// SETUP
// ==================================================

void setup()
{
  Serial.begin(115200);

  delay(1500);

  Serial.println();

  Serial.println("==================================");
  Serial.println(" ESP32-C3 WEATHER RECEIVER");
  Serial.println(" SD CARD DATA LOGGER");
  Serial.println(" DS3231 RTC");
  Serial.println("==================================");

  // Initialise RTC

  initialiseRTC();

  // Initialise LoRa UART

  LoRaSerial.begin(
    LORA_BAUD,
    SERIAL_8N1,
    LORA_RX_PIN,
    LORA_TX_PIN
  );

  Serial.println("LoRa UART initialised.");

  // Initialise SD card

  initialiseSD();

  Serial.println();

  Serial.println("Receiver ready.");

  Serial.println("Waiting for weather packets...");
}


// ==================================================
// MAIN LOOP
// ==================================================

void loop()
{
  while (LoRaSerial.available() > 0)
  {
    char c = (char)LoRaSerial.read();

    // Ignore carriage return

    if (c == '\r')
      continue;

    // End of packet

    if (c == '\n')
    {
      if (!discardPacket)
      {
        processPacket(receivedPacket);
      }
      else
      {
        Serial.println("Oversized packet discarded.");
      }

      receivedPacket = "";

      discardPacket = false;

      continue;
    }

    // Discard oversized packets

    if (discardPacket)
      continue;

    if (receivedPacket.length() < MAX_PACKET_LENGTH)
    {
      receivedPacket += c;
    }
    else
    {
      receivedPacket = "";

      discardPacket = true;
    }
  }
}
