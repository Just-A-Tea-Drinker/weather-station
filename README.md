# ESP32-C3 Low-Power Solar Weather Station

A small, solar-powered IoT weather station based on the **ESP32-C3 Super Mini**, designed for long-term autonomous operation with low power consumption.

The station measures:

- Wind speed
- Wind direction
- Light intensity
- Atmospheric pressure
- Temperature
- Relative humidity
- Rainfall

Weather data is transmitted wirelessly using an **Ebyte E220-9D LoRa module** to a separate receiver, which timestamps the measurements using an RTC and stores the data in CSV format on an SD card.

## System Overview

The system consists of two main units:

### Weather Station

The outdoor weather station is powered from a **12 V solar power system** consisting of a 30 W solar panel and a 15 Ah battery.

The station wakes approximately every **10 minutes**, performs a complete sensor measurement cycle, transmits the data over LoRa, waits for an acknowledgement, and then powers itself down until the next cycle.

### Receiver

The receiver is based on another **ESP32-C3 Super Mini** and is responsible for:

1. Receiving LoRa packets from the weather station.
2. Parsing the received sensor data.
3. Adding a date and time from the DS3231 RTC.
4. Formatting the data as CSV.
5. Storing the measurements on an SD card.
6. Sending an acknowledgement packet back to the weather station.

## Hardware

### Weather Station

| Component | Function |
|---|---|
| ESP32-C3 Super Mini | Main microcontroller |
| Adafruit TPL5110 | Ultra-low-power timer / power management |
| RS485-to-TTL converter | Interface to the wind sensor |
| DFRobot Gravity Sensor V1 | Rainfall measurement |
| Ebyte E220-9D | LoRa wireless communication |
| BH1750 | Light intensity measurement |
| BMP280 | Atmospheric pressure and temperature |
| AHT10 | Temperature and humidity measurement |
| Songle 5 V relay | Power switching |
| S8050 NPN transistor | Sensor power control |
| 57.5 kΩ resistors | Power/control circuitry |

### Receiver

| Component | Function |
|---|---|
| ESP32-C3 Super Mini | Main receiver/controller |
| Ebyte E220-9D | LoRa wireless communication |
| SD card module | Data storage |
| DS3231 RTC | Timestamp generation |

## Power System

The weather station is currently powered by:

- **30 W solar panel**
- **12 V battery**
- **15 Ah battery capacity**

The current power system is intentionally oversized for the prototype and provides a large energy margin for periods of reduced sunlight.

However, there is significant potential for further optimisation.

The ESP32-C3 can be placed into deep sleep between measurement cycles, while the rainfall sensor can potentially be monitored directly using ESP32 GPIO interrupts. Sensor power can also be switched using an NPN transistor so that sensors are only powered when measurements are required.

These changes would substantially reduce the station's average power consumption and could allow the system to operate with a much smaller solar panel and battery.

## Measurement and Transmission Cycle

The weather station follows the sequence below:

1. **TPL5110 triggers the station** at approximately 10-minute intervals.
2. The ESP32-C3 starts up and initialises the required hardware.
3. The rain sensor remains continuously powered at approximately **2 mA**.
4. The station waits approximately **5 seconds** for the ultrasonic wind sensor to start and complete its calibration.
5. All weather measurements are collected.
6. The measurements are packaged into a LoRa data packet.
7. The packet is transmitted by the E220-9D module.
8. The receiver validates and processes the packet.
9. The receiver generates a unique acknowledgement (ACK) response.
10. The weather station waits for and validates the ACK.
11. Once the ACK has been received, the rain sensor is power-cycled using the transistor/relay circuitry.
12. The ESP32 signals the TPL5110 to terminate the power cycle.
13. The system shuts down until the next scheduled measurement.

### Measurement Parameters

The transmitted dataset contains the following environmental measurements:


Wind Speed
Wind Direction
Light Intensity
Atmospheric Pressure
Temperature
Humidity
Rainfall


The receiver appends a timestamp from the DS3231 RTC before writing the measurement to the SD card.

## LoRa Communication

The weather station and receiver communicate using the **Ebyte E220-9D LoRa module**.

A basic acknowledgement mechanism is implemented to ensure that transmitted measurements are successfully received before the weather station shuts down.

The communication sequence is approximately:

