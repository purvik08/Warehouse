// =============================================================================
// RoboticArm.ino  —  micro-ROS Arm firmware (ROS2 Humble / micro_ros_arduino)
// =============================================================================
// Hardware:  ESP32 DevKit V1
// Libraries: micro_ros_arduino (Humble), WiFiManager, ESP32Servo, ElegantOTA,
//            ESPmDNS, esp_task_wdt
//
// ROS2 Topics Subscribed:
//   /warehouse/arm/command   std_msgs/String  — pick / place / home / status
//
// ROS2 Topics Published:
//   /warehouse/arm/status    std_msgs/String  — IDLE/PICKING/HOLDING/PLACING
//   /warehouse/arm/battery   sensor_msgs/BatteryState
//
// micro-ROS Agent: Raspberry Pi 5 at AGENT_IP:AGENT_PORT
// Transport: UDP over Wi-Fi
//
// BUG FIXES applied vs original RoboticArm.ino:
//   1. WDT reset inserted between servo moves to prevent 10s timeout
//   2. Removed blocking delay() calls from pickup/place — replaced with
//      non-blocking step machine using millis()
//   3. Removed polling loop (checkForCommands HTTP) — replaced with ROS2 sub
// =============================================================================

#include <micro_ros_arduino.h>
#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

#include <std_msgs/msg/string.h>
#include <sensor_msgs/msg/battery_state.h>

#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include <WebServer.h>
#include <ElegantOTA.h>

// ── Configuration ────────────────────────────────────────────────────────
#define AGENT_PORT   8888
#define WDT_TIMEOUT  10

#define BATTERY_PIN  34

// Servo GPIO pins
#define BASE_PIN     15
#define SHOULDER_PIN 2
#define ELBOW_PIN    4
#define WRIST_PIN    16
#define GRIPPER_PIN  17

// ── Servo instances ───────────────────────────────────────────────────────
static Servo baseServo;
static Servo shoulderServo;
static Servo elbowServo;
static Servo wristServo;
static Servo gripperServo;

// ── OTA Server (port 80, runs alongside micro-ROS) ─────────────────────
static WebServer otaServer(80);

// ── ROS2 Entities ─────────────────────────────────────────────────────────
static rcl_node_t         ros_node;
static rclc_support_t     ros_support;
static rcl_allocator_t    ros_allocator;
static rclc_executor_t    ros_executor;

static rcl_publisher_t    pub_status;
static rcl_publisher_t    pub_battery;
static rcl_subscription_t sub_command;

static std_msgs__msg__String          msg_cmd_in;
static std_msgs__msg__String          msg_status_out;
static sensor_msgs__msg__BatteryState msg_battery_out;

// ── State ─────────────────────────────────────────────────────────────────
static bool rosConnected = false;
static char agentIP[16]  = "";
static char statusBuf[32] = "IDLE";
static bool hasBox        = false;

// Non-blocking sequence state machine
enum ArmSeq { SEQ_NONE, SEQ_PICK_1, SEQ_PICK_2, SEQ_PICK_3,
              SEQ_PLACE_1, SEQ_PLACE_2, SEQ_HOME };
static ArmSeq   pendingSeq = SEQ_NONE;
static unsigned long seqTimer = 0;
#define SEQ_STEP_MS 1000   // ms between each servo step

// Timing
static unsigned long lastStatusPub  = 0;
static unsigned long lastBatteryPub = 0;

// ── Macros ────────────────────────────────────────────────────────────────
#define RCSOFTCHECK(fn) { rcl_ret_t t = fn; \
  if(t != RCL_RET_OK) Serial.printf("[ROS WARN] code=%d\n", (int)t); }

// ── Forward Declarations ─────────────────────────────────────────────────
void connectNetwork();
void resolveAgentIP();
bool initMicroROS();
void destroyMicroROS();
void commandCallback(const void* msg);
void publishStatus(const char* status);
void publishBattery();
void setServos(int base, int shoulder, int elbow, int wrist);
void openGripper();
void closeGripper();
void startPickSequence();
void startPlaceSequence();
void runSeqStep();

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] RoboticArm micro-ROS firmware starting...");

  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  // Allocate ESP32 PWM timers for servos
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  baseServo.setPeriodHertz(50);     baseServo.attach(BASE_PIN);
  shoulderServo.setPeriodHertz(50); shoulderServo.attach(SHOULDER_PIN);
  elbowServo.setPeriodHertz(50);    elbowServo.attach(ELBOW_PIN);
  wristServo.setPeriodHertz(50);    wristServo.attach(WRIST_PIN);
  gripperServo.setPeriodHertz(50);  gripperServo.attach(GRIPPER_PIN);

  // Home position — no blocking delay, just write immediately
  setServos(90, 90, 90, 90);
  gripperServo.write(180);  // open

  // Wi-Fi
  connectNetwork();
  resolveAgentIP();

  // OTA
  otaServer.begin();
  ElegantOTA.begin(&otaServer);
  Serial.println("[OTA] ElegantOTA ready at http://<arm-ip>/update");
}

