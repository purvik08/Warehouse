# 🚀 System Setup & Deployment Guide

This guide walks you through setting up and running the Warehouse Automation System from scratch.

---

## 📋 Prerequisites & Hardware

### 1. Hardware Requirements
* **Host Platform**: Raspberry Pi 5 (8GB recommended) running **Ubuntu 22.04 LTS (Jammy Jellyfish)**.
* **Microcontrollers**:
  * 1× ESP32-S3 DevKit (for the Server node)
  * 2× ESP32 DevKit V1 (for the Mobile Robot and Robotic Arm)
* **Peripherals**:
  * 2× MFRC522 RFID readers (1 for Server scanner, 1 for Mobile Robot floor scanner)
  * 1× HC-SR04 Ultrasonic Sensor
  * 1× L298N Motor Driver + 2× DC Motors (Mobile Robot chassis)
  * 5× Servo Motors (Robotic Arm assembly)
  * External battery packs/power supplies (e.g., Li-ion batteries + buck converters)

### 2. Software Requirements
* **PC**: Arduino IDE (v2.x recommended) installed.
* **Host**: A stable Local Area Network (Wi-Fi router) that all ESP32s and the RPi5 can connect to.

---

## 🔌 Step 1: Hardware Wiring
Before powering on, wire the components according to the [PINOUTS.md](file:///d:/arduino%20codes/Warehouse/Warehouse/PINOUTS.md) document. 

> [!WARNING]
> Do not power servo motors or DC motors directly from the ESP32 pins. Use an external 5V/6V battery pack/regulator, and remember to connect the external supply's Ground (GND) to the ESP32's Ground (GND).

---

## 💻 Step 2: Configure & Flash ESP32 Firmwares

### 1. Install Arduino Libraries
Open the Arduino IDE on your PC, navigate to **Library Manager**, and install the following:
* `micro_ros_arduino` (Download the Humble release zip from [micro-ROS GitHub](https://github.com/micro-ROS/micro_ros_arduino/releases) and install via *Sketch -> Include Library -> Add .ZIP Library...*)
* `WiFiManager` (by tzapu)
* `ESP32Servo` (by Kevin Harrington)
* `ElegantOTA` (by Ayush Sharma)
* `ArduinoJson` (by Benoit Blanchon)
* `MFRC522` (by GithubCommunity)

### 2. Configure Agent IPs
In each of the three `.ino` firmware files, make sure the micro-ROS agent IP points to your RPi5's IP address:

* **Server** ([server2.ino](file:///d:/arduino%20codes/Warehouse/Warehouse/server2/server2.ino)):
  Change `#define MICROROS_AGENT_IP` (around line 35) to your RPi5's static IP address (e.g., `192.168.1.100`).
* **Mobile Robot** ([MobileRobot.ino](file:///d:/arduino%20codes/Warehouse/Warehouse/MobileRobot/MobileRobot.ino)):
  Ensure the fallback IP in `resolveAgentIP()` (around line 227) is set to your RPi5's IP.
* **Robotic Arm** ([RoboticArm.ino](file:///d:/arduino%20codes/Warehouse/Warehouse/RoboticArm/RoboticArm.ino)):
  Ensure the fallback IP in `resolveAgentIP()` (around line 227) is set to your RPi5's IP.

### 3. Flash the Firmwares
1. Connect each ESP32 board to your PC via USB.
2. Select the correct Board and COM Port in Arduino IDE:
   * For the Server: Select **ESP32S3 Dev Module**.
   * For Robot & Arm: Select **ESP32 Dev Module**.
3. Click **Upload** to write the firmware.

---

## 🖥️ Step 3: Setup the Raspberry Pi 5 Host

1. Copy the [warehouse_ros2](file:///d:/arduino%20codes/Warehouse/Warehouse/warehouse_ros2) folder to your Raspberry Pi 5.
2. Open a terminal on the RPi5, navigate to the installer script, make it executable, and run it:
   ```bash
   cd warehouse_ros2/scripts/
   chmod +x install_ros2_humble.sh
   ./install_ros2_humble.sh
   ```
   *Note: This script will take 10-20 minutes depending on your internet connection. It installs ROS2 Humble, compiles micro-ROS agents, builds the custom nodes, and configures the system services.*

3. Reboot the RPi5 to apply environment variables and trigger the background micro-ROS agent service.

---

## 📡 Step 4: Network Pairing (First Boot)

On first boot, the ESP32s will start as local Wi-Fi hotspots because they do not have network credentials stored yet.

1. Turn on the **Server** ESP32.
2. On your phone or PC, connect to the Wi-Fi network named `WarehouseConfig_Server`.
3. A portal page will automatically open. Select your local Wi-Fi network, type your Wi-Fi password, and hit **Save**.
4. Repeat this process for:
   * **Mobile Robot**: Connects to `WarehouseConfig_Robot`
   * **Robotic Arm**: Connects to `WarehouseConfig_Arm01`
5. Once configured, all three microcontrollers will automatically connect to your router whenever they boot.

---

## 🚦 Step 5: Operating the System

### 1. Access the Dashboard Web UI
Find the IP address of your Server ESP32 (check your router client list or read the Arduino Serial Monitor output on boot). 
* Open a browser and navigate to: `http://<server-ip>/` (or `http://warehouse.local/` if mDNS is active).
* The dashboard displays live connected devices, inventory counts, and network logs.

### 2. Launch the Host nodes (RPi5)
Open a terminal on your Pi and source the ROS2 workspace, then launch the system nodes:
```bash
source /opt/ros/humble/setup.bash
source ~/warehouse_ws/install/setup.bash
ros2 launch warehouse_ros2 warehouse.launch.py
```
This launches:
* **Warehouse Manager Node**: Orchestrates the mission cycles (routing robot commands when box scans arrive).
* **Fleet Monitor Node**: Spawns a real-time console dashboard tracking robot battery, states, and errors.
* **Nav2 Bridge**: Listens to path planning goal poses and drives robot navigation.

### 3. Triggering a Test Run
1. Pass a provisioned RFID tag (e.g., starting with `BOXA_`) over the Server's RFID reader.
2. The Server publishes the event to ROS2.
3. The Manager node processes the box category, updates the web dashboard, and commands the Mobile Robot to start.
4. The Robotic arm prepares, picks, and coordinates hand-off tasks asynchronously.
