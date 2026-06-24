#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ElegantOTA.h>
#include <MFRC522.h>

// ── micro-ROS (ROS2 Humble bridge) ──────────────────────────────────────────
// The server ESP32-S3 publishes RFID scans and inventory snapshots to ROS2.
// All existing web dashboard code below is UNCHANGED.
#include <micro_ros_arduino.h>
#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <std_msgs/msg/string.h>

// micro-ROS entities (server node)
static rcl_node_t          mros_node;
static rclc_support_t      mros_support;
static rcl_allocator_t     mros_allocator;
static rclc_executor_t     mros_executor;
static rcl_publisher_t     pub_rfid;
static rcl_publisher_t     pub_inventory;
static std_msgs__msg__String msg_rfid_out;
static std_msgs__msg__String msg_inv_out;
static char rfidBuf[64]  = "";
static char invBuf[128]  = "";
static bool mrosReady    = false;

// IMPORTANT: Set this to your RPi5's IP address (or use mDNS warehouse-rpi.local)
#define MICROROS_AGENT_IP   "192.168.1.100"
#define MICROROS_AGENT_PORT 8888

#define RCSOFTCHECK_SVR(fn) { rcl_ret_t _t = fn; \
  if(_t != RCL_RET_OK) DEBUG_PRINTF("[mROS WARN] ret=%d\n",(int)_t); }

void initMicroROSServer();
void publishRFIDEvent(const char* tag);
void publishInventorySnapshot();
// ────────────────────────────────────────────────────────────────────────────

#define DEBUG 1

#if DEBUG
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif
#include <SPI.h>
#include <vector>
#include <algorithm> // for std::find

#define SS_PIN 5
#define RST_PIN 0
MFRC522 rfid(SS_PIN, RST_PIN);



#define DB_DIR "/db"
#define CONFIG_FILE DB_DIR "/config.json"
#define INVENTORY_FILE DB_DIR "/inventory.json"
#define ROBOTS_FILE DB_DIR "/robots.json"
#define TRANSACTIONS_FILE DB_DIR "/transactions.csv"
#define RFID_LOGS_FILE DB_DIR "/rfid_logs.csv"

#define BOX_TAG_PREFIX "BOX"
#define LOCATION_TAG_PREFIX "LOC"
#define TAG_LENGTH 10 // 3 chars prefix + 7 chars ID

#include <map>
std::vector<String> connectedDevices;
std::map<String, unsigned long> deviceLastSeen;
std::map<String, String> commandQueue;

struct {
  String teamName = "Team Lakshya";
} config;

struct Inventory {
  int rackA = 0;
  int rackB = 0;
  int rackC = 0;
  const int rackACapacity = 500;
  const int rackBCapacity = 600;
  const int rackCCapacity = 700;
} inventory;



String getBoxType(String boxTag) {
  if (!boxTag.startsWith(BOX_TAG_PREFIX)) return "INVALID";
  String boxCode = boxTag.substring(3, 6); // Extract type code
  
  if (boxCode == "AXX") return "TYPE_A";
  if (boxCode == "BXX") return "TYPE_B"; 
  if (boxCode == "CXX") return "TYPE_C";
  
  return "UNKNOWN";
}

WebServer server(80);
WebSocketsServer webSocket(81);

int countPendingForRack(String rack) {
  return 0; // pendingPlacements feature removed to fix memory leaks
}

int getRackAvailableCapacity(String rack) {
  if (rack == "RackA") return inventory.rackACapacity - inventory.rackA;
  if (rack == "RackB") return inventory.rackBCapacity - inventory.rackB;
  if (rack == "RackC") return inventory.rackCCapacity - inventory.rackC;
  return 0;
}

void initializeStorage() {
  DEBUG_PRINTLN("Initializing LittleFS...");

  if (!LittleFS.begin(true)) {
    DEBUG_PRINTLN("LittleFS Mount Failed");
    return;
  }
  DEBUG_PRINTLN("LittleFS mounted successfully");

  if (!LittleFS.exists(DB_DIR)) LittleFS.mkdir(DB_DIR);
  if (!LittleFS.exists(CONFIG_FILE)) saveConfig(); else loadConfig();
  if (!LittleFS.exists(INVENTORY_FILE)) saveInventory(); else loadInventory();
  if (!LittleFS.exists(TRANSACTIONS_FILE)) appendToFile(TRANSACTIONS_FILE, "timestamp,event,robot,location,box\n");
  if (!LittleFS.exists(RFID_LOGS_FILE)) appendToFile(RFID_LOGS_FILE, "timestamp,tag,location\n");
}

void saveDatabase() {
  saveConfig();
  saveInventory();
}

void saveConfig() {
  File file = LittleFS.open(CONFIG_FILE, FILE_WRITE);
  if (!file) return;
  DynamicJsonDocument doc(256);
  doc["team"] = config.teamName;
  serializeJson(doc, file);
  file.close();
}

void loadConfig() {
  File file = LittleFS.open(CONFIG_FILE, FILE_READ);
  if (!file) return;
  DynamicJsonDocument doc(256);
  deserializeJson(doc, file);
  config.teamName = doc["team"] | "Team Lakshya";
  file.close();
}

void saveInventory() {
  File file = LittleFS.open(INVENTORY_FILE, FILE_WRITE);
  if (!file) return;
  DynamicJsonDocument doc(256);
  doc["rackA"] = inventory.rackA;
  doc["rackB"] = inventory.rackB;
  doc["rackC"] = inventory.rackC;
  serializeJson(doc, file);
  file.close();
}

void loadInventory() {
  File file = LittleFS.open(INVENTORY_FILE, FILE_READ);
  if (!file) return;
  DynamicJsonDocument doc(256);
  deserializeJson(doc, file);
  inventory.rackA = doc["rackA"] | 0;
  inventory.rackB = doc["rackB"] | 0;
  inventory.rackC = doc["rackC"] | 0;
  file.close();
}

void appendToFile(String filename, String data) {
  File file = LittleFS.open(filename, FILE_APPEND);
  if (!file) {
    DEBUG_PRINTLN("Failed to open file: " + filename);
    return;
  }
  if (!file.print(data)) {
    DEBUG_PRINTLN("Write failed: " + filename);
  }
  file.close();
}

void processRFID() {
  String tag = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    tag += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    tag += String(rfid.uid.uidByte[i], HEX);
  }
  tag.toUpperCase();
  String logEntry = String(millis()) + "," + tag + ",Reader1\n";
  appendToFile(RFID_LOGS_FILE, logEntry);

  if (tag.startsWith("BOX")) {
    processBoxTag(tag);
  } else if (tag.startsWith("LOC")) {
    processLocationTag(tag);
  }

  // Publish RFID tag to ROS2 /warehouse/server/rfid
  publishRFIDEvent(tag.c_str());

  DynamicJsonDocument doc(128);
  doc["type"] = "rfid";
  doc["tag"] = tag;
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
}

