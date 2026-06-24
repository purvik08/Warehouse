# 📑 Developer Architecture & Code Reference Guide

This document explains the internal architecture, directory structure, code logic, communication protocols, and Web UI frontend/backend design of the Warehouse Automation System to help you modify it according to your needs.

---

## 📁 1. Directory Structure Map

```
d:\arduino codes\Warehouse
├── .gitignore                      # Prevents compiler build files from being tracked
└── Warehouse/                      # Source directory containing code
    ├── PINOUTS.md                  # Detailed hardware wiring diagrams
    ├── SETUP_GUIDE.md              # Installation and deployment walkthrough
    ├── README.md                   # Project overview and specifications
    │
    ├── MobileRobot/                # ESP32 AGV Chassis firmware
    │   └── MobileRobot.ino         # Line follower, RFID localizer, micro-ROS publishers
    │
    ├── RoboticArm/                 # ESP32 5-Axis Pick & Place Robotic Arm firmware
    │   └── RoboticArm.ino          # Non-blocking servo sequencer, micro-ROS receiver
    │
    ├── server2/                    # ESP32-S3 Server & Web Interface firmware
    │   └── server2.ino             # Web Server, WebSockets Server, LittleFS database
    │
    ├── rfid_read/                  # Utility sketch to read RFID tag sectors
    │   └── rfid_read.ino
    │
    ├── rfid_write/                 # Utility sketch to format & provision new RFID tags
    │   └── rfid_write.ino
    │
    └── warehouse_ros2/             # ROS2 Humble Python workspace for Raspberry Pi 5
        ├── package.xml             # ROS2 package dependencies manifest
        ├── setup.py                # Build instructions and entry points for scripts
        ├── config/
        │   └── warehouse_params.yaml  # Parameter configuration (speeds, timers, paths)
        ├── launch/
        │   └── warehouse.launch.py # Main launch script (starts micro-ROS Agent & nodes)
        ├── scripts/
        │   └── install_ros2_humble.sh # One-shot script to build ROS2/micro-ROS agent
        └── warehouse_ros2/         # Core ROS2 Python nodes
            ├── __init__.py
            ├── warehouse_manager_node.py # Central mission orchestration state machine
            ├── fleet_monitor_node.py     # Terminal dashboard UI & CSV transaction logger
            └── nav2_bridge_node.py       # Intermediary action client for Nav2 planning
```

---

## 📡 2. Architecture & Communication Flow

The system employs a **hybrid communication architecture** leveraging both **ROS2 (DDS/micro-ROS)** for hardware control and **WebSockets/HTTP** for user telemetry.

```
                  ┌───────────────────────────────┐
                  │   Raspberry Pi 5 (ROS2)       │
                  │   - warehouse_manager_node    │
                  │   - fleet_monitor_node        │
                  │   - nav2_bridge_node          │
                  └───────────────┬───────────────┘
                                  │
                       micro-ROS  │ (UDP / DDS Topics)
            ┌─────────────────────┼─────────────────────┐
            │                     │                     │
┌───────────▼───────────┐ ┌───────▼───────────┐ ┌───────▼───────────┐
│     ESP32-S3 Server   │ │   ESP32 Robot     │ │    ESP32 Arm      │
│  - Web server (80)    │ │   - Line Follower │ │   - 5 Servos      │
│  - WebSockets (81)    │ │   - Obstacle Det  │ │   - Watchdog      │
└───────────┬───────────┘ └───────────────────┘ └───────────────────┘
            │
  WebSocket │ (Live JSON telemetry)
┌───────────▼───────────┐
│   Web UI Dashboard    │
│   (HTML5/CSS3/JS)     │
└───────────────────────┘
```

### The Cargo Handling Loop (Step-by-Step)
*Note: The Robotic Arm is stationary at a fixed base near the conveyor belt. The Mobile Robot (AGV) is a separate line-following cart that receives the box at the pickup point and carries it to the storage rack.*

1. **RFID Box Scan**: A box is scanned at the conveyor by the reader connected to the **ESP32-S3 Server**.
2. **ROS2 Broadcast**: The Server increments its inventory (Sector A, B, or C) based on the tag prefix (`BOXA_`, `BOXB_`, `BOXC_`), writes to LittleFS flash, broadcasts via WebSockets, and publishes the tag to `/warehouse/server/rfid`.
3. **Mission Start**: The RPi5's `warehouse_manager_node` transitions to `BOX_DETECTED` and commands the stationary Robotic Arm to `pick` via `/warehouse/arm/command`.
4. **Arm Loading**: The Robotic Arm picks the box from the conveyor and places it onto the waiting Mobile Robot (AGV) at the pickup station.
5. **AGV Dispatch**: The AGV senses the box placed on top of it (e.g. via an onboard sensor or when commanded by the manager node on `/warehouse/agv/command`).
6. **AGV Navigation**: The AGV drives along the line track carrying the box. When it scans the target floor RFID tag (e.g. `LOC-RACK-A`), it publishes the landmark to `/warehouse/agv/location`, and the manager stops the AGV.
7. **Cargo Unloading**: The box is transferred to the destination rack, and the manager resets the state machine to `IDLE` for the next run.

---

## ⚙️ 3. Node & Code Functionality

