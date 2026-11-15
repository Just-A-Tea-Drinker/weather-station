er #include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <BH1750.h>
#include <DFRobot_RainfallSensor.h>
#include <math.h>
#include <esp_system.h>

// TARGET BOARD: ESP32-S3 DevKit with native USB-OTG connector.
// Arduino IDE settings:
//   Board: your matching ESP32-S3 Dev Module / DevKit option
//   USB CDC On Boot: Enabled (so Serial uses native USB for debugging)
//   USB Mode: Hardware CDC and JTAG, if this option is shown
// Install the DFRobot_RainfallSensor library through Arduino Library Manager.
// Do not use GPIO 19 or GPIO 20 for sensors; they are the native USB D-/D+ pins.

// ==================================================
// I2C PINS
// ==================================================
#define SDA_PIN 7
#define SCL_PIN 6

// ==================================================
// E220 UART PINS
// ==================================================
#define LORA_RX_PIN 16
#define LORA_TX_PIN 17
#define LORA_BAUD 115200

// Relay control for the DFRobot rainfall sensor power supply.
// HIGH energises the relay and briefly cuts power to the sensor.
#define RAIN_POWER_RELAY_PIN 5
#define RAIN_POWER_CYCLE_MS 1000

// TPL5110 DONE input
#define TPL_DONE_PIN 8

// Rainfall sensor uses the shared I2C bus on SDA_PIN / SCL_PIN.
// DFRobot SEN0575 I2C address is 0x1D; select I2C mode on its switch.

// ==================================================
// ULTRASONIC WIND SENSOR UART (RS485-TTL AUTO-DIRECTION MODULE)
// ==================================================
#define WIND_RX_PIN 11
#define WIND_TX_PIN 12
#define WIND_BAUD 4800
#define WIND_MODBUS_ADDRESS 0x01
#define WIND_START_REGISTER 0x0000  // Working sensor example reads registers 0 and 1
#define WIND_REGISTER_COUNT 0x0002  // Speed and direction, two 16-bit registers
#define WIND_SPEED_SCALE 100.0f     // Speed raw value / 100 = m/s
#define WIND_DIRECTION_SCALE 10.0f  // Direction raw value / 10 = degrees
#define MPS_TO_MPH 2.236936f

// On ESP32-S3, native USB CDC handles Serial debugging. The wind sensor
// uses UART1 and the E220 LoRa module uses UART2. Rainfall uses I2C.
#define WindSerial Serial1

// Communication settings
#define MAX_TRANSMISSION_ATTEMPTS 3
#define ACK_TIMEOUT_MS 100

// ==================================================
// SENSOR OBJECTS
// ==================================================
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
BH1750 lightMeter;

// DFRobot Gravity tipping-bucket rainfall sensor (I2C mode).
// Requires the DFRobot_RainfallSensor library by DFRobot.
DFRobot_RainfallSensor_I2C rainSensor(&Wire);
bool rainAvailable = false;

bool ahtAvailable = false;
bool bmpAvailable = false;
bool lightAvailable = false;

// ==================================================
// CALIBRATION OFFSETS
// ==================================================
float ahtTempOffset = -1.7;
float ahtHumidityOffset = -0.75;
float bmpTempOffset = -2.5;
float bmpPressureOffset = 0.0;
float luxScale = 1.0;

// ==================================================
// WEATHER DATA STRUCTURE
// ==================================================
struct WeatherData {
  float temperature;
  float humidity;
  float pressure;
  float light;
  float wind;          // mph
  float windDirection; // degrees
  float rainfall;      // accumulated rainfall, mm
  float rainfall1h;    // rainfall in last hour, mm
  float rainRaw;       // tipping count
  float rainWorkingHours;
};

bool acknowledged = false;
uint32_t packetSequence = 0;
uint32_t bootToAckMs = 0;