void processBoxTag(String tag) {
  if (tag.indexOf("BOXA") >= 0) inventory.rackA++;
  else if (tag.indexOf("BOXB") >= 0) inventory.rackB++;
  else if (tag.indexOf("BOXC") >= 0) inventory.rackC++;

  String logEntry = String(millis()) + ",box_scanned,,," + tag + "\n";
  appendToFile(TRANSACTIONS_FILE, logEntry);

  DynamicJsonDocument doc(256);
  doc["type"] = "inventory";
  doc["rackA"] = inventory.rackA;
  doc["rackB"] = inventory.rackB;
  doc["rackC"] = inventory.rackC;
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
}

void processLocationTag(String tag) {
  String logEntry = String(millis()) + "," + tag + ",location_update\n";
  appendToFile(RFID_LOGS_FILE, logEntry);
  DynamicJsonDocument doc(128);
  doc["type"] = "location";
  doc["tag"] = tag;
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
}



void handleWebSocket(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  // Debug header
  DEBUG_PRINTF("[WebSocket][Client %u] Event: ", num);
  
  switch(type) {
    case WStype_DISCONNECTED:
      DEBUG_PRINTLN("Disconnected");
      break;

    case WStype_CONNECTED: {
      DEBUG_PRINTLN("Connected");
      // Send initial configuration
      DynamicJsonDocument doc(256);
      doc["type"] = "config";
      doc["team"] = config.teamName;
      String json;
      serializeJson(doc, json);
      webSocket.sendTXT(num, json);
      break;
    }

    case WStype_TEXT: {
      DEBUG_PRINTLN("Text Message Received");
      // Sandbox message processing
      if (length > 512) {  // Prevent overly large messages
        DEBUG_PRINTLN("Message too large, rejecting");
        webSocket.sendTXT(num, "{\"error\":\"message_too_large\"}");
        break;
      }

      String message = (char*)payload;
      DEBUG_PRINTLN("Raw message: " + message);

      // Safe JSON parsing
      DynamicJsonDocument doc(512);
      DeserializationError error = deserializeJson(doc, message);
      
      if (error) {
        DEBUG_PRINT("JSON parse error: ");
        DEBUG_PRINTLN(error.c_str());
        webSocket.sendTXT(num, "{\"error\":\"invalid_json\"}");
        break;
      }

      // Message type validation
      if (!doc.containsKey("type")) {
        DEBUG_PRINTLN("Missing message type");
        webSocket.sendTXT(num, "{\"error\":\"missing_type\"}");
        break;
      }

      String msgType = doc["type"].as<String>();
      DEBUG_PRINTLN("Processing message type: " + msgType);

      // Process different message types
      if (msgType == "command") {
        // Validate command structure
        if (!doc.containsKey("command")) {
          webSocket.sendTXT(num, "{\"error\":\"missing_command\"}");
          break;
        }
        
        String command = doc["command"].as<String>();
        DEBUG_PRINTLN("Executing command: " + command);
        
        // Send to robotic arm with validation
        if (command == "pick" || command == "place" || command == "home") {
          String targetArm = "Arm-01";
          for (const String& d : connectedDevices) { if(d.startsWith("Arm")) { targetArm = d; break; } }
          notifyRobot(targetArm, command, "");
          webSocket.sendTXT(num, "{\"status\":\"command_sent\"}");
        } else {
          webSocket.sendTXT(num, "{\"error\":\"invalid_command\"}");
        }
      }
      // Add other message types here...
      break;
    }

    case WStype_BIN:
      DEBUG_PRINTF("Binary message length: %u\n", length);
      // Echo back binary data for testing
      webSocket.sendBIN(num, payload, length);
      break;

    case WStype_ERROR:
      DEBUG_PRINTF("Error: %u\n", *payload);
      break;

    case WStype_PING:
      DEBUG_PRINTLN("Ping received");
      break;

    case WStype_PONG:
      DEBUG_PRINTLN("Pong received");
      break;

    default:
      DEBUG_PRINTF("Unhandled event type: %u\n", type);
      break;
  }

  // Debug footer
  DEBUG_PRINTF("[WebSocket][Client %u] Free Heap: %u bytes\n", 
               num, ESP.getFreeHeap());
}

void handleSettingsUpdate() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }

  String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  // Update configuration
  if (doc.containsKey("team")) config.teamName = doc["team"].as<String>();

  // Save to SD card
  saveConfig();

  server.send(200, "application/json", "{\"status\":\"success\"}");
}

void handleStatus() {
  DynamicJsonDocument doc(512);
  doc["team"] = config.teamName;
  int lf_ct = 0, arm_ct = 0;
  for (const String &d : connectedDevices) {
    if(d.startsWith("LF-")) lf_ct++;
    if(d.startsWith("Arm")) arm_ct++;
  }
  doc["lineFollowers"] = lf_ct;
  doc["roboticArms"] = arm_ct;
  
  JsonObject inv = doc.createNestedObject("inventory");
  inv["rackA"] = inventory.rackA;
  inv["rackB"] = inventory.rackB;
  inv["rackC"] = inventory.rackC;
  
  JsonArray devices = doc.createNestedArray("connectedDevices");
  for (const String &d : connectedDevices) {
    devices.add(d);
  }

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleRobotStatusUpdate() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  DEBUG_PRINTLN("[Server] Received robot status update");
  
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  
  String deviceName = "";
  if (doc.containsKey("robot")) deviceName = doc["robot"].as<String>();
  else if (doc.containsKey("device")) deviceName = doc["device"].as<String>();
  
  if (deviceName != "") {
    if (std::find(connectedDevices.begin(), connectedDevices.end(), deviceName) == connectedDevices.end()) {
      connectedDevices.push_back(deviceName);
    }
    deviceLastSeen[deviceName] = millis(); // Refresh Heartbeat
  }

  // Log received data for debugging
  DEBUG_PRINT("Robot: ");
  DEBUG_PRINTLN(doc["robot"].as<String>());
  DEBUG_PRINT("Location: ");
  DEBUG_PRINTLN(doc["location"].as<String>());
  
  // Send success response
  server.send(200, "application/json", "{\"status\":\"updated\"}");

  // Broadcast to WebSocket clients
  doc["type"] = "robot_status";
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
}

void handleCommand() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String command = server.arg("plain");
  String targetArm = "Arm-01";
  for (const String& dev : connectedDevices) {
    if (dev.startsWith("Arm")) {
      targetArm = dev;
      break;
    }
  }
  DEBUG_PRINTLN("Sending to arm (" + targetArm + "): " + command);
  notifyRobot(targetArm, command, "");
  server.send(200, "application/json", "{\"status\":\"command_sent\"}");
}

