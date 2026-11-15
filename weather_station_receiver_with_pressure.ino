#include <Arduino.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>

// ==================================================
// E220 UART PINS
// ==================================================

#define LORA_RX_PIN 16
#define LORA_TX_PIN 17
#define LORA_BAUD 115200

HardwareSerial LoRaSerial(2);

#define MAX_PACKET_LENGTH 250

String receivedPacket = "";

// ==================================================
// WEATHER DATA
// ==================================================

struct WeatherData
{
  uint32_t sequence;

  float temperature;
  float humidity;
  float pressure;       // hPa
  float light;

  int bucketTips;

  float windSpeed;
  float windDirection;
};

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

  if (endPtr == field.c_str() || *endPtr != '\0' ||
      errno == ERANGE || !isfinite(value))
    return false;

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
  long parsedValue = strtol(field.c_str(), &endPtr, 10);

  if (endPtr == field.c_str() || *endPtr != '\0' ||
      errno == ERANGE || parsedValue < INT32_MIN ||
      parsedValue > INT32_MAX)
    return false;

  value = (int)parsedValue;
  return true;
}

// ==================================================
// PARSE SEQUENCE NUMBER
// ==================================================

bool parseSequenceNumber(const String &field, uint32_t &sequence)
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
  unsigned long parsedValue = strtoul(field.c_str(), &endPtr, 10);

  if (endPtr == field.c_str() || *endPtr != '\0' ||
      errno == ERANGE || parsedValue > UINT32_MAX)
    return false;

  sequence = (uint32_t)parsedValue;
  return true;
}

// ==================================================
// PARSE WEATHER PACKET
//
// WX,sequence,temperature,humidity,pressure,light,
// bucketTips,windSpeed,windDirection
// ==================================================

bool parseWeatherPacket(String packet, WeatherData &data)
{
  packet.trim();

  if (!packet.startsWith("WX,"))
    return false;

  String fields[9];
  int fieldCount = 0;
  int start = 0;

  while (start <= packet.length())
  {
    int comma = packet.indexOf(',', start);
    String field = (comma == -1)
      ? packet.substring(start)
      : packet.substring(start, comma);

    field.trim();

    if (fieldCount >= 9)
    {
      Serial.println("Too many packet fields");
      return false;
    }

    fields[fieldCount++] = field;

    if (comma == -1)
      break;

    start = comma + 1;
  }

  if (fieldCount != 9)
  {
    Serial.print("Incorrect field count: ");
    Serial.println(fieldCount);
    return false;
  }

  if (fields[0] != "WX")
    return false;

  if (!parseSequenceNumber(fields[1], data.sequence))
  {
    Serial.println("Invalid sequence");
    return false;
  }

  if (!parseFloatField(fields[2], data.temperature))
  {
    Serial.println("Invalid temperature");
    return false;
  }

  if (!parseFloatField(fields[3], data.humidity))
  {
    Serial.println("Invalid humidity");
    return false;
  }

  if (!parseFloatField(fields[4], data.pressure))
  {
    Serial.println("Invalid air pressure");
    return false;
  }

  if (!parseFloatField(fields[5], data.light))
  {
    Serial.println("Invalid light");
    return false;
  }

  if (!parseIntField(fields[6], data.bucketTips))
  {
    Serial.println("Invalid bucket tip count");
    return false;
  }

  if (!parseFloatField(fields[7], data.windSpeed))
  {
    Serial.println("Invalid wind speed");
    return false;
  }

  if (!parseFloatField(fields[8], data.windDirection))
  {
    Serial.println("Invalid wind direction");
    return false;
  }

  return true;
}

// ==================================================
// PRINT WEATHER DATA
// ==================================================

void printWeatherData(const WeatherData &data)
{
  Serial.println();
  Serial.println("========== WEATHER DATA ==========");

  Serial.print("Sequence: ");
  Serial.println((unsigned long)data.sequence);

  Serial.print("Temperature: ");
  Serial.print(data.temperature, 2);
  Serial.println(" °C");

  Serial.print("Humidity: ");
  Serial.print(data.humidity, 2);
  Serial.println(" %");

  Serial.print("Air pressure: ");
  if (data.pressure < 0)
    Serial.println("Unavailable");
  else
  {
    Serial.print(data.pressure, 2);
    Serial.println(" hPa");
  }

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

  Serial.println("=================================");
}

// ==================================================
// SEND ACKNOWLEDGEMENT
// ==================================================

void sendAcknowledgement(uint32_t sequence)
{
  String ack = "ACK,";
  ack += String((unsigned long)sequence);

  Serial.print("Sending acknowledgement: ");
  Serial.println(ack);

  LoRaSerial.println(ack);
  LoRaSerial.flush();

  Serial.println("Acknowledgement sent to E220 UART");
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

  if (packet.startsWith("WX,"))
  {
    WeatherData data;

    if (!parseWeatherPacket(packet, data))
    {
      Serial.println("Invalid weather packet; ACK not sent");
      return;
    }

    printWeatherData(data);
    sendAcknowledgement(data.sequence);
  }
  else
  {
    Serial.println("Unknown packet type; ignoring");
  }
}

// ==================================================
// SETUP
// ==================================================

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("==================================");
  Serial.println("    WEATHER STATION RECEIVER");
  Serial.println("==================================");

  LoRaSerial.begin(
    LORA_BAUD,
    SERIAL_8N1,
    LORA_RX_PIN,
    LORA_TX_PIN
  );

  Serial.println("LoRa hardware UART initialised");
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

    if (c == '\r')
      continue;

    if (c == '\n')
    {
      processPacket(receivedPacket);
      receivedPacket = "";
      continue;
    }

    if (receivedPacket.length() < MAX_PACKET_LENGTH)
    {
      receivedPacket += c;
    }
    else
    {
      Serial.println("Packet too long; discarding");
      receivedPacket = "";

      while (LoRaSerial.available() > 0)
      {
        char discard = (char)LoRaSerial.read();
        if (discard == '\n')
          break;
      }
    }
  }
}