```text
Weather Station
       |
       |  Sensor data
       |--------------------->|
       |                       | Receiver
       |                       | Parse data
       |                       | Add timestamp
       |                       | Store CSV
       |                       |
       |       ACK             |
       |<---------------------|
       |
   Validate ACK
       |
   Power down
```

This prevents the station from immediately shutting down after transmission and provides confirmation that the receiver has processed the packet.

## Data Storage

The receiver converts the incoming measurements into CSV format and stores them on an SD card together with the timestamp supplied by the DS3231 RTC.

A typical record contains:


Date,Time,Temperature,Humidity,AirPressure,Light,BucketTips,WindSpeed,WindDirection


This makes the collected data straightforward to import into tools such as Excel, Python, MATLAB, or other data-analysis software.

## Power Consumption

During a measurement and transmission cycle, the weather station currently draws approximately **170 mA for around 500 ms** during the main active portion of the cycle.

The TPL5110 is then used to remove power from the system until the next scheduled measurement.

The current implementation prioritises simplicity and reliable operation rather than absolute minimum power consumption. The 30 W solar panel and 15 Ah battery therefore provide considerably more capacity than is strictly required by the present design.

## Future Power Optimisation

There are several areas where the design could be further improved.

### ESP32-C3 Deep Sleep

Instead of relying primarily on the TPL5110 to remove power from the entire system, the ESP32-C3 could remain powered and spend most of its time in deep sleep.

The ESP32 could wake periodically, collect measurements, transmit the data and return to deep sleep.

### Direct Rainfall Interrupt Detection

The rainfall sensor currently remains powered continuously.

A future revision could monitor the rain gauge's reed-switch pulses directly using an ESP32 GPIO configured as an interrupt.

This would allow the sensor to consume substantially less power while still accurately counting rainfall events.

### Switched Sensor Power

Sensors that do not need to remain continuously powered could be supplied through an NPN transistor or other load-switching circuit.

The ESP32 would enable the sensor supply only when required:


Sleep
  |
  v
Wake ESP32
  |
  v
Power sensors
  |
  v
Wait for sensor startup
  |
  v
Take measurements
  |
  v
Transmit via LoRa
  |
  v
Receive ACK
  |
  v
Disable sensor power
  |
  v
Return to sleep

This approach would reduce the average current consumption and improve the overall energy efficiency of the station.

## Design Goals

The primary goals of the project are:

- Autonomous long-term operation
- Solar-powered operation
- Low average power consumption
- Reliable wireless data transmission
- Local data storage
- Accurate timestamping
- Modular hardware
- Simple and robust operation
- Potential for further power optimisation

## Current Status

This project is currently built around a working prototype architecture using the ESP32-C3, LoRa communication, multiple environmental sensors, and a solar-powered 12 V supply.

The current design deliberately uses a larger-than-necessary battery and solar panel to provide a generous power margin. Future revisions will focus on reducing the station's average power consumption through deeper ESP32 sleep states, interrupt-driven rainfall measurement, and selective sensor power switching.

## Project Architecture

                    WEATHER STATION
                 ┌─────────────────────┐
                 │     ESP32-C3        │
                 │                     │
                 │  Wind   Light       │
                 │  Rain   Pressure    │
                 │  Temp   Humidity    │
                 └─────────┬───────────┘
                           │
                           │ UART
                           v
                    E220-9D LoRa
                           │
                           │ ~ ~ ~ ~ ~ ~ ~
                           │   LoRa Link
                           │ ~ ~ ~ ~ ~ ~ ~
                           v
                     E220-9D LoRa
                           │
                           v
                 ┌─────────────────────┐
                 │     ESP32-C3        │
                 │                     │
                 │   Parse Data        │
                 │   Add RTC Timestamp │
                 │   Generate ACK      │
                 └─────────┬───────────┘
                           │
                           v
                       SD Card
                           │
                           v
                         CSV


## Future Improvements

Potential future development includes:

- ESP32-C3 deep-sleep optimisation
- Interrupt-driven rainfall measurement
- Improved sensor power switching
- Reduced solar-panel and battery requirements
- More efficient LoRa packet handling
- Additional weather parameters
- Remote data visualisation
- Battery voltage and system health monitoring
- Improved enclosure and environmental protection
- Long-term power-consumption analysis

---