void handleDatabaseDownload() {
  if(!LittleFS.exists(DB_DIR)) {
    server.send(404, "text/plain", "Database not found");
    return;
  }
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Disposition", "attachment; filename=warehouse_db.zip");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/octet-stream", "");
  File dir = LittleFS.open(DB_DIR);
  File file = dir.openNextFile();
  while(file){
    if(!file.isDirectory()){
      server.sendContent(String(file.name()) + "\n");
    }
    file = dir.openNextFile();
  }
  server.client().stop();
}

void handleBoxPlacement() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  String robot = doc["robot"] | "Unknown";
  String rack = doc["rack"] | "Unknown";
  String box = doc["box"] | "Unknown";
  
  int pending = countPendingForRack(rack);
  if (getRackAvailableCapacity(rack) <= pending) {
    server.send(429, "application/json", "{\"error\":\"rack_full\"}");
    return;
  }

  if (rack == "RackA" && inventory.rackA >= inventory.rackACapacity) {
    server.send(400, "application/json", "{\"error\":\"RackA at full capacity\"}");
    return;
  }
  if (rack == "RackB" && inventory.rackB >= inventory.rackBCapacity) {
    server.send(400, "application/json", "{\"error\":\"RackB at full capacity\"}");
    return;
  }

  // Log the transaction
  String logEntry = String(millis()) + ",box_placed," + robot + "," + rack + "," + box + "\n";
  appendToFile(TRANSACTIONS_FILE, logEntry);

  completePlacement(robot, box, rack);
  server.send(200, "application/json", "{\"status\":\"confirmed\"}");
}

