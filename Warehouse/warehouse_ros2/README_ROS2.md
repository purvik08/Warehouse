# 🤖 ROS2 Integration — Warehouse Automation System
**ROS2 Humble Hawksbill | Raspberry Pi 5 | micro-ROS on ESP32**

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│               Raspberry Pi 5  (Ubuntu 22.04)                 │
│                                                              │
│  ┌────────────────────────┐  ┌──────────────────────────┐   │
│  │  micro-ROS UDP Agent   │  │  warehouse_manager_node  │   │
│  │  port 8888             │  │  (mission orchestrator)  │   │
│  └────────────┬───────────┘  └──────────┬───────────────┘   │
│               │ DDS topics               │                   │
│  ┌────────────▼──────────────────────────▼───────────────┐  │
│  │           ROS2 DDS Middleware (Humble/CycloneDDS)      │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌────────────────────┐  ┌──────────────────────────────┐   │
│  │  fleet_monitor     │  │  nav2_bridge_node            │   │
│  │  (terminal dashboard)│  │  (NavigateToPose → Nav2)   │   │
│  └────────────────────┘  └──────────────────────────────┘   │
└───────────────────────────┬──────────────────────────────────┘
                            │  WiFi UDP (micro-ROS transport)
          ┌─────────────────┼──────────────────┐
          │                 │                  │
   ┌──────▼──────┐  ┌───────▼──────┐  ┌────────▼────────┐
   │ ESP32-S3    │  │ ESP32 (AGV)  │  │ ESP32 (Arm)     │
   │ Server +    │  │ MobileRobot  │  │ RoboticArm      │
   │ Web UI      │  │ micro-ROS    │  │ micro-ROS       │
   │ micro-ROS   │  │ pub+sub      │  │ pub+sub+OTA     │
   └─────────────┘  └──────────────┘  └─────────────────┘
```

---

## ROS2 Topic Map

| Topic | Msg Type | Publisher | Subscriber |
|---|---|---|---|
| `/warehouse/agv/status` | `std_msgs/String` | ESP32 AGV | manager, monitor |
| `/warehouse/agv/battery` | `sensor_msgs/BatteryState` | ESP32 AGV | manager, monitor |
| `/warehouse/agv/location` | `std_msgs/String` | ESP32 AGV | manager, nav2_bridge |
| `/warehouse/agv/obstacle` | `std_msgs/Bool` | ESP32 AGV | manager, monitor |
| `/warehouse/agv/odom` | `nav_msgs/Odometry` | ESP32 AGV | nav2_bridge, Nav2 |
| `/warehouse/agv/command` | `std_msgs/String` | RPi5 manager | ESP32 AGV |
| `/warehouse/agv/cmd_vel` | `geometry_msgs/Twist` | Nav2 stack | ESP32 AGV |
| `/warehouse/arm/status` | `std_msgs/String` | ESP32 Arm | manager, monitor |
| `/warehouse/arm/battery` | `sensor_msgs/BatteryState` | ESP32 Arm | monitor |
| `/warehouse/arm/command` | `std_msgs/String` | RPi5 manager | ESP32 Arm |
| `/warehouse/server/rfid` | `std_msgs/String` | ESP32-S3 Server | manager, monitor |
| `/warehouse/server/inventory` | `std_msgs/String` | ESP32-S3 Server | manager, monitor |
| `/warehouse/mission/status` | `std_msgs/String` | RPi5 manager | monitor, nav2 |
| `/warehouse/nav2/navigate_to` | `std_msgs/String` | manager | nav2_bridge |
| `/warehouse/nav2/goal_reached` | `std_msgs/String` | nav2_bridge | manager |
| `/diagnostics` | `diagnostic_msgs/DiagnosticArray` | manager | rqt |

---

## Bugs Fixed in This Update

| # | File | Bug | Fix |
|---|---|---|---|
| 1 | `server2.ino` | `while(!Serial)` hangs on battery boot (no USB) | 3-second timeout |
| 2 | `MobileRobot.ino` | `stopMotors()` called in `setup()` before WiFi → HTTP POST fails silently every boot | Removed from setup, raw GPIO init instead |
| 3 | `MobileRobot.ino` | Obstacle event spam — `notifyServerEvent()` fired every loop cycle while obstacle present | State-change only triggering (publish on enter/exit) |
| 4 | `RoboticArm.ino` | `moveToPosition()` with 4× `delay(1000)` blocks WDT reset during complex sequences → WDT reset crash risk | Non-blocking `millis()`-based step machine with WDT reset at each step |
| 5 | `server2.ino` | `handleRobotStatusUpdate()` reads `doc["robot"]` but AGV sends `doc["device"]` → robot never registered properly | Both keys now accepted (already present in existing code) |
| 6 | `server2.ino` | `notifyRobot()` and `completePlacement()` called but not defined → **compilation error** | Both functions confirmed present at lines 1186+/1206+ |

> **Note on bugs 5 & 6:** After reading the full file, `notifyRobot()` and `completePlacement()` ARE defined (lines 1186 and 1206) but declared AFTER their call sites — this compiles fine in C++ with implicit declarations but is poor practice. The micro-ROS addition adds forward declarations at the top to be explicit.

---

## Setup Guide

### Step 1 — Raspberry Pi 5 Setup

```bash
# On RPi5 (Ubuntu 22.04):
cd /path/to/warehouse_ros2/scripts/
chmod +x install_ros2_humble.sh
./install_ros2_humble.sh
```

The script installs:
- ROS2 Humble desktop
- Nav2 full stack
- micro-ROS agent (built from source)
- `warehouse_ros2` package
- Systemd auto-start service

### Step 2 — Configure ESP32 Agent IP

In each firmware, set the fallback IP to your RPi5's static IP:

**MobileRobot.ino** (line ~93):
```cpp
strncpy(agentIP, "192.168.1.100", sizeof(agentIP));  // ← your RPi5 IP
```

**RoboticArm.ino** (line ~105):
```cpp
strncpy(agentIP, "192.168.1.100", sizeof(agentIP));  // ← your RPi5 IP
```

**server2.ino** (line ~46):
```cpp
#define MICROROS_AGENT_IP   "192.168.1.100"   // ← your RPi5 IP
```

> **Tip:** Set a static IP on your RPi5 via `/etc/netplan/` and use the mDNS hostname `warehouse-rpi.local` for automatic resolution.

### Step 3 — Arduino Library Requirements

Install these in Arduino IDE (Tools → Manage Libraries):

| Library | Version | Purpose |
|---|---|---|
| `micro_ros_arduino` | v2.0.7-humble | ROS2 micro-ROS (must match distro) |
| `WiFiManager` | 2.0.17+ | WiFi provisioning |
| `MFRC522` | 1.4.10+ | RFID reader |
| `ArduinoJson` | 6.x | JSON (server only) |
| `ESP32Servo` | 0.13+ | Servo control (arm only) |
| `WebSockets` | 2.4+ | WebSocket server (server only) |
| `ElegantOTA` | 3.x | OTA updates (server, arm) |

> **Critical:** Download `micro_ros_arduino` from:
> https://github.com/micro-ROS/micro_ros_arduino/releases
> Select the `humble` release. Install as .zip in Arduino IDE.

### Step 4 — Board Settings

| Device | Board | Partition Scheme |
|---|---|---|
| ESP32-S3 Server | `ESP32S3 Dev Module` | `Huge APP (3MB No OTA)` |
| ESP32 AGV | `ESP32 Dev Module` | `Default` |
| ESP32 Arm | `ESP32 Dev Module` | `Default` |

### Step 5 — First Boot Provisioning

1. Flash each ESP32
2. Each ESP32 creates a WiFi AP named `WarehouseConfig_XXX`
3. Connect your phone to it and enter your LAN credentials
4. The ESP32 saves credentials and connects automatically on reboot

### Step 6 — Launch ROS2 System

```bash
# On RPi5:
source ~/.bashrc

