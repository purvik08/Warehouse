// =============================================================================
// MobileRobot.ino  —  micro-ROS AGV firmware (ROS2 Humble / micro_ros_arduino)
// =============================================================================
// Hardware:  ESP32 DevKit V1
// Libraries: micro_ros_arduino (Humble), WiFiManager, MFRC522, ESPmDNS,
//            esp_task_wdt, ArduinoJson
//
// ROS2 Topics Published:
//   /warehouse/agv/status    std_msgs/String    — IDLE/MOVING/TURNING/OBSTACLE
//   /warehouse/agv/battery   sensor_msgs/BatteryState
//   /warehouse/agv/location  std_msgs/String    — RFID floor tag payload
//   /warehouse/agv/obstacle  std_msgs/Bool      — true = obstacle present
//   /warehouse/agv/odom      nav_msgs/Odometry  — dead-reckoning odometry
//
// ROS2 Topics Subscribed:
//   /warehouse/agv/command   std_msgs/String    — move_forward/turn_left/
//                                                  turn_right/stop
//   /warehouse/agv/cmd_vel   geometry_msgs/Twist — Nav2-compatible velocity
//
// micro-ROS Agent: Raspberry Pi 5 at AGENT_IP:AGENT_PORT
// Transport: UDP over Wi-Fi (must be on same LAN as RPi5)
// =============================================================================

// ── Includes ────────────────────────────────────────────────────────────────
#include <micro_ros_arduino.h>
#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

// ROS2 message types
#include <std_msgs/msg/string.h>
#include <std_msgs/msg/bool.h>
#include <sensor_msgs/msg/battery_state.h>
#include <geometry_msgs/msg/twist.h>
#include <nav_msgs/msg/odometry.h>

// ESP32 libraries
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include <SPI.h>
#include <MFRC522.h>

// ── Configuration ────────────────────────────────────────────────────────────
#define AGENT_PORT      8888
#define WDT_TIMEOUT     10      // Watchdog: 10 seconds

// Motor pins (L298N)
#define MOTOR_A_IN1     26
#define MOTOR_A_IN2     25
#define MOTOR_B_IN3     33
#define MOTOR_B_IN4     32

// Sensor pins
#define BATTERY_PIN     34
#define TRIG_PIN        14
#define ECHO_PIN        27

// RFID (MFRC522)
#define SS_PIN          5
#define RST_PIN         0

// Odometry constants — CALIBRATE THESE for your robot
#define WHEEL_BASE_M    0.18f   // distance between wheels in metres
#define SPEED_MPS       0.25f   // approximate speed when moving forward (m/s)
#define TURN_RADS       1.57f   // approximate rad/s when turning

// ── ROS2 Entities ─────────────────────────────────────────────────────────
static rcl_node_t          ros_node;
static rclc_support_t      ros_support;
static rcl_allocator_t     ros_allocator;
static rclc_executor_t     ros_executor;

// Publishers
static rcl_publisher_t     pub_status;
static rcl_publisher_t     pub_battery;
static rcl_publisher_t     pub_location;
static rcl_publisher_t     pub_obstacle;
static rcl_publisher_t     pub_odom;

// Subscribers
static rcl_subscription_t  sub_command;
static rcl_subscription_t  sub_cmd_vel;

// Messages
static std_msgs__msg__String           msg_status;
static sensor_msgs__msg__BatteryState  msg_battery;
static std_msgs__msg__String           msg_location;
static std_msgs__msg__Bool             msg_obstacle;
static nav_msgs__msg__Odometry         msg_odom;
static std_msgs__msg__String           msg_command_in;
static geometry_msgs__msg__Twist       msg_cmd_vel_in;

// ── State ─────────────────────────────────────────────────────────────────
static MFRC522 rfid(SS_PIN, RST_PIN);
static bool    rosConnected     = false;
static char    agentIP[16]      = "";   // RPi5 IP resolved via mDNS
static char    statusBuf[32]    = "IDLE";
static char    locationBuf[64]  = "";
static bool    obstacleActive   = false;

// Odometry state
static float   odom_x     = 0.0f;
static float   odom_y     = 0.0f;
static float   odom_theta = 0.0f;
static unsigned long lastOdomUpdate = 0;
static bool    isMovingForward = false;
static bool    isTurningLeft   = false;
static bool    isTurningRight  = false;