void serveWebInterface() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
      :root {
        --bg-dark: #0f172a;
        --bg-card: rgba(30, 41, 59, 0.7);
        --text-main: #f8fafc;
        --text-muted: #94a3b8;
        --accent: #38bdf8;
        --accent-hover: #0ea5e9;
        --success: #10b981;
        --danger: #ef4444;
        --border: rgba(255, 255, 255, 0.1);
      }
      body { 
        font-family: system-ui, -apple-system, sans-serif; 
        margin: 0; 
        padding: 0; 
        background-color: var(--bg-dark); 
        color: var(--text-main);
        min-height: 100vh;
      }
      header { 
        padding: 2rem; 
        text-align: center; 
        background: linear-gradient(to right, rgba(15, 23, 42, 0.9), rgba(30, 41, 59, 0.9));
        border-bottom: 1px solid var(--border);
      }
      h1, h2 { margin: 0 0 1rem 0; font-weight: 600; }
      h1 { background: -webkit-linear-gradient(45deg, #38bdf8, #818cf8); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
      nav { 
        display: flex; justify-content: center; gap: 1rem; padding: 1rem;
        background: var(--bg-card); backdrop-filter: blur(12px);
        border-bottom: 1px solid var(--border); position: sticky; top: 0; z-index: 100; flex-wrap: wrap;
      }
      nav a {
        color: var(--text-muted); text-decoration: none; padding: 0.75rem 1.5rem;
        border-radius: 8px; font-weight: 500; transition: all 0.3s ease; border: 1px solid transparent;
      }
      nav a:hover, nav a.active { 
        color: #fff; background: rgba(56, 189, 248, 0.1);
        border: 1px solid var(--accent); box-shadow: 0 0 15px rgba(56, 189, 248, 0.2);
      }
      section { padding: 2rem; max-width: 1200px; margin: 0 auto; }
      .card {
        background: var(--bg-card); backdrop-filter: blur(12px);
        border: 1px solid var(--border); border-radius: 16px; padding: 1.5rem;
        box-shadow: 0 10px 30px -10px rgba(0,0,0,0.5); transition: transform 0.3s ease;
      }
      .card:hover { transform: translateY(-5px); }
      
      .status-item { display: flex; justify-content: space-between; align-items: center; padding: 1rem 0; border-bottom: 1px solid var(--border); }
      .status-item:last-child { border-bottom: none; }
      .status-value { font-weight: bold; color: var(--accent); font-size: 1.2rem; }
      
      button {
        background: linear-gradient(135deg, var(--accent), var(--accent-hover)); border: none;
        padding: 0.75rem 1.5rem; color: white; font-weight: 600; border-radius: 8px; cursor: pointer;
        transition: all 0.3s ease; box-shadow: 0 4px 12px rgba(56, 189, 248, 0.3);
      }
      button:hover { transform: translateY(-2px); box-shadow: 0 6px 16px rgba(56, 189, 248, 0.5); }
      
      input {
        width: 100%; padding: 1rem; background: rgba(15, 23, 42, 0.5); border: 1px solid var(--border);
        border-radius: 8px; color: white; box-sizing: border-box; transition: all 0.3s ease;
      }
      input:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 2px rgba(56, 189, 248, 0.2); }
      
      .log { 
        font-family: 'Courier New', Courier, monospace; font-size: 0.9rem;
        white-space: pre-wrap; background: #000; color: #10b981; padding: 1rem;
        border-radius: 8px; max-height: 300px; overflow-y: auto; border: 1px solid #333;
      }
    </style>
  </head>
  <body>

    <header><h1>Warehouse Automation - %TEAM_NAME%</h1></header>
    <nav>
      <a href="/" class="active">Dashboard</a>
      <a href="/inventory">Inventory</a>
      <a href="/robot">Robot</a>
      <a href="/settings">Settings</a>
    </nav>
    <section>
      <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 2rem;">
        <div class="card">
          <h2>System Status</h2>
          <div class="status-item"><span>Connected Devices</span><span class="status-value" id="deviceCount">0</span></div>
          <div class="status-item"><span>Line Followers</span><span class="status-value">%LINE_FOLLOWERS%</span></div>
          <div class="status-item"><span>Robotic Arms</span><span class="status-value">%ROBOTIC_ARMS%</span></div>
          <div class="status-item"><span>Free Memory</span><span class="status-value" id="freeMemory">-</span></div>
        </div>
        <div class="card">
          <h2>Quick Inventory</h2>
          <div class="status-item"><span>Rack A</span><span class="status-value" id="rackA-status">%RACKA_COUNT%/%RACKA_CAPACITY%</span></div>
          <div class="status-item"><span>Rack B</span><span class="status-value" id="rackB-status">%RACKB_COUNT%/%RACKB_CAPACITY%</span></div>
          <div class="status-item"><span>Rack C</span><span class="status-value" id="rackC-status">%RACKC_COUNT%/%RACKC_CAPACITY%</span></div>
        </div>
      </div>
      
      <div class="card" style="margin-top: 2rem;">
        <h2>Fleet Telemetry</h2>
        <div id="fleetStatus" style="display: flex; flex-direction: column; gap: 0.5rem; margin-top: 1rem;">
          <div style="color: var(--text-muted); font-style: italic;">Awaiting telemetry...</div>
        </div>
      </div>

      <div class="card" style="margin-top: 2rem;">
        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem;">
          <h2 style="margin: 0;">Live Network Logs</h2>
          <button onclick="clearLogs()" style="border-radius: 20px; padding: 0.5rem 1rem;">Clear</button>
        </div>
        <div class="log" id="logOutput"></div>
      </div>
    </section>

  <script>
    let ws;
    let logOutput = document.getElementById('logOutput');

    let fleetData = {};

    function connectWebSocket() {
      ws = new WebSocket(`ws://${location.hostname}:81`);
      ws.onopen = () => log("Network connection established.");
      ws.onmessage = (e) => {
        try {
          const d = JSON.parse(e.data);
          if (d.type === 'rfid') log(">>> RFID Tag: " + d.tag);
          else if (d.type === 'inventory') { updateInventoryStatus(d); log(">>> Grid Inventory Sync."); }
          else if (d.type === 'location') log(">>> Localization Ping: " + d.tag);
          else if (d.type === 'arm') log(">>> Arm Event: " + JSON.stringify(d));
          else if (d.type === 'status') updateDeviceStatus(d);
          else if (d.type === 'robot_status') updateFleetUI(d);
        } catch (err) { log("RAW: " + e.data); }
      };
      ws.onclose = () => { log("Connection lost. Reconnecting..."); setTimeout(connectWebSocket, 3000); };
    }

    function log(msg) {
      logOutput.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\n`;
      logOutput.scrollTop = logOutput.scrollHeight;
    }
    function clearLogs() { logOutput.textContent = ''; }

    function updateInventoryStatus(data) {
      if(data.rackA !== undefined) document.getElementById('rackA-status').textContent = `${data.rackA}/%RACKA_CAPACITY%`;
      if(data.rackB !== undefined) document.getElementById('rackB-status').textContent = `${data.rackB}/%RACKB_CAPACITY%`;
      if(data.rackC !== undefined) document.getElementById('rackC-status').textContent = `${data.rackC}/%RACKC_CAPACITY%`;
    }

    function updateFleetUI(d) {
      let devName = d.robot || d.device || "Unknown Device";
      if (!fleetData[devName]) fleetData[devName] = {};
      
      if(d.battery !== undefined) fleetData[devName].battery = d.battery;
      if(d.error !== undefined) fleetData[devName].error = d.error;
      if(d.status !== undefined) fleetData[devName].status = d.status;
      fleetData[devName].lastSeen = new Date().toLocaleTimeString();

      let container = document.getElementById('fleetStatus');
      container.innerHTML = '';
      for (let [name, info] of Object.entries(fleetData)) {
          let errStatus = info.error || 'OK';
          let errColor = errStatus !== 'OK' ? 'var(--danger)' : 'var(--success)';
          let batt = info.battery !== undefined ? info.battery + '%' : 'N/A';
          let card = `<div style="display: flex; justify-content: space-between; background: rgba(0,0,0,0.2); padding: 1rem; border-radius: 8px; border-left: 4px solid ${errColor}">
              <strong style="color:var(--accent)">${name}</strong>
              <span style="color:var(--text-main)">⚡ ${batt}</span>
              <span style="color:${errColor}; font-weight:bold;">${errStatus}</span>
              <span style="color:var(--text-muted); font-size:0.8rem;">${info.lastSeen}</span>
          </div>`;
          container.innerHTML += card;
      }
    }

    function updateDeviceStatus(data) {
      if(data.connectedDevices) document.getElementById('deviceCount').textContent = data.connectedDevices.length;
      if(data.freeHeap) document.getElementById('freeMemory').textContent = Math.round(data.freeHeap/1024) + " KB";
    }

    function fetchStatus() {
      fetch('/api/status').then(res => res.json()).then(data => {
        updateDeviceStatus(data); updateInventoryStatus(data.inventory);
      }).catch(err => log("Status fetch error: " + err));
    }
    
    window.onload = () => { connectWebSocket(); fetchStatus(); setInterval(fetchStatus, 5000); };
  </script>
  </body>
  </html>
  )rawliteral";

  html.replace("%TEAM_NAME%", config.teamName);
  int lf_cnt = 0, arm_cnt = 0;
  for(auto &d : connectedDevices){
    if(d.startsWith("LF-")) lf_cnt++;
    if(d.startsWith("Arm")) arm_cnt++;
  }
  html.replace("%LINE_FOLLOWERS%", String(lf_cnt));
  html.replace("%ROBOTIC_ARMS%", String(arm_cnt));
  html.replace("%RACKA_COUNT%", String(inventory.rackA));
  html.replace("%RACKA_CAPACITY%", String(inventory.rackACapacity));
  html.replace("%RACKB_COUNT%", String(inventory.rackB));
  html.replace("%RACKB_CAPACITY%", String(inventory.rackBCapacity));
  html.replace("%RACKC_COUNT%", String(inventory.rackC));
  html.replace("%RACKC_CAPACITY%", String(inventory.rackCCapacity));

  // Calculate percentages
  int rackAPercent = (inventory.rackA * 100) / inventory.rackACapacity;
  int rackBPercent = (inventory.rackB * 100) / inventory.rackBCapacity;
  int rackCPercent = (inventory.rackC * 100) / inventory.rackCCapacity;
  
  html.replace("%RACKA_PERCENT%", String(rackAPercent));
  html.replace("%RACKB_PERCENT%", String(rackBPercent));
  html.replace("%RACKC_PERCENT%", String(rackCPercent));

  server.send(200, "text/html", html);
}

void serveRobotPage() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
      :root {
        --bg-dark: #0f172a;
        --bg-card: rgba(30, 41, 59, 0.7);
        --text-main: #f8fafc;
        --text-muted: #94a3b8;
        --accent: #38bdf8;
        --accent-hover: #0ea5e9;
        --success: #10b981;
        --danger: #ef4444;
        --border: rgba(255, 255, 255, 0.1);
      }
      body { 
        font-family: system-ui, -apple-system, sans-serif; 
        margin: 0; 
        padding: 0; 
        background-color: var(--bg-dark); 
        color: var(--text-main);
        min-height: 100vh;
      }
      header { 
        padding: 2rem; 
        text-align: center; 
        background: linear-gradient(to right, rgba(15, 23, 42, 0.9), rgba(30, 41, 59, 0.9));
        border-bottom: 1px solid var(--border);
      }
      h1, h2 { margin: 0 0 1rem 0; font-weight: 600; }
      h1 { background: -webkit-linear-gradient(45deg, #38bdf8, #818cf8); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
      nav { 
        display: flex; justify-content: center; gap: 1rem; padding: 1rem;
        background: var(--bg-card); backdrop-filter: blur(12px);
        border-bottom: 1px solid var(--border); position: sticky; top: 0; z-index: 100; flex-wrap: wrap;
      }
      nav a {
        color: var(--text-muted); text-decoration: none; padding: 0.75rem 1.5rem;
        border-radius: 8px; font-weight: 500; transition: all 0.3s ease; border: 1px solid transparent;
      }
      nav a:hover, nav a.active { 
        color: #fff; background: rgba(56, 189, 248, 0.1);
        border: 1px solid var(--accent); box-shadow: 0 0 15px rgba(56, 189, 248, 0.2);
      }
      section { padding: 2rem; max-width: 1200px; margin: 0 auto; }
      .card {
        background: var(--bg-card); backdrop-filter: blur(12px);
        border: 1px solid var(--border); border-radius: 16px; padding: 1.5rem;
        box-shadow: 0 10px 30px -10px rgba(0,0,0,0.5); transition: transform 0.3s ease;
      }
      .card:hover { transform: translateY(-5px); }
      
      .status-item { display: flex; justify-content: space-between; align-items: center; padding: 1rem 0; border-bottom: 1px solid var(--border); }
      .status-item:last-child { border-bottom: none; }
      .status-value { font-weight: bold; color: var(--accent); font-size: 1.2rem; }
      
      button {
        background: linear-gradient(135deg, var(--accent), var(--accent-hover)); border: none;
        padding: 0.75rem 1.5rem; color: white; font-weight: 600; border-radius: 8px; cursor: pointer;
        transition: all 0.3s ease; box-shadow: 0 4px 12px rgba(56, 189, 248, 0.3);
      }
      button:hover { transform: translateY(-2px); box-shadow: 0 6px 16px rgba(56, 189, 248, 0.5); }
      
      input {
        width: 100%; padding: 1rem; background: rgba(15, 23, 42, 0.5); border: 1px solid var(--border);
        border-radius: 8px; color: white; box-sizing: border-box; transition: all 0.3s ease;
      }
      input:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 2px rgba(56, 189, 248, 0.2); }
      
      .log { 
        font-family: 'Courier New', Courier, monospace; font-size: 0.9rem;
        white-space: pre-wrap; background: #000; color: #10b981; padding: 1rem;
        border-radius: 8px; max-height: 300px; overflow-y: auto; border: 1px solid #333;
      }
    </style>
  </head>
  <body>

    <header><h1>Robotic Arm Console</h1></header>
    <nav>
      <a href="/">Dashboard</a>
      <a href="/inventory">Inventory</a>
      <a href="/robot" class="active">Robot</a>
      <a href="/settings">Settings</a>
    </nav>
    <section>
      <div class="card" style="max-width: 600px; margin: 0 auto;">
        <h2>Command Matrix</h2>
        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; margin-bottom: 2rem;">
          <button onclick="sendCommand('pick')" style="padding: 1.5rem; font-size: 1.1rem;">Pick Item</button>
          <button onclick="sendCommand('place')" style="padding: 1.5rem; font-size: 1.1rem;">Place Item</button>
          <button onclick="sendCommand('home')" style="grid-column: span 2; padding: 1.5rem; background: linear-gradient(135deg, #8b5cf6, #d946ef);">Return Home</button>
        </div>
        
        <h2>Terminal Injection</h2>
        <div style="display: flex; gap: 1rem; margin-bottom: 2rem;">
          <input type="text" id="customCommand" placeholder="Enter remote instruction snippet...">
          <button onclick="sendCustomCommand()">Execute</button>
        </div>
        
        <h2>Arm Telemetry</h2>
        <div id="commandLog" class="log" style="height: 150px;"></div>
      </div>
    </section>

    <script>
      function sendCommand(cmd) {
        fetch('/api/command', { method: 'POST', body: cmd })
        .then(() => logMessage(`Tx -> ${cmd}`))
        .catch(err => logMessage(`ERR -> ${err}`));
      }

      function sendCustomCommand() {
        const i = document.getElementById('customCommand');
        if(i.value.trim()){ sendCommand(i.value); i.value = ''; }
      }

      function logMessage(msg) {
        const l = document.getElementById('commandLog');
        l.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\n`;
        l.scrollTop = l.scrollHeight;
      }

      const ws = new WebSocket(`ws://${location.hostname}:81`);
      ws.onmessage = (e) => {
        const d = JSON.parse(e.data);
        if(d.type === 'arm' || d.type === 'notification') logMessage(`Rx <- ${JSON.stringify(d)}`);
      };
    </script>
  </body>
  </html>
  )rawliteral";

    server.send(200, "text/html", html);
}