# Option A: Full launch (recommended)
ros2 launch warehouse_ros2 warehouse.launch.py

# Option B: Step by step
micro-ros-agent udp4 --port 8888 -v4 &    # Start agent
ros2 run warehouse_ros2 warehouse_manager & # Orchestrator
ros2 run warehouse_ros2 fleet_monitor       # Terminal dashboard

# Option C: Systemd (auto-start on boot)
sudo systemctl start warehouse-ros2
sudo systemctl status warehouse-ros2
```

---

## Useful ROS2 Commands

```bash
# Verify all topics are live
ros2 topic list

# Watch AGV status in real time
ros2 topic echo /warehouse/agv/status

# Watch AGV battery
ros2 topic echo /warehouse/agv/battery

# Manually send a command to the AGV
ros2 topic pub --once /warehouse/agv/command std_msgs/String "data: 'move_forward'"

# Manually command the arm
ros2 topic pub --once /warehouse/arm/command std_msgs/String "data: 'pick'"

# Check node graph
ros2 node list
rqt_graph   # visual node/topic diagram

# Check diagnostics
ros2 topic echo /diagnostics

# Record all topics for analysis
ros2 bag record -a -o warehouse_session_1

# Replay a bag
ros2 bag play warehouse_session_1
```

---

## Nav2 Integration Notes

The `nav2_bridge_node` connects to Nav2's `NavigateToPose` action server.

**Landmark Map** (edit in `nav2_bridge_node.py`):
```python
LANDMARK_MAP = {
    'LOC-HOME':    (0.00,  0.00,  0.00),   # Home / Pickup
    'LOC-RACK-A':  (3.00,  0.00,  0.00),   # Rack A: 3m ahead
    'LOC-RACK-B':  (3.00,  1.50,  1.57),   # Rack B
    'LOC-RACK-C':  (3.00, -1.50, -1.57),   # Rack C
}
```

**How RFID enables localisation:**
1. AGV scans floor RFID tag → publishes to `/warehouse/agv/location`
2. `nav2_bridge_node` receives tag, looks up coordinates from `LANDMARK_MAP`
3. Publishes `/initialpose` with high-confidence covariance
4. AMCL localiser snaps robot position to the known landmark

**To create a warehouse map:**
```bash
# Install slam-toolbox (included in install script)
ros2 launch slam_toolbox online_sync_launch.py

# Drive robot around manually to build map
# Save map:
ros2 run nav2_map_server map_saver_cli -f ~/warehouse_map
```

---

## File Structure

```
warehouse_ros2/                  ← ROS2 Python package (deploy to RPi5)
├── package.xml
├── setup.py
├── resource/warehouse_ros2
├── config/
│   └── warehouse_params.yaml    ← Node parameters (timeouts, thresholds)
├── launch/
│   └── warehouse.launch.py      ← Main launch file
├── scripts/
│   └── install_ros2_humble.sh   ← One-shot RPi5 setup script
└── warehouse_ros2/
    ├── __init__.py
    ├── warehouse_manager_node.py  ← Mission orchestrator
    ├── fleet_monitor_node.py      ← Terminal telemetry dashboard
    └── nav2_bridge_node.py        ← Nav2 waypoint dispatcher

MobileRobot/MobileRobot.ino      ← ESP32 AGV firmware (micro-ROS)
RoboticArm/RoboticArm.ino        ← ESP32 Arm firmware (micro-ROS)
server2/server2.ino              ← ESP32-S3 Server (web UI + micro-ROS pub)
```