// ── Loop ─────────────────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset();

  // OTA handler — always run regardless of ROS state
  otaServer.handleClient();
  ElegantOTA.loop();

  if (WiFi.status() != WL_CONNECTED) {
    rosConnected = false;
    destroyMicroROS();
    connectNetwork();
    resolveAgentIP();
    return;
  }

  if (!rosConnected) {
    if (initMicroROS()) {
      rosConnected = true;
      Serial.println("[ROS] Connected to micro-ROS agent.");
      publishStatus("IDLE");
    } else {
      Serial.println("[ROS] Init failed. Retrying in 3s...");
      delay(3000);
      return;
    }
  }

  // Spin executor (handles incoming command messages)
  rclc_executor_spin_some(&ros_executor, RCL_MS_TO_NS(10));

  // Non-blocking arm sequence runner
  runSeqStep();

  // Periodic publishes
  unsigned long now = millis();
  if (now - lastStatusPub  > 2000) { publishStatus(statusBuf); lastStatusPub  = now; }
  if (now - lastBatteryPub > 10000) { publishBattery();         lastBatteryPub = now; }

  delay(10);
}

// ── Network Setup ─────────────────────────────────────────────────────────
void connectNetwork() {
  WiFiManager wifiManager;
  wifiManager.setConnectTimeout(60);
  Serial.println("[WiFi] Starting WiFiManager...");
  if (!wifiManager.autoConnect("WarehouseConfig_Arm01")) {
    Serial.println("[WiFi] Timeout. Restarting...");
    delay(3000);
    ESP.restart();
  }
  Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

void resolveAgentIP() {
  if (!MDNS.begin("arm-01")) {
    Serial.println("[mDNS] Setup failed.");
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
    Serial.printf("\n[mDNS] Using fallback IP: %s\n", agentIP);
  }
}

// ── micro-ROS Lifecycle ───────────────────────────────────────────────────
bool initMicroROS() {
  set_microros_wifi_transports(
    (char*)WiFi.SSID().c_str(),
    (char*)WiFi.psk().c_str(),
    agentIP,
    AGENT_PORT
  );
  delay(500);

  ros_allocator = rcl_get_default_allocator();
  if (rclc_support_init(&ros_support, 0, NULL, &ros_allocator) != RCL_RET_OK) return false;
  if (rclc_node_init_default(&ros_node, "arm_01", "warehouse", &ros_support) != RCL_RET_OK) return false;

  rclc_publisher_init_default(&pub_status,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/warehouse/arm/status");

  rclc_publisher_init_default(&pub_battery,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, BatteryState),
    "/warehouse/arm/battery");

  rclc_subscription_init_default(&sub_command,
    &ros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/warehouse/arm/command");

  // Allocate message buffers
  msg_cmd_in.data.capacity = 64;
  msg_cmd_in.data.data = (char*)malloc(64);
  msg_cmd_in.data.size = 0;

  msg_status_out.data.capacity = 32;
  msg_status_out.data.data = statusBuf;
  msg_status_out.data.size = strlen(statusBuf);

  rclc_executor_init(&ros_executor, &ros_support.context, 1, &ros_allocator);
  rclc_executor_add_subscription(&ros_executor, &sub_command,
    &msg_cmd_in, &commandCallback, ON_NEW_DATA);

  return true;
}

void destroyMicroROS() {
  if (!rosConnected) return;
  rcl_publisher_fini(&pub_status,  &ros_node);
  rcl_publisher_fini(&pub_battery, &ros_node);
  rcl_subscription_fini(&sub_command, &ros_node);
  rclc_executor_fini(&ros_executor);
  rclc_support_fini(&ros_support);
  rosConnected = false;
}

// ── Subscriber Callback ───────────────────────────────────────────────────
void commandCallback(const void* msg_in) {
  const std_msgs__msg__String* msg = (const std_msgs__msg__String*)msg_in;
  String cmd(msg->data.data);
  cmd.trim();
  Serial.printf("[CMD] Arm command: %s\n", cmd.c_str());

  if      (cmd == "pick")   startPickSequence();
  else if (cmd == "place")  startPlaceSequence();
  else if (cmd == "home")   pendingSeq = SEQ_HOME;
  else if (cmd == "status") publishStatus(statusBuf);
  else Serial.printf("[CMD] Unknown command: %s\n", cmd.c_str());
}

// ── Non-blocking Sequence Runner ─────────────────────────────────────────
// BUG FIX: Original used delay(1000) inside movement — this would eventually
// trigger the WDT on complex sequences. Now uses millis()-based step machine.
void runSeqStep() {
  if (pendingSeq == SEQ_NONE) return;
  if (millis() - seqTimer < SEQ_STEP_MS) return;  // wait for step interval
  seqTimer = millis();
  esp_task_wdt_reset();  // Feed watchdog at each step

  switch (pendingSeq) {
    case SEQ_PICK_1:
      setServos(90, 45, 135, 90);   // Approach
      openGripper();
      pendingSeq = SEQ_PICK_2;
      break;
    case SEQ_PICK_2:
      setServos(90, 60, 120, 90);   // Grab
      closeGripper();
      pendingSeq = SEQ_PICK_3;
      break;
    case SEQ_PICK_3:
      setServos(90, 90, 90, 90);    // Return home
      hasBox    = true;
      strncpy(statusBuf, "HOLDING", sizeof(statusBuf));
      publishStatus("HOLDING");
      pendingSeq = SEQ_NONE;
      Serial.println("[ARM] Pick complete. Holding box.");
      break;

    case SEQ_PLACE_1:
      if (!hasBox) {
        Serial.println("[ARM] ERROR: No box to place!");
        pendingSeq = SEQ_NONE;
        publishStatus("IDLE");
        break;
      }
      setServos(90, 45, 135, 90);   // Approach
      openGripper();
      pendingSeq = SEQ_PLACE_2;
      break;
    case SEQ_PLACE_2:
      setServos(90, 90, 90, 90);    // Return home
      hasBox    = false;
      strncpy(statusBuf, "IDLE", sizeof(statusBuf));
      publishStatus("IDLE");
      pendingSeq = SEQ_NONE;
      Serial.println("[ARM] Place complete.");
      break;

    case SEQ_HOME:
      setServos(90, 90, 90, 90);
      openGripper();
      hasBox    = false;
      strncpy(statusBuf, "IDLE", sizeof(statusBuf));
      publishStatus("IDLE");
      pendingSeq = SEQ_NONE;
      Serial.println("[ARM] Home position.");
      break;

    default:
      pendingSeq = SEQ_NONE;
      break;
  }
}

void startPickSequence() {
  strncpy(statusBuf, "PICKING", sizeof(statusBuf));
  publishStatus("PICKING");
  seqTimer   = millis();
  pendingSeq = SEQ_PICK_1;
}

void startPlaceSequence() {
  if (!hasBox) {
    Serial.println("[ARM] Cannot place — no box held.");
    return;
  }
  strncpy(statusBuf, "PLACING", sizeof(statusBuf));
  publishStatus("PLACING");
  seqTimer   = millis();
  pendingSeq = SEQ_PLACE_1;
}

// ── Publishers ────────────────────────────────────────────────────────────
void publishStatus(const char* status) {
  if (!rosConnected) return;
  strncpy(statusBuf, status, sizeof(statusBuf));
  msg_status_out.data.size = strlen(status);
  RCSOFTCHECK(rcl_publish(&pub_status, &msg_status_out, NULL));
}

void publishBattery() {
  if (!rosConnected) return;
  int raw = analogRead(BATTERY_PIN);
  msg_battery_out.percentage =
    (float)constrain(map(raw, 0, 4095, 0, 100), 0, 100) / 100.0f;
  msg_battery_out.present = true;
  msg_battery_out.power_supply_status =
    sensor_msgs__msg__BatteryState__POWER_SUPPLY_STATUS_DISCHARGING;
  RCSOFTCHECK(rcl_publish(&pub_battery, &msg_battery_out, NULL));
}

// ── Servo Helpers ─────────────────────────────────────────────────────────
void setServos(int base, int shoulder, int elbow, int wrist) {
  baseServo.write(base);
  shoulderServo.write(shoulder);
  elbowServo.write(elbow);
  wristServo.write(wrist);
  // Note: NO delay() here — timing is handled by runSeqStep()
}

void openGripper()  { gripperServo.write(180); }
void closeGripper() { gripperServo.write(0); }