void serveSettingsPage() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
      :root {
        --bg-dark: #0f172a;
        --bg-card: rgba(30, 41, 59, 0.7);
        --text-main: #f8fafc;
        --text-muted: #94a3b8;
        --accent: #38bdf8;
        --accent-hover: #0ea5e9;
        --success: #10b981;
        --danger: #ef4444;
        --border: rgba(255, 255, 255, 0.1);
      }
      body { 
        font-family: system-ui, -apple-system, sans-serif; 
        margin: 0; 
        padding: 0; 
        background-color: var(--bg-dark); 
        color: var(--text-main);
        min-height: 100vh;
      }
      header { 
        padding: 2rem; 
        text-align: center; 
        background: linear-gradient(to right, rgba(15, 23, 42, 0.9), rgba(30, 41, 59, 0.9));
        border-bottom: 1px solid var(--border);
      }
      h1, h2 { margin: 0 0 1rem 0; font-weight: 600; }
      h1 { background: -webkit-linear-gradient(45deg, #38bdf8, #818cf8); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
      nav { 
        display: flex; justify-content: center; gap: 1rem; padding: 1rem;
        background: var(--bg-card); backdrop-filter: blur(12px);
        border-bottom: 1px solid var(--border); position: sticky; top: 0; z-index: 100; flex-wrap: wrap;
      }
      nav a {
        color: var(--text-muted); text-decoration: none; padding: 0.75rem 1.5rem;
        border-radius: 8px; font-weight: 500; transition: all 0.3s ease; border: 1px solid transparent;
      }
      nav a:hover, nav a.active { 
        color: #fff; background: rgba(56, 189, 248, 0.1);
        border: 1px solid var(--accent); box-shadow: 0 0 15px rgba(56, 189, 248, 0.2);
      }
      section { padding: 2rem; max-width: 1200px; margin: 0 auto; }
      .card {
        background: var(--bg-card); backdrop-filter: blur(12px);
        border: 1px solid var(--border); border-radius: 16px; padding: 1.5rem;
        box-shadow: 0 10px 30px -10px rgba(0,0,0,0.5); transition: transform 0.3s ease;
      }
      .card:hover { transform: translateY(-5px); }
      
      .status-item { display: flex; justify-content: space-between; align-items: center; padding: 1rem 0; border-bottom: 1px solid var(--border); }
      .status-item:last-child { border-bottom: none; }
      .status-value { font-weight: bold; color: var(--accent); font-size: 1.2rem; }
      
      button {
        background: linear-gradient(135deg, var(--accent), var(--accent-hover)); border: none;
        padding: 0.75rem 1.5rem; color: white; font-weight: 600; border-radius: 8px; cursor: pointer;
        transition: all 0.3s ease; box-shadow: 0 4px 12px rgba(56, 189, 248, 0.3);
      }
      button:hover { transform: translateY(-2px); box-shadow: 0 6px 16px rgba(56, 189, 248, 0.5); }
      
      input {
        width: 100%; padding: 1rem; background: rgba(15, 23, 42, 0.5); border: 1px solid var(--border);
        border-radius: 8px; color: white; box-sizing: border-box; transition: all 0.3s ease;
      }
      input:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 2px rgba(56, 189, 248, 0.2); }
      
      .log { 
        font-family: 'Courier New', Courier, monospace; font-size: 0.9rem;
        white-space: pre-wrap; background: #000; color: #10b981; padding: 1rem;
        border-radius: 8px; max-height: 300px; overflow-y: auto; border: 1px solid #333;
      }
    </style>
  </head>
  <body>

    <header><h1>System Parameters</h1></header>
    <nav>
      <a href="/">Dashboard</a>
      <a href="/inventory">Inventory</a>
      <a href="/robot">Robot</a>
      <a href="/settings" class="active">Settings</a>
    </nav>
    <section>
      <div class="card" style="max-width: 500px; margin: 0 auto;">
        <div style="margin-bottom: 1.5rem;">
          <label style="display: block; margin-bottom: 0.5rem; color: var(--text-muted);">Team Designation / Cluster ID</label>
          <input type="text" id="teamName" value="%TEAM_NAME%">
        </div>
        
        
        
        <button onclick="saveSettings()" style="width: 100%; margin-bottom: 1rem;">Sync Configuration</button>
        <button onclick="window.location.href='/update'" class="danger" style="width: 100%;">System OTA Update</button>
        
        <div id="statusMessage" style="margin-top: 1rem; padding: 1rem; border-radius: 8px; display: none; text-align: center; font-weight: bold;"></div>
      </div>
    </section>

    <script>
      function saveSettings() {
        const settings = {
          team: document.getElementById('teamName').value
        };
        fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(settings) })
        .then(() => showStatus('Parametric sync successful!', 'success'))
        .catch(err => showStatus('Sync failed: ' + err, 'error'));
      }

      function showStatus(msg, type) {
        const d = document.getElementById('statusMessage');
        d.textContent = msg; d.style.display = 'block';
        d.style.backgroundColor = type === 'success' ? 'rgba(16, 185, 129, 0.2)' : 'rgba(239, 68, 68, 0.2)';
        d.style.color = type === 'success' ? '#34d399' : '#f87171';
        d.style.border = type === 'success' ? '1px solid #059669' : '1px solid #dc2626';
        setTimeout(() => d.style.display = 'none', 3000);
      }
    </script>
  </body>
  </html>
  )rawliteral";

    // Replace placeholders with current config values
    html.replace("%TEAM_NAME%", config.teamName);
    int lf_cnt = 0, arm_cnt = 0;
    for(auto &d : connectedDevices){
      if(d.startsWith("LF-")) lf_cnt++;
      if(d.startsWith("Arm")) arm_cnt++;
    }
    html.replace("%LINE_FOLLOWERS%", String(lf_cnt));
    html.replace("%ROBOTIC_ARMS%", String(arm_cnt));

  server.send(200, "text/html", html);
}

