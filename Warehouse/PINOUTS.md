# 🔌 Hardware Pinout Guide — Warehouse Automation System

This guide outlines the wiring and pinouts for all three ESP32 microcontroller units (MCUs) used in the system.

---

## 1. 🎛️ ESP32-S3 Server / RFID Scanner Node
* **Hardware**: ESP32-S3 DevKit
* **Role**: Runs the web dashboard, manages LittleFS storage database, and scans incoming boxes via local RFID.

| Component | Component Pin | ESP32-S3 GPIO Pin | Connection Type / Notes |
| :--- | :--- | :--- | :--- |
| **MFRC522 RFID** | VCC (3.3V) | 3.3V | Power (Do not connect to 5V!) |
| | GND | GND | Ground |
| | RST (Reset) | **GPIO 0** | Control |
| | SDA (SS) | **GPIO 5** | SPI Slave Select |
| | MOSI | **GPIO 23** | SPI Master Out Slave In |
| | MISO | **GPIO 19** | SPI Master In Slave Out |
| | SCK | **GPIO 18** | SPI Clock |

---

## 2. 🤖 Mobile Robot (AGV) Node
* **Hardware**: ESP32 DevKit V1 (Generic 30-pin or 38-pin board)
* **Role**: Navigates the floor grid, checks battery levels, reports obstacles, and follows command/cmd_vel instructions.

### A. Power and Logic Connections
| Component | Component Pin | ESP32 GPIO Pin | Connection Type / Notes |
| :--- | :--- | :--- | :--- |
| **L298N H-Bridge** | IN1 (Motor A) | **GPIO 26** | PWM/Direction Control |
| | IN2 (Motor A) | **GPIO 25** | PWM/Direction Control |
| | IN3 (Motor B) | **GPIO 33** | PWM/Direction Control |
| | IN4 (Motor B) | **GPIO 32** | PWM/Direction Control |
| | GND | GND | Common ground with ESP32 |
| **HC-SR04 Sonar** | VCC | 5V | Sensor power |
| | GND | GND | Ground |
| | TRIG | **GPIO 14** | Trigger Output |
| | ECHO | **GPIO 27** | Echo Input (Use 5V to 3.3V voltage divider!) |
| **Battery Divider** | Analog Out | **GPIO 34** | Analog Input (Read battery voltage) |

### B. MFRC522 Floor RFID Reader
Used to read floor tags for real-time localization.
| MFRC522 Pin | ESP32 GPIO Pin | Connection Type / Notes |
| :--- | :--- | :--- |
| VCC | 3.3V | Power |
| GND | GND | Ground |
| RST | **GPIO 0** | Control |
| SDA (SS) | **GPIO 5** | SPI Slave Select |
| MOSI | **GPIO 23** | SPI MOSI |
| MISO | **GPIO 19** | SPI MISO |
| SCK | **GPIO 18** | SPI SCK |

---

## 3. 🦾 Robotic Arm Node
* **Hardware**: ESP32 DevKit V1
* **Role**: Controls 5 analog servo motors for picking and placing items based on ROS2 commands.

| Component / Servo | Control Pin | ESP32 GPIO Pin | Connection Type / Notes |
| :--- | :--- | :--- | :--- |
| **Base Servo** | PWM Signal | **GPIO 15** | Servo 1 Signal |
| **Shoulder Servo** | PWM Signal | **GPIO 2** | Servo 2 Signal |
| **Elbow Servo** | PWM Signal | **GPIO 4** | Servo 3 Signal |
| **Wrist Servo** | PWM Signal | **GPIO 16** | Servo 4 Signal |
| **Gripper Servo** | PWM Signal | **GPIO 17** | Servo 5 Signal |
| **Battery Monitor** | Analog Out | **GPIO 34** | Analog Input (Read battery voltage) |
| **Common Power** | Servo VCC (5V-6V) | *External Supply* | **DO NOT** power servos directly from ESP32 5V pin! |
| | Servo GND | GND | Connect external power GND to ESP32 GND. |

---

## ⚠️ Important Wiring Notes

1. **Servo Power Supply**: Servo motors draw peak currents of up to 2A when under load. Always use a dedicated external 5V/6V DC power supply (e.g., UBEC or buck converter) to power the servos. Connect the GND of the external supply to the ESP32 GND to maintain a common reference.
2. **HC-SR04 Echo Pin**: The HC-SR04 Echo pin outputs 5V. The ESP32 pins are **not 5V tolerant**. It is highly recommended to use a simple voltage divider (1kΩ and 2kΩ resistors) on the Echo line to reduce the voltage to 3.3V before connecting to GPIO 27.
3. **RFID SPI Shareability**: On boards that require both an RFID reader and other SPI peripherals, SPI lines (SCK, MISO, MOSI) can be shared, but each device must have a unique Slave Select (SS/SDA) pin.