### 🎛️ A. Server / Database Hub ([server2.ino](file:///d:/arduino%20codes/Warehouse/Warehouse/server2/server2.ino))
* **Database (LittleFS)**: Writes configurations (`config.json`), inventories (`inventory.json`), and raw transactions (`transactions.csv`) into the ESP32-S3 internal SPIFFS/LittleFS flash partition.
* **WebSocket Server (Port 81)**: Manages bi-directional client connections. On events, it broadcasts JSON string updates:
  * `{"type": "inventory", "rackA": 5, "rackB": 2}`
  * `{"type": "robot_status", "robot": "LF-01", "battery": 92, "status": "MOVING"}`
* **Web Server (Port 80)**: Serves static HTML routes (`/`, `/inventory`, `/robot`, `/settings`).
* **micro-ROS Publisher**: Standard micro-ROS initialization hooks are established upon Wi-Fi completion to publish scanner updates immediately.

### 🤖 B. Mobile Robot AGV ([MobileRobot.ino](file:///d:/arduino%20codes/Warehouse/Warehouse/MobileRobot/MobileRobot.ino))
* **Line Following Engine**: Monitors analog/digital line sensors, calculating proportional correction to adjust differential drive speeds on `MOTOR_A` and `MOTOR_B` pins.
* **Non-Spamming Sonar**: Pings HC-SR04 sonar at 20Hz. To avoid flooding network traffic, obstacle notifications (`std_msgs/Bool`) are published **only on state changes** (when an obstacle appears or disappears).
* **Asynchronous Spin**: Keeps a fast loop rate to ensure DDS execution updates do not delay motor controls.
* **Dead-Reckoning Odometry**: Aggregates wheel rotations based on speed models to generate `nav_msgs/Odometry` messages over the network.

### 🦾 C. Robotic Arm ([RoboticArm.ino](file:///d:/arduino%20codes/Warehouse/Warehouse/RoboticArm/RoboticArm.ino))
* **Non-Blocking Step Sequencer**: Runs a `runSeqStep()` state machine inside `loop()`. Rather than blocking the ESP32 with C `delay(1000)` calls (which trigger the hardware Watchdog crash), it records `millis()` timestamps and shifts servo targets sequentially step-by-step.
* **Watchdog Feeding**: Proactively feeds the `esp_task_wdt_reset()` watchdog during long movements.

### 🐍 D. ROS2 Orchestrator Node ([warehouse_manager_node.py](file:///d:/arduino%20codes/Warehouse/Warehouse/warehouse_ros2/warehouse_ros2/warehouse_manager_node.py))
* **Main State Machine**: Implements state transitions (`IDLE` -> `AWAITING_ROBOT` -> `ARM_PICKING` -> `ROBOT_TRANSIT` -> `ARM_PLACING`).
* **Coordination Layer**: Acts as the central brain, bridging events coming from the ESP32-S3 Server and issuing commands to the AGV and Arm.

---

## 🎨 4. Frontend Web UI (HTML/CSS/JS)

The user dashboard and control console are served directly from the ESP32-S3 Server's flash memory.
* **Design Language**: Dark slate UI (`#0f172a`), utilizing responsive flex grids, CSS gradients, glowing percentage indicators, and custom micro-animations for cards.
* **Live Telemetry (JS)**: Uses standard JavaScript WebSockets (`new WebSocket()`) to receive JSON telemetry strings, parse them dynamically, and update the DOM elements without reloading the page.
* **Fallback API Polling**: If the WebSocket drops, the JS engine falls back to polling `/api/status` every 5 seconds.
* **Robotic Arm Console (`/robot` route)**: Spawns a dedicated manual control page that lets operators trigger manual Pick, Place, or Home movements, or inject custom raw instructions directly to the Robotic Arm.

---

## 🛠️ 5. Customization & Modification Guide

### 1. How to Add a New Robotic Arm Motion Sequence
In [RoboticArm.ino](file:///d:/arduino%20codes/Warehouse/Warehouse/RoboticArm/RoboticArm.ino):
1. Add new enum step definitions inside the `ArmSeq` enum (e.g., `SEQ_PICK_4`, `SEQ_INSPECT`).
2. Add your custom step case inside `runSeqStep()`:
   ```cpp
   case SEQ_INSPECT:
     setServos(90, 80, 100, 120); // Move arm to camera inspection pose
     pendingSeq = SEQ_PICK_3;      // Shift to next sequence state
     break;
   ```
3. Trigger it inside the command subscription callback `commandCallback()`.

### 2. How to Modify Robot Drive Speeds
In [MobileRobot.ino](file:///d:/arduino%20codes/Warehouse/Warehouse/MobileRobot/MobileRobot.ino):
* Adjust `WHEEL_BASE_M` (wheel separation distance in meters), `SPEED_MPS` (meters/second velocity), and `TURN_RADS` (rad/s turn speed) to calibrate the dead-reckoning odometry matching your physical AGV chassis.

### 3. How to Expand Storage Capacity Parameters
In [server2.ino](file:///d:/arduino%20codes/Warehouse/Warehouse/server2/server2.ino):
* Modify the `Inventory` struct (around lines 87-95) to adjust constants:
  ```cpp
  const int rackACapacity = 1000; // Increase Sector A limit to 1000 units
  ```