void serveInventoryPage() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
      :root {
        --bg-dark: #0f172a;
        --bg-card: rgba(30, 41, 59, 0.7);
        --text-main: #f8fafc;
        --text-muted: #94a3b8;
        --accent: #38bdf8;
        --accent-hover: #0ea5e9;
        --success: #10b981;
        --danger: #ef4444;
        --border: rgba(255, 255, 255, 0.1);
      }
      body { 
        font-family: system-ui, -apple-system, sans-serif; 
        margin: 0; 
        padding: 0; 
        background-color: var(--bg-dark); 
        color: var(--text-main);
        min-height: 100vh;
      }
      header { 
        padding: 2rem; 
        text-align: center; 
        background: linear-gradient(to right, rgba(15, 23, 42, 0.9), rgba(30, 41, 59, 0.9));
        border-bottom: 1px solid var(--border);
      }
      h1, h2 { margin: 0 0 1rem 0; font-weight: 600; }
      h1 { background: -webkit-linear-gradient(45deg, #38bdf8, #818cf8); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
      nav { 
        display: flex; justify-content: center; gap: 1rem; padding: 1rem;
        background: var(--bg-card); backdrop-filter: blur(12px);
        border-bottom: 1px solid var(--border); position: sticky; top: 0; z-index: 100; flex-wrap: wrap;
      }
      nav a {
        color: var(--text-muted); text-decoration: none; padding: 0.75rem 1.5rem;
        border-radius: 8px; font-weight: 500; transition: all 0.3s ease; border: 1px solid transparent;
      }
      nav a:hover, nav a.active { 
        color: #fff; background: rgba(56, 189, 248, 0.1);
        border: 1px solid var(--accent); box-shadow: 0 0 15px rgba(56, 189, 248, 0.2);
      }
      section { padding: 2rem; max-width: 1200px; margin: 0 auto; }
      .card {
        background: var(--bg-card); backdrop-filter: blur(12px);
        border: 1px solid var(--border); border-radius: 16px; padding: 1.5rem;
        box-shadow: 0 10px 30px -10px rgba(0,0,0,0.5); transition: transform 0.3s ease;
      }
      .card:hover { transform: translateY(-5px); }
      
      .status-item { display: flex; justify-content: space-between; align-items: center; padding: 1rem 0; border-bottom: 1px solid var(--border); }
      .status-item:last-child { border-bottom: none; }
      .status-value { font-weight: bold; color: var(--accent); font-size: 1.2rem; }
      
      button {
        background: linear-gradient(135deg, var(--accent), var(--accent-hover)); border: none;
        padding: 0.75rem 1.5rem; color: white; font-weight: 600; border-radius: 8px; cursor: pointer;
        transition: all 0.3s ease; box-shadow: 0 4px 12px rgba(56, 189, 248, 0.3);
      }
      button:hover { transform: translateY(-2px); box-shadow: 0 6px 16px rgba(56, 189, 248, 0.5); }
      
      input {
        width: 100%; padding: 1rem; background: rgba(15, 23, 42, 0.5); border: 1px solid var(--border);
        border-radius: 8px; color: white; box-sizing: border-box; transition: all 0.3s ease;
      }
      input:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 2px rgba(56, 189, 248, 0.2); }
      
      .log { 
        font-family: 'Courier New', Courier, monospace; font-size: 0.9rem;
        white-space: pre-wrap; background: #000; color: #10b981; padding: 1rem;
        border-radius: 8px; max-height: 300px; overflow-y: auto; border: 1px solid #333;
      }
    </style>
  </head>
  <body>

    <header><h1>Visual Storage Manifest</h1></header>
    <nav>
      <a href="/">Dashboard</a>
      <a href="/inventory" class="active">Inventory</a>
      <a href="/robot">Robot</a>
      <a href="/settings">Settings</a>
    </nav>
    <section>
      <div style="display: flex; flex-direction: column; gap: 2rem; max-width: 800px; margin: 0 auto;">
        
        <div class="card">
          <div style="display: flex; justify-content: space-between; align-items: baseline;">
            <h2>Sector Alpha</h2>
            <div style="color: var(--text-muted);"><span id="rackA-count" style="color: var(--text-main); font-size: 1.5rem; font-weight: bold;">%RACKA_COUNT%</span> / %RACKA_CAPACITY%</div>
          </div>
          <div style="height: 24px; background: rgba(0,0,0,0.5); border-radius: 12px; margin-top: 1rem; overflow: hidden; border: 1px solid var(--border); box-shadow: inset 0 2px 5px rgba(0,0,0,0.5);">
            <div id="rackA-bar" style="height: 100%; width: %RACKA_PERCENT%%; background: linear-gradient(90deg, #10b981, #3b82f6); transition: width 0.8s cubic-bezier(0.4, 0, 0.2, 1); border-radius: 12px; box-shadow: 0 0 10px rgba(59, 130, 246, 0.8);"></div>
          </div>
        </div>

        <div class="card">
          <div style="display: flex; justify-content: space-between; align-items: baseline;">
            <h2>Sector Bravo</h2>
            <div style="color: var(--text-muted);"><span id="rackB-count" style="color: var(--text-main); font-size: 1.5rem; font-weight: bold;">%RACKB_COUNT%</span> / %RACKB_CAPACITY%</div>
          </div>
          <div style="height: 24px; background: rgba(0,0,0,0.5); border-radius: 12px; margin-top: 1rem; overflow: hidden; border: 1px solid var(--border); box-shadow: inset 0 2px 5px rgba(0,0,0,0.5);">
            <div id="rackB-bar" style="height: 100%; width: %RACKB_PERCENT%%; background: linear-gradient(90deg, #8b5cf6, #d946ef); transition: width 0.8s cubic-bezier(0.4, 0, 0.2, 1); border-radius: 12px; box-shadow: 0 0 10px rgba(217, 70, 239, 0.8);"></div>
          </div>
        </div>

        <div class="card">
          <div style="display: flex; justify-content: space-between; align-items: baseline;">
            <h2>Sector Charlie</h2>
            <div style="color: var(--text-muted);"><span id="rackC-count" style="color: var(--text-main); font-size: 1.5rem; font-weight: bold;">%RACKC_COUNT%</span> / %RACKC_CAPACITY%</div>
          </div>
          <div style="height: 24px; background: rgba(0,0,0,0.5); border-radius: 12px; margin-top: 1rem; overflow: hidden; border: 1px solid var(--border); box-shadow: inset 0 2px 5px rgba(0,0,0,0.5);">
            <div id="rackC-bar" style="height: 100%; width: %RACKC_PERCENT%%; background: linear-gradient(90deg, #f59e0b, #ef4444); transition: width 0.8s cubic-bezier(0.4, 0, 0.2, 1); border-radius: 12px; box-shadow: 0 0 10px rgba(239, 68, 68, 0.8);"></div>
          </div>
        </div>
        
      </div>
    </section>

    <script>
      const ws = new WebSocket(`ws://${location.hostname}:81`);
      ws.onmessage = (e) => {
        const d = JSON.parse(e.data);
        if(d.type === 'inventory') {
          if(d.rackA !== undefined) {
             document.getElementById('rackA-count').textContent = d.rackA;
             document.getElementById('rackA-bar').style.width = Math.min(100, (d.rackA/%RACKA_CAPACITY%)*100) + '%';
          }
          if(d.rackB !== undefined) {
             document.getElementById('rackB-count').textContent = d.rackB;
             document.getElementById('rackB-bar').style.width = Math.min(100, (d.rackB/%RACKB_CAPACITY%)*100) + '%';
          }
          if(d.rackC !== undefined) {
             document.getElementById('rackC-count').textContent = d.rackC;
             document.getElementById('rackC-bar').style.width = Math.min(100, (d.rackC/%RACKC_CAPACITY%)*100) + '%';
          }
        }
      };
    </script>
  </body>
  </html>
  )rawliteral";

      // Replace placeholders with actual values
      html.replace("%RACKA_COUNT%", String(inventory.rackA));
      html.replace("%RACKA_CAPACITY%", String(inventory.rackACapacity));
      html.replace("%RACKA_PERCENT%", 
        String((inventory.rackA * 100) / inventory.rackACapacity));
      
      // Similar replacements for rackB and rackC

      server.send(200, "text/html", html);
}