// ==================================================
// MODBUS CRC16
// ==================================================
uint16_t calculateModbusCRC(const byte *data, byte length) {
  uint16_t crc = 0xFFFF;

  for (byte i = 0; i < length; i++) {
    crc ^= data[i];

    for (byte j = 0; j < 8; j++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }

  return crc;
}

// ==================================================
// READ ULTRASONIC WIND SPEED AND DIRECTION OVER MODBUS
// ==================================================
// Based on the user's working test sketch: address 1, 4800 baud,
// function 03, start register 0, read two registers (speed + direction).
// Speed raw / 100 = m/s; direction raw / 10 = degrees.
bool readWindSensor(float &speedMph, float &directionDegrees) {
  byte request[8] = {
    WIND_MODBUS_ADDRESS,
    0x03,
    (byte)(WIND_START_REGISTER >> 8),
    (byte)(WIND_START_REGISTER & 0xFF),
    0x00,
    WIND_REGISTER_COUNT,
    0x00,
    0x00
  };

  uint16_t crc = calculateModbusCRC(request, 6);
  request[6] = crc & 0xFF;
  request[7] = crc >> 8;

  while (WindSerial.available() > 0) WindSerial.read();

  WindSerial.write(request, sizeof(request));
  WindSerial.flush();

  byte response[9];
  byte count = 0;
  unsigned long startTime = millis();

  while ((uint32_t)(millis() - startTime) < 1000 && count < sizeof(response)) {
    while (WindSerial.available() > 0 && count < sizeof(response)) {
      response[count++] = (byte)WindSerial.read();
    }
    delay(1);
  }

  if (count != sizeof(response)) {
    Serial.print("Wind sensor: expected 9 response bytes, received ");
    Serial.println(count);
    if (count > 0) {
      Serial.print("Wind raw response: ");
      for (byte i = 0; i < count; i++) {
        if (response[i] < 0x10) Serial.print('0');
        Serial.print(response[i], HEX);
        Serial.print(' ');
      }
      Serial.println();
    }
    return false;
  }

  if (response[0] != WIND_MODBUS_ADDRESS || response[1] != 0x03 || response[2] != 0x04) {
    Serial.println("Wind sensor: invalid Modbus response header");
    return false;
  }

  uint16_t receivedCRC = (uint16_t)response[7] | ((uint16_t)response[8] << 8);
  uint16_t calculatedCRC = calculateModbusCRC(response, 7);
  if (receivedCRC != calculatedCRC) {
    Serial.println("Wind sensor: Modbus CRC error");
    return false;
  }

  uint16_t rawSpeed = ((uint16_t)response[3] << 8) | response[4];
  uint16_t rawDirection = ((uint16_t)response[5] << 8) | response[6];

  float speedMps = rawSpeed / WIND_SPEED_SCALE;
  speedMph = speedMps * MPS_TO_MPH;
  directionDegrees = rawDirection / WIND_DIRECTION_SCALE;

  if (!isfinite(speedMph) || speedMph < 0.0f ||
      !isfinite(directionDegrees) || directionDegrees < 0.0f || directionDegrees > 360.0f) {
    Serial.println("Wind sensor: invalid decoded measurement");
    return false;
  }

  return true;
}

// ==================================================
// READ DFRobot GRAVITY RAINFALL SENSOR OVER I2C
// ==================================================
void readRainSensorData(WeatherData &data) {
  if (!rainAvailable) {
    Serial.println("Rain sensor unavailable; rain values remain -9999");
    return;
  }

  data.rainfall = rainSensor.getRainfall();
  data.rainfall1h = rainSensor.getRainfall(1);
  data.rainRaw = rainSensor.getRawData();
  data.rainWorkingHours = rainSensor.getSensorWorkingTime();

  if (!isfinite(data.rainfall) || data.rainfall < 0.0f) data.rainfall = -9999.0f;
  if (!isfinite(data.rainfall1h) || data.rainfall1h < 0.0f) data.rainfall1h = -9999.0f;
  if (!isfinite(data.rainRaw) || data.rainRaw < 0.0f) data.rainRaw = -9999.0f;
  if (!isfinite(data.rainWorkingHours) || data.rainWorkingHours < 0.0f) data.rainWorkingHours = -9999.0f;
}

// ==================================================
// INITIALISE SENSORS
// ==================================================
void initialiseSensors() {

  Wire.begin(SDA_PIN, SCL_PIN);

  // Standard I2C speed for reliable communication.
  Wire.setClock(100000);

  // --------------------------------------------------
  // AHT20 TEMPERATURE AND HUMIDITY SENSOR
  // --------------------------------------------------
  ahtAvailable = aht.begin();

  if (!ahtAvailable) {
    Serial.println("WARNING: AHT20 unavailable");
  } else {
    Serial.println("AHT20 initialised successfully");
  }

  // --------------------------------------------------
  // BMP280 PRESSURE SENSOR
  // --------------------------------------------------
  bmpAvailable = bmp.begin();

  if (bmpAvailable) {

    bmp.setSampling(
        Adafruit_BMP280::MODE_FORCED,
        Adafruit_BMP280::SAMPLING_X1,
        Adafruit_BMP280::SAMPLING_X1,
        Adafruit_BMP280::FILTER_OFF
    );

    Serial.println("BMP280 initialised successfully");

  } else {

    Serial.println("WARNING: BMP280 unavailable");
  }

  // --------------------------------------------------
  // BH1750 LIGHT SENSOR
  // --------------------------------------------------
  // Common BH1750 addresses:
  // ADDR LOW  = 0x23
  // ADDR HIGH = 0x5C

  uint8_t lightAddress = 0;

  const uint8_t possibleLightAddresses[] = {
    0x23,
    0x5C
  };

  for (uint8_t address : possibleLightAddresses) {

    Wire.beginTransmission(address);

    if (Wire.endTransmission() == 0) {

      lightAddress = address;

      break;
    }
  }

  if (lightAddress != 0) {

    Serial.printf(
        "BH1750 detected at I2C address 0x%02X\n",
        lightAddress
    );

    lightAvailable = lightMeter.begin(
        BH1750::ONE_TIME_HIGH_RES_MODE,
        lightAddress,
        &Wire
    );

  } else {

    lightAvailable = false;

    Serial.println(
        "ERROR: BH1750 not found at 0x23 or 0x5C."
    );

    Serial.println(
        "Check SDA, SCL, power, GND and ADDR connections."
    );
  }

  if (!lightAvailable) {

    Serial.println(
        "WARNING: BH1750 initialisation failed"
    );

  } else {

    Serial.println(
        "BH1750 initialised successfully"
    );
  }

  // Initialise the DFRobot Gravity rainfall sensor on the shared I2C bus.
  // Wire.begin(SDA_PIN, SCL_PIN) was called above. The library calls
  // Wire.begin() internally; on ESP32 this should leave an already-started
  // Wire instance using the configured SDA/SCL pins.
  Serial.println("Initialising DFRobot Gravity rainfall sensor over I2C...");
  rainAvailable = rainSensor.begin();
  if (rainAvailable) {
    Serial.println("Rainfall sensor initialised successfully over I2C");
    Serial.print("Rain sensor firmware: ");
    Serial.println(rainSensor.getFirmwareVersion());
    Serial.print("Rain sensor PID: 0x");
    Serial.println(rainSensor.pid, HEX);
    Serial.print("Rain sensor VID: 0x");
    Serial.println(rainSensor.vid, HEX);
  } else {
    Serial.println("WARNING: Rainfall sensor initialisation failed; check SDA/SCL wiring, power, and set the sensor switch to I2C mode");
  }
}

// ==================================================
// READ WEATHER DATA
// ==================================================
WeatherData readWeatherData() {

  WeatherData data;

  data.temperature = -9999.0f;
  data.humidity = -9999.0f;
  data.pressure = -9999.0f;
  data.light = -9999.0f;
  data.wind = -9999.0f;
  data.windDirection = -9999.0f;
  data.rainfall = -9999.0f;
  data.rainfall1h = -9999.0f;
  data.rainRaw = -9999.0f;
  data.rainWorkingHours = -9999.0f;

  // --------------------------------------------------
  // AHT20
  // --------------------------------------------------
  if (ahtAvailable) {

    sensors_event_t humidity;
    sensors_event_t temperature;

    aht.getEvent(&humidity, &temperature);

    float t =
        temperature.temperature + ahtTempOffset;

    float h =
        humidity.relative_humidity + ahtHumidityOffset;

    if (isfinite(t) && isfinite(h)) {

      data.temperature = t;
      data.humidity = h;
    }
  }

  // --------------------------------------------------
  // BMP280
  // --------------------------------------------------
  if (bmpAvailable && bmp.takeForcedMeasurement()) {

    float t =
        bmp.readTemperature() + bmpTempOffset;

    float p =
        bmp.readPressure() / 100.0f
        + bmpPressureOffset;

    if (isfinite(t) && isfinite(p) && p > 0.0f) {

      data.pressure = p;

      // Use BMP280 temperature if AHT20 is unavailable.
      if (data.temperature == -9999.0f) {

        data.temperature = t;
      }
    }
  }

  // --------------------------------------------------
  // BH1750 LIGHT SENSOR
  // --------------------------------------------------
  if (lightAvailable) {

    Serial.println("Taking BH1750 light measurement...");

    // Start a fresh one-time high-resolution measurement.
    bool measurementStarted =
        lightMeter.configure(
            BH1750::ONE_TIME_HIGH_RES_MODE
        );

    if (measurementStarted) {

      // Allow time for the measurement to complete.
      delay(180);

      float rawLux = lightMeter.readLightLevel();

      float lux = rawLux * luxScale;

      if (isfinite(lux) && lux >= 0.0f) {

        data.light = lux;

        Serial.print("BH1750 reading: ");
        Serial.print(lux, 2);
        Serial.println(" lux");

      } else {

        Serial.printf(
            "WARNING: Invalid BH1750 reading: %.2f\n",
            rawLux
        );
      }

    } else {

      Serial.println(
          "WARNING: Could not trigger BH1750 measurement"
      );
    }

  } else {

    Serial.println(
        "BH1750 unavailable; light reading set to -9999"
    );
  }

  // --------------------------------------------------
  // ULTRASONIC WIND SENSOR (RS485 / MODBUS)
  // --------------------------------------------------
  float windMph = -9999.0f;
  float windDirection = -9999.0f;
  if (readWindSensor(windMph, windDirection)) {
    data.wind = windMph;
    data.windDirection = windDirection;
  }

  // Read rain totals via the DFRobot library.
  readRainSensorData(data);

  return data;
}

// ==================================================
// PRINT WEATHER DATA
// ==================================================
void printWeatherData(const WeatherData &data) {

  Serial.println();
  Serial.println("-------- WEATHER DATA --------");

  Serial.print("Temperature: ");
  Serial.println(data.temperature);

  Serial.print("Humidity: ");
  Serial.println(data.humidity);

  Serial.print("Pressure: ");
  Serial.println(data.pressure);

  Serial.print("Light: ");
  Serial.print(data.light);
  Serial.println(" lux");

  Serial.print("Wind speed: ");
  Serial.print(data.wind);
  Serial.println(" mph");

  Serial.print("Wind direction: ");
  Serial.print(data.windDirection);
  Serial.println(" degrees");

  Serial.print("Accumulated rainfall: ");
  Serial.print(data.rainfall);
  Serial.println(" mm");

  Serial.print("Rainfall in last hour: ");
  Serial.print(data.rainfall1h);
  Serial.println(" mm");

  Serial.print("Rain gauge tip count: ");
  Serial.println(data.rainRaw);

  Serial.print("Rain sensor working time: ");
  Serial.print(data.rainWorkingHours);
  Serial.println(" hours");

  Serial.println("------------------------------");
}

// ==================================================
// BUILD WEATHER PACKET
// ==================================================
String buildWeatherPacket(
    uint32_t sequence,
    const WeatherData &data
) {

  String packet = "WX,";

  packet += String((unsigned long)sequence);
  packet += ",";

  packet += String(data.temperature, 2);
  packet += ",";

  packet += String(data.humidity, 2);
  packet += ",";

  packet += String(data.pressure, 2);
  packet += ",";

  packet += String(data.light, 2);
  packet += ",";

  packet += String(data.wind, 2);
  packet += ",";
  packet += String(data.windDirection, 1);
  packet += ",";
  packet += String(data.rainfall, 2);
  packet += ",";
  packet += String(data.rainfall1h, 2);
  packet += ",";
  packet += String(data.rainRaw, 0);
  packet += ",";
  packet += String(data.rainWorkingHours, 2);

  return packet;
}

// ==================================================
// CLEAR STALE UART DATA
// ==================================================
void clearLoRaInput() {

  while (Serial2.available() > 0) {

    Serial2.read();
  }
}

// ==================================================
// WAIT FOR ACKNOWLEDGEMENT
// ==================================================
bool waitForAcknowledgement(
    uint32_t expectedSequence,
    uint32_t timeoutMs
) {

  uint32_t start = millis();

  String response;

  while ((uint32_t)(millis() - start) < timeoutMs) {

    while (Serial2.available() > 0) {

      char c = (char)Serial2.read();

      if (c == '\r') {
        continue;
      }

      if (c == '\n') {

        response.trim();

        String expected =
            "ACK," + String((unsigned long)expectedSequence);

        if (response == expected) {

          Serial.print("Received acknowledgement: ");
          Serial.println(response);

          return true;
        }

        if (response.length() > 0) {

          Serial.print("Ignoring unexpected response: ");
          Serial.println(response);
        }

        response = "";

      } else if (response.length() < 64) {

        response += c;

      } else {

        response = "";
      }
    }

    delay(1);
  }

  return false;
}

// ==================================================
// TRANSMIT WEATHER DATA WITH RETRIES
// ==================================================
bool transmitWeatherData(
    const String &packet,
    uint32_t sequence,
    uint32_t &elapsedToAckMs
) {

  for (
      int attempt = 1;
      attempt <= MAX_TRANSMISSION_ATTEMPTS;
      attempt++
  ) {

    Serial.printf(
        "Transmission attempt %d/%d\n",
        attempt,
        MAX_TRANSMISSION_ATTEMPTS
    );

    // Discard stale responses before sending.
    clearLoRaInput();

    uint32_t txStartMs = millis();

    // Send the complete weather packet.
    Serial2.println(packet);
    Serial2.flush();

    Serial.println(
        "Weather packet sent to E220 UART; waiting for ACK"
    );

    // Wait for a matching acknowledgement.
    if (waitForAcknowledgement(
            sequence,
            ACK_TIMEOUT_MS
        )) {

      elapsedToAckMs = millis();

      Serial.printf(
          "Boot-to-ACK time: %lu ms\n",
          (unsigned long)elapsedToAckMs
      );

      return true;
    }

    Serial.printf(
        "No ACK on attempt %d (attempt duration %lu ms)\n",
        attempt,
        (unsigned long)(millis() - txStartMs)
    );
  }

  Serial.println(
      "ERROR: All transmission attempts failed"
  );

  return false;
}

// ==================================================
// SIGNAL TPL5110 DONE AND STOP RUNNING
// ==================================================
void signalTpl5110DoneAndStop() {

  // Briefly energise the relay to cut power to the rainfall sensor.
  // When the TPL5110 removes power from the ESP32, GPIO 5 will stop
  // driving the relay, allowing the rainfall sensor to power back up
  // and start a fresh measurement window for the next cycle.
  Serial.println("Power-cycling rainfall sensor via relay on GPIO 5...");
  digitalWrite(RAIN_POWER_RELAY_PIN, HIGH);
  delay(RAIN_POWER_CYCLE_MS);

  Serial.println("Rainfall sensor power-cycle pulse complete; signalling TPL5110 DONE on GPIO 8");
  Serial.flush();

  // TPL5110 DONE is active HIGH. The ESP32 will lose power, releasing
  // GPIO 5 and de-energising the relay.
  digitalWrite(TPL_DONE_PIN, HIGH);

  // Remain here until the TPL5110 removes power.
  while (true) {

    delay(1000);
  }
}

// ==================================================
// SETUP
// ==================================================
void setup() {

  // Start native USB CDC debugging. In Arduino IDE, enable USB CDC On Boot.
  // Do not connect the rain sensor to the USB D-/D+ pins (GPIO 19/20).
  Serial.begin(115200);

  delay(100);
  
  // Configure the rainfall sensor power relay (inactive at startup).
  pinMode(RAIN_POWER_RELAY_PIN, OUTPUT);
  digitalWrite(RAIN_POWER_RELAY_PIN, LOW);
  pinMode(TPL_DONE_PIN, OUTPUT);
  digitalWrite(TPL_DONE_PIN, LOW);

  Serial.println("Diagnostic mode: GPIO 8 held LOW.");
  // Configure TPL5110 DONE.
  pinMode(TPL_DONE_PIN, OUTPUT);
  digitalWrite(TPL_DONE_PIN, LOW);

  // Start wind sensor RS485-TTL UART1 on GPIO 11 (RX) and GPIO 12 (TX).
  WindSerial.begin(
      WIND_BAUD,
      SERIAL_8N1,
      WIND_RX_PIN,
      WIND_TX_PIN
  );

  // Start E220 LoRa UART2 on GPIO 16 (RX) and GPIO 17 (TX).
  Serial2.begin(
      LORA_BAUD,
      SERIAL_8N1,
      LORA_RX_PIN,
      LORA_TX_PIN
  );

  // Generate a sequence number.
  // TPL5110 removes power, so RAM state is lost each cycle.
  packetSequence = esp_random();

  Serial.println();
  Serial.println("Weather station powered on");

  Serial.printf(
      "Packet sequence: %lu\n",
      (unsigned long)packetSequence
  );

  // Initialise weather sensors.
  initialiseSensors();

  // Read the weather data.
  WeatherData data = readWeatherData();

  // Print the readings.
  printWeatherData(data);

  // Build the weather packet.
  String packet = buildWeatherPacket(
      packetSequence,
      data
  );

  Serial.print("Transmitting weather packet: ");
  Serial.println(packet);

  // Transmit and wait for acknowledgement.
  acknowledged = transmitWeatherData(
      packet,
      packetSequence,
      bootToAckMs
  );

  if (acknowledged) {

    Serial.printf(
        "ACK received %lu ms after boot\n",
        (unsigned long)bootToAckMs
    );

  } else {

    Serial.println(
        "No acknowledgement received"
    );
  }

  // Finish the powered cycle.
  // TPL5110 should remove power after DONE goes HIGH.
  //signalTpl5110DoneAndStop();
}

// ==================================================
// MAIN LOOP
// ==================================================
void loop() {

  delay(1000);
}