// Timing
static unsigned long lastStatusPub  = 0;
static unsigned long lastBatteryPub = 0;
static unsigned long lastOdomPub    = 0;

// ── Error handling macro ──────────────────────────────────────────────────
#define RCCHECK(fn) { rcl_ret_t temp_rc = fn; \
  if(temp_rc != RCL_RET_OK){ handleRosError(); return; } }
#define RCSOFTCHECK(fn) { rcl_ret_t temp_rc = fn; \
  if(temp_rc != RCL_RET_OK){ Serial.println("[WARN] ROS soft error"); } }

// ── Forward Declarations ─────────────────────────────────────────────────
void connectNetwork();
void resolveAgentIP();
bool initMicroROS();
void destroyMicroROS();
void handleRosError();
void commandCallback(const void * msg_in);
void cmdVelCallback(const void * msg_in);
void publishStatus(const char* status);
void publishBattery();
void publishObstacle(bool detected);
void publishLocation(const char* tag);
void publishOdometry();
void updateOdometry();
void checkObstacle();
void processRFID();
void moveForward();
void turnLeft();
void turnRight();
void stopMotors();

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] MobileRobot micro-ROS firmware starting...");

  // Watchdog
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  // Motor GPIO
  pinMode(MOTOR_A_IN1, OUTPUT);
  pinMode(MOTOR_A_IN2, OUTPUT);
  pinMode(MOTOR_B_IN3, OUTPUT);
  pinMode(MOTOR_B_IN4, OUTPUT);

  // Sensor GPIO
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // ── BUG FIX: Do NOT call stopMotors() here — notifyServerEvent inside it
  //    would fire before any connection is established. Just zero the pins.
  digitalWrite(MOTOR_A_IN1, LOW); digitalWrite(MOTOR_A_IN2, LOW);
  digitalWrite(MOTOR_B_IN3, LOW); digitalWrite(MOTOR_B_IN4, LOW);
  strncpy(statusBuf, "IDLE", sizeof(statusBuf));

  // RFID
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("[BOOT] RFID reader initialised.");

  // Wi-Fi (WiFiManager handles first-boot provisioning)
  connectNetwork();
  resolveAgentIP();
}

// ── Loop ──────────────────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset();

  // Re-connect Wi-Fi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    rosConnected = false;
    destroyMicroROS();
    connectNetwork();
    resolveAgentIP();
    return;
  }

  // (Re-)initialise micro-ROS if not connected
  if (!rosConnected) {
    Serial.println("[ROS] Attempting micro-ROS connection...");
    if (initMicroROS()) {
      rosConnected = true;
      Serial.println("[ROS] Connected to micro-ROS agent.");
    } else {
      Serial.println("[ROS] Failed. Retrying in 3s...");
      delay(3000);
      return;
    }
  }

  // Spin the executor (handles incoming command/cmd_vel messages)
  rclc_executor_spin_some(&ros_executor, RCL_MS_TO_NS(10));

  // Sensor loop
  checkObstacle();
  processRFID();
  updateOdometry();

  // Periodic publishes
  unsigned long now = millis();

  if (now - lastStatusPub > 1000) {
    publishStatus(statusBuf);
    lastStatusPub = now;
  }
  if (now - lastBatteryPub > 5000) {
    publishBattery();
    lastBatteryPub = now;
  }
  if (now - lastOdomPub > 100) {   // 10 Hz odometry
    publishOdometry();
    lastOdomPub = now;
  }

  delay(10);
}