void updateInventory(String rack, int change) {
  if (rack == "RackA") {
    inventory.rackA += change;
    if (inventory.rackA < 0) inventory.rackA = 0;
  } 
  else if (rack == "RackB") {
    inventory.rackB += change;
    if (inventory.rackB < 0) inventory.rackB = 0;
  } 
  else if (rack == "RackC") {
    inventory.rackC += change;
    if (inventory.rackC < 0) inventory.rackC = 0;
  }
  
  saveInventory(); // Save the updated inventory to flash

  // Publish inventory snapshot to ROS2 /warehouse/server/inventory
  publishInventorySnapshot();

  // Broadcast update to all WebSocket clients (existing dashboard)
  DynamicJsonDocument doc(256);
  doc["type"] = "inventory";
  doc["rackA"] = inventory.rackA;
  doc["rackB"] = inventory.rackB;
  doc["rackC"] = inventory.rackC;
  
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
}

void notifyRobot(String robotId, String message, String boxTag) {
  DynamicJsonDocument doc(256);
  doc["type"] = "notification";
  doc["robot"] = robotId;
  doc["message"] = message;
  doc["box"] = boxTag;
  
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
  
  // Queue command for robots polling via HTTP
  DynamicJsonDocument cmdDoc(128);
  cmdDoc["command"] = message;
  if(boxTag != "") cmdDoc["box_tag"] = boxTag;
  String cmdJson;
  serializeJson(cmdDoc, cmdJson);
  commandQueue[robotId] = cmdJson;
}

void completePlacement(String robotId, String boxTag, String rack) {
  // Update inventory
  updateInventory(rack, 1);
  
  // Notify robot
  notifyRobot(robotId, "placement_success", boxTag);
}

// ── micro-ROS server publisher init ─────────────────────────────────────────
void initMicroROSServer() {
  // Use WiFi already connected by WiFiManager
  // micro-ROS WiFi transport points at RPi5 agent
  set_microros_wifi_transports(
    (char*)WiFi.SSID().c_str(),
    (char*)WiFi.psk().c_str(),
    (char*)MICROROS_AGENT_IP,
    MICROROS_AGENT_PORT
  );
  delay(500);

  mros_allocator = rcl_get_default_allocator();
  if (rclc_support_init(&mros_support, 0, NULL, &mros_allocator) != RCL_RET_OK) {
    DEBUG_PRINTLN("[mROS] support init failed");
    return;
  }
  if (rclc_node_init_default(&mros_node, "server_s3", "warehouse", &mros_support) != RCL_RET_OK) {
    DEBUG_PRINTLN("[mROS] node init failed");
    return;
  }

  rclc_publisher_init_default(&pub_rfid,
    &mros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/warehouse/server/rfid");

  rclc_publisher_init_default(&pub_inventory,
    &mros_node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/warehouse/server/inventory");

  msg_rfid_out.data.capacity = 64;
  msg_rfid_out.data.data = rfidBuf;
  msg_rfid_out.data.size = 0;

  msg_inv_out.data.capacity = 128;
  msg_inv_out.data.data = invBuf;
  msg_inv_out.data.size = 0;

  // Executor with 0 subscriptions (server only publishes)
  rclc_executor_init(&mros_executor, &mros_support.context, 1, &mros_allocator);

  mrosReady = true;
  DEBUG_PRINTLN("[mROS] Server micro-ROS ready. Publishing to /warehouse/server/rfid and /warehouse/server/inventory");
}

void publishRFIDEvent(const char* tag) {
  if (!mrosReady) return;
  strncpy(rfidBuf, tag, sizeof(rfidBuf));
  msg_rfid_out.data.size = strlen(tag);
  RCSOFTCHECK_SVR(rcl_publish(&pub_rfid, &msg_rfid_out, NULL));
}

void publishInventorySnapshot() {
  if (!mrosReady) return;
  // Serialise as compact JSON: {"rackA":N,"rackB":N,"rackC":N}
  snprintf(invBuf, sizeof(invBuf),
    "{\"rackA\":%d,\"rackB\":%d,\"rackC\":%d}",
    inventory.rackA, inventory.rackB, inventory.rackC);
  msg_inv_out.data.size = strlen(invBuf);
  RCSOFTCHECK_SVR(rcl_publish(&pub_inventory, &msg_inv_out, NULL));
}
// ────────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  // BUG FIX: Removed while(!Serial) — hangs forever on battery-powered boot
  //          without USB. Use a short timeout instead.
  unsigned long _t0 = millis();
  while(!Serial && millis() - _t0 < 3000);
  DEBUG_PRINTLN("\n\nStarting Warehouse System Debug");

  
  SPI.begin();
  rfid.PCD_Init();
  initializeStorage();

  WiFiManager wifiManager;
  DEBUG_PRINTLN("Starting WiFiManager AP / Connecting...");
  if (!wifiManager.autoConnect("WarehouseConfig_Server")) {
    DEBUG_PRINTLN("Failed to connect and hit timeout. Restarting...");
    delay(3000);
    ESP.restart();
  }

  DEBUG_PRINTLN("Connected to Wi-Fi successfully!");
  DEBUG_PRINT("IP Address: ");
  DEBUG_PRINTLN(WiFi.localIP());

  if (!MDNS.begin("warehouse")) {
    DEBUG_PRINTLN("Error setting up MDNS responder!");
  } else {
    DEBUG_PRINTLN("mDNS responder started at http://warehouse.local");
  }

  server.on("/", HTTP_GET, serveWebInterface);
  server.on("/inventory", HTTP_GET, serveInventoryPage);
  server.on("/robot", HTTP_GET, serveRobotPage);
  server.on("/settings", HTTP_GET, serveSettingsPage);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/status", HTTP_POST, handleRobotStatusUpdate);
  server.on("/api/command", HTTP_POST, handleCommand);
  server.on("/api/database", HTTP_GET, handleDatabaseDownload);
  server.on("/api/settings", HTTP_POST, handleSettingsUpdate);
  server.on("/api/box_placed", HTTP_POST, handleBoxPlacement);
  
  server.on("/api/register", HTTP_GET, []() {
    if (server.hasArg("robot")) {
      String id = server.arg("robot");
      if (std::find(connectedDevices.begin(), connectedDevices.end(), id) == connectedDevices.end()) {
        connectedDevices.push_back(id);
      }
      deviceLastSeen[id] = millis();
      server.send(200, "application/json", "{\"status\":\"registered\"}");
    } else {
      server.send(400, "application/json", "{\"error\":\"missing_robot_id\"}");
    }
  });

  server.onNotFound([]() {
    if (server.uri().startsWith("/api/commands/")) {
      String id = server.uri().substring(14); // len("/api/commands/") == 14
      if (commandQueue.find(id) != commandQueue.end() && commandQueue[id] != "") {
        String cmd = commandQueue[id];
        commandQueue[id] = ""; // clear after reading
        server.send(200, "application/json", cmd);
      } else {
        server.send(200, "application/json", "{}"); // Empty command object
      }
      return;
    }
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  ElegantOTA.begin(&server);

  webSocket.begin();
  webSocket.onEvent(handleWebSocket);

  // Initialise micro-ROS after WiFi is connected
  // BUG FIX: original had while(!Serial) which hangs forever on USB-less boot.
  // Moved micro-ROS init here — after WiFiManager connection is confirmed.
  initMicroROSServer();
}

void loop() {
  server.handleClient();
  ElegantOTA.loop();
  webSocket.loop();

  // Spin micro-ROS executor (non-blocking, minimal time budget)
  if (mrosReady) {
    rclc_executor_spin_some(&mros_executor, RCL_MS_TO_NS(2));
  }

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    processRFID();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  

  static unsigned long lastSave = 0;
  if (millis() - lastSave > 30000) {
    saveDatabase();
    
    // Purge zombie devices missing for >30s
    unsigned long now = millis();
    for (auto it = deviceLastSeen.begin(); it != deviceLastSeen.end(); ) {
      if (now - it->second > 30000) {
        it = deviceLastSeen.erase(it);
      } else {
        ++it;
      }
    }

    connectedDevices.erase(std::remove_if(connectedDevices.begin(), connectedDevices.end(), [](const String& dev) {
      return deviceLastSeen.find(dev) == deviceLastSeen.end();
    }), connectedDevices.end());
    
    lastSave = millis();
  }
}