// ── Network Setup ─────────────────────────────────────────────────────────
void connectNetwork() {
  WiFiManager wifiManager;
  wifiManager.setConnectTimeout(60);
  Serial.println("[WiFi] Starting WiFiManager...");
  if (!wifiManager.autoConnect("WarehouseConfig_LF01")) {
    Serial.println("[WiFi] Timeout. Restarting...");
    delay(3000);
    ESP.restart();
  }
  Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

void resolveAgentIP() {
  // Try mDNS: RPi5 should advertise as 'warehouse-rpi.local'
  if (!MDNS.begin("agv-lf01")) {
    Serial.println("[mDNS] Setup failed for this node.");
  }

  Serial.print("[mDNS] Resolving warehouse-rpi.local...");
  int retries = 0;
  while (strlen(agentIP) == 0 && retries < 20) {
    esp_task_wdt_reset();
    IPAddress ip = MDNS.queryHost("warehouse-rpi");
    if (ip.toString() != "0.0.0.0") {
      ip.toString().toCharArray(agentIP, sizeof(agentIP));
      Serial.printf(" Resolved: %s\n", agentIP);
    } else {
      delay(1000);
      Serial.print(".");
      retries++;
    }
  }

  if (strlen(agentIP) == 0) {
    // ── IMPORTANT: Set your RPi5's static IP here as fallback ────────────
    strncpy(agentIP, "192.168.1.100", sizeof(agentIP));
    Serial.printf("\n[mDNS] Failed. Using fallback IP: %s\n", agentIP);
  }
}

// ── micro-ROS Lifecycle ───────────────────────────────────────────────────
bool initMicroROS() {
  // Transport: UDP to RPi5 micro-ROS agent
  IPAddress agentAddr;
  if (!agentAddr.fromString(agentIP)) return false;

  set_microros_wifi_transports(
    (char*)WiFi.SSID().c_str(),
    (char*)WiFi.psk().c_str(),
    agentIP,
    AGENT_PORT
  );

  delay(500);

  ros_allocator = rcl_get_default_allocator();

  // Init support (RMW + ROS2 context)
  if (rclc_support_init(&ros_support, 0, NULL, &ros_allocator) != RCL_RET_OK)
    return false;

  // Create node
  if (rclc_node_init_default(&ros_node, "agv_lf01", "warehouse", &ros_support) != RCL_RET_OK)
    return false;

  // ── Publishers ─────────────────────────────────────────────────────────
  rclc_publisher_init_default(&pub_status,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/warehouse/agv/status");

  rclc_publisher_init_default(&pub_battery,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, BatteryState),
    "/warehouse/agv/battery");

  rclc_publisher_init_default(&pub_location,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/warehouse/agv/location");

  rclc_publisher_init_default(&pub_obstacle,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
    "/warehouse/agv/obstacle");

  rclc_publisher_init_default(&pub_odom,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(nav_msgs, msg, Odometry),
    "/warehouse/agv/odom");

  // ── Subscribers ────────────────────────────────────────────────────────
  rclc_subscription_init_default(&sub_command,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/warehouse/agv/command");

  rclc_subscription_init_default(&sub_cmd_vel,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist),
    "/warehouse/agv/cmd_vel");

  // Allocate string message storage
  msg_command_in.data.capacity = 64;
  msg_command_in.data.data = (char*)malloc(64);
  msg_command_in.data.size = 0;

  msg_status.data.capacity = 32;
  msg_status.data.data = statusBuf;
  msg_status.data.size = 0;

  msg_location.data.capacity = 64;
  msg_location.data.data = locationBuf;
  msg_location.data.size = 0;

  // ── Executor (2 subscriptions) ─────────────────────────────────────────
  rclc_executor_init(&ros_executor, &ros_support.context, 2, &ros_allocator);

  rclc_executor_add_subscription(&ros_executor, &sub_command,
    &msg_command_in, &commandCallback, ON_NEW_DATA);

  rclc_executor_add_subscription(&ros_executor, &sub_cmd_vel,
    &msg_cmd_vel_in, &cmdVelCallback, ON_NEW_DATA);

  return true;
}

void destroyMicroROS() {
  if (!rosConnected) return;
  rcl_publisher_fini(&pub_status,   &ros_node);
  rcl_publisher_fini(&pub_battery,  &ros_node);
  rcl_publisher_fini(&pub_location, &ros_node);
  rcl_publisher_fini(&pub_obstacle, &ros_node);
  rcl_publisher_fini(&pub_odom,     &ros_node);
  rcl_subscription_fini(&sub_command, &ros_node);
  rcl_subscription_fini(&sub_cmd_vel, &ros_node);
  rclc_executor_fini(&ros_executor);
  rclc_support_fini(&ros_support);
  rosConnected = false;
}

void handleRosError() {
  Serial.println("[ROS] ERROR — reconnecting...");
  destroyMicroROS();
  delay(2000);
}

// ── Subscriber Callbacks ──────────────────────────────────────────────────
void commandCallback(const void * msg_in) {
  const std_msgs__msg__String * msg = (const std_msgs__msg__String *)msg_in;
  String cmd(msg->data.data);
  cmd.trim();
  Serial.printf("[CMD] Received command: %s\n", cmd.c_str());

  // Only execute if no obstacle is blocking
  if (obstacleActive) {
    Serial.println("[CMD] Obstacle active — command rejected.");
    return;
  }

  if      (cmd == "move_forward") moveForward();
  else if (cmd == "turn_left")    turnLeft();
  else if (cmd == "turn_right")   turnRight();
  else if (cmd == "stop")         stopMotors();
  else Serial.printf("[CMD] Unknown command: %s\n", cmd.c_str());
}

void cmdVelCallback(const void * msg_in) {
  // Nav2-compatible Twist command interpretation
  const geometry_msgs__msg__Twist * twist = (const geometry_msgs__msg__Twist *)msg_in;
  float linear  = twist->linear.x;
  float angular = twist->angular.z;

  if (obstacleActive) return;

  if (linear > 0.05f && fabs(angular) < 0.1f) {
    moveForward();
  } else if (linear < -0.05f) {
    // Reverse not implemented on L298N layout — stop instead
    stopMotors();
  } else if (angular > 0.1f) {
    turnLeft();
  } else if (angular < -0.1f) {
    turnRight();
  } else {
    stopMotors();
  }
}

// ── Publishers ────────────────────────────────────────────────────────────
void publishStatus(const char* status) {
  if (!rosConnected) return;
  strncpy(statusBuf, status, sizeof(statusBuf));
  msg_status.data.size = strlen(status);
  RCSOFTCHECK(rcl_publish(&pub_status, &msg_status, NULL));
}

void publishBattery() {
  if (!rosConnected) return;
  int raw = analogRead(BATTERY_PIN);
  // ── BUG FIX: Use proper BatteryState fields, not a plain percentage
  msg_battery.percentage   = (float)constrain(map(raw, 0, 4095, 0, 100), 0, 100) / 100.0f;
  msg_battery.present      = true;
  msg_battery.power_supply_status = sensor_msgs__msg__BatteryState__POWER_SUPPLY_STATUS_DISCHARGING;
  RCSOFTCHECK(rcl_publish(&pub_battery, &msg_battery, NULL));
}

void publishObstacle(bool detected) {
  if (!rosConnected) return;
  msg_obstacle.data = detected;
  RCSOFTCHECK(rcl_publish(&pub_obstacle, &msg_obstacle, NULL));
}

void publishLocation(const char* tag) {
  if (!rosConnected) return;
  strncpy(locationBuf, tag, sizeof(locationBuf));
  msg_location.data.size = strlen(tag);
  RCSOFTCHECK(rcl_publish(&pub_location, &msg_location, NULL));
}

void publishOdometry() {
  if (!rosConnected) return;

  // Simple yaw-only quaternion from odom_theta
  float half_theta = odom_theta / 2.0f;
  msg_odom.header.frame_id.data = (char*)"odom";
  msg_odom.header.frame_id.size = 4;
  msg_odom.child_frame_id.data  = (char*)"base_link";
  msg_odom.child_frame_id.size  = 9;

  msg_odom.pose.pose.position.x = odom_x;
  msg_odom.pose.pose.position.y = odom_y;
  msg_odom.pose.pose.orientation.w = cosf(half_theta);
  msg_odom.pose.pose.orientation.z = sinf(half_theta);

  // Velocity
  msg_odom.twist.twist.linear.x  = isMovingForward ? SPEED_MPS : 0.0f;
  msg_odom.twist.twist.angular.z =
    isTurningLeft  ?  TURN_RADS :
    isTurningRight ? -TURN_RADS : 0.0f;

  RCSOFTCHECK(rcl_publish(&pub_odom, &msg_odom, NULL));
}

// ── Odometry Integration ──────────────────────────────────────────────────
void updateOdometry() {
  unsigned long now = millis();
  float dt = (now - lastOdomUpdate) / 1000.0f;
  lastOdomUpdate = now;

  if (dt <= 0 || dt > 1.0f) return;  // skip large gaps

  if (isMovingForward) {
    odom_x += SPEED_MPS * cosf(odom_theta) * dt;
    odom_y += SPEED_MPS * sinf(odom_theta) * dt;
  } else if (isTurningLeft) {
    odom_theta += TURN_RADS * dt;
  } else if (isTurningRight) {
    odom_theta -= TURN_RADS * dt;
  }
}

// ── Obstacle Detection ────────────────────────────────────────────────────
void checkObstacle() {
  // ── BUG FIX: Added state tracking to avoid constant event spam.
  //    Only fires publishObstacle() on state CHANGE.
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return;  // sensor timeout — ignore

  float distance = duration * 0.034f / 2.0f;

  if (distance > 0 && distance < 15.0f) {
    if (!obstacleActive) {
      obstacleActive = true;
      stopMotors();
      publishObstacle(true);
      publishStatus("OBSTACLE");
      Serial.println("[SENSOR] Obstacle detected. Motors stopped.");
    }
  } else {
    if (obstacleActive) {
      obstacleActive = false;
      publishObstacle(false);
      publishStatus(statusBuf);  // restore previous status
      Serial.println("[SENSOR] Obstacle cleared.");
    }
  }
}

// ── RFID Floor Tag Processing ─────────────────────────────────────────────
void processRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  byte buffer[18];
  byte len = 18;
  char tagPayload[64] = "";
  uint8_t payloadLen = 0;

  MFRC522::StatusCode status =
    rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 4, &key, &(rfid.uid));

  if (status == MFRC522::STATUS_OK) {
    status = rfid.MIFARE_Read(4, buffer, &len);
    if (status == MFRC522::STATUS_OK) {
      for (uint8_t i = 0; i < 16 && payloadLen < 63; i++) {
        if (buffer[i] >= 32 && buffer[i] <= 126) {
          tagPayload[payloadLen++] = (char)buffer[i];
        }
      }
      tagPayload[payloadLen] = '\0';

      // Trim trailing spaces
      while (payloadLen > 0 && tagPayload[payloadLen - 1] == ' ') {
        tagPayload[--payloadLen] = '\0';
      }
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (payloadLen > 0) {
    Serial.printf("[RFID] Floor tag: %s\n", tagPayload);
    publishLocation(tagPayload);
  }
}

// ── Motor Control ─────────────────────────────────────────────────────────
void moveForward() {
  isMovingForward = true; isTurningLeft = false; isTurningRight = false;
  strncpy(statusBuf, "MOVING", sizeof(statusBuf));
  digitalWrite(MOTOR_A_IN1, HIGH); digitalWrite(MOTOR_A_IN2, LOW);
  digitalWrite(MOTOR_B_IN3, HIGH); digitalWrite(MOTOR_B_IN4, LOW);
  publishStatus("MOVING");
  Serial.println("[MOTOR] Moving forward.");
}

void turnLeft() {
  isMovingForward = false; isTurningLeft = true; isTurningRight = false;
  strncpy(statusBuf, "TURNING", sizeof(statusBuf));
  digitalWrite(MOTOR_A_IN1, LOW);  digitalWrite(MOTOR_A_IN2, HIGH);
  digitalWrite(MOTOR_B_IN3, HIGH); digitalWrite(MOTOR_B_IN4, LOW);
  publishStatus("TURNING");
  Serial.println("[MOTOR] Turning left.");
}

void turnRight() {
  isMovingForward = false; isTurningLeft = false; isTurningRight = true;
  strncpy(statusBuf, "TURNING", sizeof(statusBuf));
  digitalWrite(MOTOR_A_IN1, HIGH); digitalWrite(MOTOR_A_IN2, LOW);
  digitalWrite(MOTOR_B_IN3, LOW);  digitalWrite(MOTOR_B_IN4, HIGH);
  publishStatus("TURNING");
  Serial.println("[MOTOR] Turning right.");
}

void stopMotors() {
  isMovingForward = false; isTurningLeft = false; isTurningRight = false;
  strncpy(statusBuf, "IDLE", sizeof(statusBuf));
  digitalWrite(MOTOR_A_IN1, LOW); digitalWrite(MOTOR_A_IN2, LOW);
  digitalWrite(MOTOR_B_IN3, LOW); digitalWrite(MOTOR_B_IN4, LOW);
  publishStatus("IDLE");
  Serial.println("[MOTOR] Stopped.");
}
