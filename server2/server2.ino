
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "FS.h"
#include "SD_MMC.h"
#include <MFRC522.h>
#include <SPI.h>
#include <vector>
#include <algorithm> // for std::find

#define SS_PIN 5
#define RST_PIN 0
MFRC522 rfid(SS_PIN, RST_PIN);

#define SERIAL_RX 13
#define SERIAL_TX 12
HardwareSerial NanoSerial(1);

#define DB_DIR "/db"
#define CONFIG_FILE DB_DIR "/config.json"
#define INVENTORY_FILE DB_DIR "/inventory.json"
#define ROBOTS_FILE DB_DIR "/robots.json"
#define TRANSACTIONS_FILE DB_DIR "/transactions.csv"
#define RFID_LOGS_FILE DB_DIR "/rfid_logs.csv"

#define BOX_TAG_PREFIX "BOX"
#define LOCATION_TAG_PREFIX "LOC"
#define TAG_LENGTH 10 // 3 chars prefix + 7 chars ID

std::vector<String> connectedDevices;

struct {
  String teamName = "Team Lakshya";
  int lineFollowers = 3;
  int roboticArms = 1;
} config;

struct Inventory {
  int rackA = 0;
  int rackB = 0;
  int rackC = 0;
  const int rackACapacity = 500;
  const int rackBCapacity = 600;
  const int rackCCapacity = 700;
} inventory;

struct PendingPlacement {
  String robotId;
  String boxTag;
  String rack;
  unsigned long timestamp;
};

std::vector<PendingPlacement> pendingPlacements;

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
  int count = 0;
  for (const auto& placement : pendingPlacements) {
    if (placement.rack == rack) {
      count++;
    }
  }
  return count;
}

int getRackAvailableCapacity(String rack) {
  if (rack == "RackA") return inventory.rackACapacity - inventory.rackA;
  if (rack == "RackB") return inventory.rackBCapacity - inventory.rackB;
  if (rack == "RackC") return inventory.rackCCapacity - inventory.rackC;
  return 0;
}

void initializeSD() {
  Serial.println("Initializing SD card...");

  if (!SD_MMC.begin()) {
    Serial.println("SD Card Mount Failed");
    Serial.println("Check: ");
    Serial.println("1. Is card inserted?");
    Serial.println("2. Is it formatted as FAT32?");
    Serial.println("3. Are pins properly connected?");
    return;
  }

  Serial.println("SD Card mounted successfully");

  uint8_t cardType = SD_MMC.cardType();

  if(cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }
  
  Serial.print("SD Card Type: ");

  if(cardType == CARD_MMC) Serial.println("MMC");
  else if(cardType == CARD_SD) Serial.println("SDSC");
  else if(cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("UNKNOWN");

  if (!SD_MMC.exists(DB_DIR)) SD_MMC.mkdir(DB_DIR);
  if (!SD_MMC.exists(CONFIG_FILE)) saveConfig(); else loadConfig();
  if (!SD_MMC.exists(INVENTORY_FILE)) saveInventory(); else loadInventory();
  if (!SD_MMC.exists(TRANSACTIONS_FILE)) appendToFile(TRANSACTIONS_FILE, "timestamp,event,robot,location,box\n");
  if (!SD_MMC.exists(RFID_LOGS_FILE)) appendToFile(RFID_LOGS_FILE, "timestamp,tag,location\n");
}

void saveDatabase() {
  saveConfig();
  saveInventory();
}

void saveConfig() {
  File file = SD_MMC.open(CONFIG_FILE, FILE_WRITE);
  if (!file) return;
  DynamicJsonDocument doc(256);
  doc["team"] = config.teamName;
  doc["lineFollowers"] = config.lineFollowers;
  doc["roboticArms"] = config.roboticArms;
  serializeJson(doc, file);
  file.close();
}

void loadConfig() {
  File file = SD_MMC.open(CONFIG_FILE, FILE_READ);
  if (!file) return;
  DynamicJsonDocument doc(256);
  deserializeJson(doc, file);
  config.teamName = doc["team"] | "Team Lakshya";
  config.lineFollowers = doc["lineFollowers"] | 3;
  config.roboticArms = doc["roboticArms"] | 1;
  file.close();
}

void saveInventory() {
  File file = SD_MMC.open(INVENTORY_FILE, FILE_WRITE);
  if (!file) return;
  DynamicJsonDocument doc(256);
  doc["rackA"] = inventory.rackA;
  doc["rackB"] = inventory.rackB;
  doc["rackC"] = inventory.rackC;
  serializeJson(doc, file);
  file.close();
}

void loadInventory() {
  File file = SD_MMC.open(INVENTORY_FILE, FILE_READ);
  if (!file) return;
  DynamicJsonDocument doc(256);
  deserializeJson(doc, file);
  inventory.rackA = doc["rackA"] | 0;
  inventory.rackB = doc["rackB"] | 0;
  inventory.rackC = doc["rackC"] | 0;
  file.close();
}

void appendToFile(String filename, String data) {
  File file = SD_MMC.open(filename, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open file: " + filename);
    return;
  }
  if (!file.print(data)) {
    Serial.println("Write failed: " + filename);
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

void processArmMessage(String message) {
  DynamicJsonDocument doc(128);
  DeserializationError error = deserializeJson(doc, message);
  if (error) return;

  if (doc.containsKey("device")) {
  String deviceName = doc["device"];
  if (std::find(connectedDevices.begin(), connectedDevices.end(), deviceName) == connectedDevices.end()) {
    connectedDevices.push_back(deviceName);
    }
  }

  if (doc.containsKey("event")) {
    String event = doc["event"];
    String logEntry = String(millis()) + "," + event + ",ARM,,";
    if (doc.containsKey("box")) logEntry += doc["box"].as<String>();
    logEntry += "\n";
    appendToFile(TRANSACTIONS_FILE, logEntry);

    doc["type"] = "arm";
    String json;
    serializeJson(doc, json);
    webSocket.broadcastTXT(json);
  }
}

void handleWebSocket(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  // Debug header
  Serial.printf("[WebSocket][Client %u] Event: ", num);
  
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("Disconnected");
      // Remove from connected devices list
      connectedDevices.erase(std::remove_if(connectedDevices.begin(), 
                                          connectedDevices.end(),
                                          [num](const String& dev) {
                                            return dev.startsWith("Client"+String(num));
                                          }),
                          connectedDevices.end());
      break;

    case WStype_CONNECTED: {
      Serial.println("Connected");
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
      Serial.println("Text Message Received");
      // Sandbox message processing
      if (length > 512) {  // Prevent overly large messages
        Serial.println("Message too large, rejecting");
        webSocket.sendTXT(num, "{\"error\":\"message_too_large\"}");
        break;
      }

      String message = (char*)payload;
      Serial.println("Raw message: " + message);

      // Safe JSON parsing
      DynamicJsonDocument doc(512);
      DeserializationError error = deserializeJson(doc, message);
      
      if (error) {
        Serial.print("JSON parse error: ");
        Serial.println(error.c_str());
        webSocket.sendTXT(num, "{\"error\":\"invalid_json\"}");
        break;
      }

      // Message type validation
      if (!doc.containsKey("type")) {
        Serial.println("Missing message type");
        webSocket.sendTXT(num, "{\"error\":\"missing_type\"}");
        break;
      }

      String msgType = doc["type"].as<String>();
      Serial.println("Processing message type: " + msgType);

      // Process different message types
      if (msgType == "command") {
        // Validate command structure
        if (!doc.containsKey("command")) {
          webSocket.sendTXT(num, "{\"error\":\"missing_command\"}");
          break;
        }
        
        String command = doc["command"].as<String>();
        Serial.println("Executing command: " + command);
        
        // Send to robotic arm with validation
        if (command == "pick" || command == "place" || command == "home") {
          NanoSerial.println(command);
          webSocket.sendTXT(num, "{\"status\":\"command_sent\"}");
        } else {
          webSocket.sendTXT(num, "{\"error\":\"invalid_command\"}");
        }
      }
      // Add other message types here...
      break;
    }

    case WStype_BIN:
      Serial.printf("Binary message length: %u\n", length);
      // Echo back binary data for testing
      webSocket.sendBIN(num, payload, length);
      break;

    case WStype_ERROR:
      Serial.printf("Error: %u\n", *payload);
      break;

    case WStype_PING:
      Serial.println("Ping received");
      break;

    case WStype_PONG:
      Serial.println("Pong received");
      break;

    default:
      Serial.printf("Unhandled event type: %u\n", type);
      break;
  }

  // Debug footer
  Serial.printf("[WebSocket][Client %u] Free Heap: %u bytes\n", 
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
  if (doc.containsKey("lineFollowers")) config.lineFollowers = doc["lineFollowers"];
  if (doc.containsKey("roboticArms")) config.roboticArms = doc["roboticArms"];

  // Save to SD card
  saveConfig();

  server.send(200, "application/json", "{\"status\":\"success\"}");
}

void handleStatus() {
  DynamicJsonDocument doc(512);
  doc["team"] = config.teamName;
  doc["lineFollowers"] = config.lineFollowers;
  doc["roboticArms"] = config.roboticArms;
  
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

  Serial.println("[Server] Received robot status update");
  
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  
  if (error) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  // Log received data for debugging
  Serial.print("Robot: ");
  Serial.println(doc["robot"].as<String>());
  Serial.print("Location: ");
  Serial.println(doc["location"].as<String>());
  
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
  Serial.println("Sending to arm: " + command);
  NanoSerial.println(command);
  server.send(200, "application/json", "{\"status\":\"command_sent\"}");
}

void handleDatabaseDownload() {
  if(!SD_MMC.exists(DB_DIR)) {
    server.send(404, "text/plain", "Database not found");
    return;
  }
  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Content-Disposition", "attachment; filename=warehouse_db.zip");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/octet-stream", "");
  File dir = SD_MMC.open(DB_DIR);
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

  PendingPlacement p {
    doc["robot"].as<String>(),
    doc["box"].as<String>(),
    doc["rack"].as<String>(),
    millis()
  };
  pendingPlacements.push_back(p);
  server.send(202, "application/json", "{\"status\":\"processing\"}");
  
  // Broadcast update to all clients
  DynamicJsonDocument update(256);
  update["type"] = "inventory";
  update["rackA"] = inventory.rackA;
  update["rackB"] = inventory.rackB;
  update["rackC"] = inventory.rackC;
  
  String json;
  serializeJson(update, json);
  webSocket.broadcastTXT(json);
}

void serveWebInterface() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <title>Warehouse Automation</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
      body { 
        font-family: Arial; 
        margin: 0; 
        padding: 0; 
        background: #f5f5f5; 
      }
      header { 
        background: #333; 
        color: #fff; 
        padding: 1em; 
        text-align: center; 
      }
      nav { 
        display: flex; 
        background: #444; 
        flex-wrap: wrap;
      }
      nav a {
        flex: 1;
        padding: 1em;
        background: #444;
        color: white;
        text-align: center;
        text-decoration: none;
        min-width: 120px;
      }
      nav a:hover, nav a.active { 
        background: #666; 
      }
      section { 
        padding: 1em; 
      }
      .dashboard-grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
        gap: 1em;
      }
      .card {
        background: white;
        border-radius: 5px;
        padding: 1em;
        box-shadow: 0 2px 5px rgba(0,0,0,0.1);
      }
      .log { 
        font-family: monospace; 
        white-space: pre-wrap; 
        background: #eee; 
        padding: 0.5em;
        max-height: 300px;
        overflow-y: auto;
      }
      .status-item {
        margin-bottom: 0.5em;
      }
      .status-value {
        font-weight: bold;
      }
    </style>
  </head>
  <body>
    <header>
      <h1>Warehouse Automation - %TEAM_NAME%</h1>
    </header>
    <nav>
      <a href="/" class="active">Dashboard</a>
      <a href="/inventory">Inventory</a>
      <a href="/robot">Robot</a>
      <a href="/settings">Settings</a>
    </nav>

    <section>
      <div class="dashboard-grid">
        <div class="card">
          <h2>System Status</h2>
          <div class="status-item">
            <span>Connected Devices: </span>
            <span class="status-value" id="deviceCount">0</span>
          </div>
          <div class="status-item">
            <span>Line Followers: </span>
            <span class="status-value">%LINE_FOLLOWERS%</span>
          </div>
          <div class="status-item">
            <span>Robotic Arms: </span>
            <span class="status-value">%ROBOTIC_ARMS%</span>
          </div>
          <div class="status-item">
            <span>Free Memory: </span>
            <span class="status-value" id="freeMemory">-</span>
          </div>
        </div>

        <div class="card">
          <h2>Quick Inventory</h2>
          <div class="status-item">
            <span>Rack A: </span>
            <span class="status-value" id="rackA-status">%RACKA_COUNT%/%RACKA_CAPACITY%</span>
          </div>
          <div class="status-item">
            <span>Rack B: </span>
            <span class="status-value" id="rackB-status">%RACKB_COUNT%/%RACKB_CAPACITY%</span>
          </div>
          <div class="status-item">
            <span>Rack C: </span>
            <span class="status-value" id="rackC-status">%RACKC_COUNT%/%RACKC_CAPACITY%</span>
          </div>
        </div>
      </div>

      <div class="card" style="margin-top: 1em;">
        <h2>Live Logs</h2>
        <div class="log" id="logOutput"></div>
        <button onclick="clearLogs()" style="margin-top: 0.5em;">Clear Logs</button>
      </div>
    </section>

  <script>
    let ws;
    let logOutput = document.getElementById('logOutput');

    // Initialize WebSocket connection
    function connectWebSocket() {
      ws = new WebSocket(`ws://${location.hostname}:81`);
      
      ws.onopen = () => {
        log("Connected to WebSocket server");
      };
      
      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          if (data.type === 'rfid') {
            log("RFID Tag: " + data.tag);
          } else if (data.type === 'inventory') {
            updateInventoryStatus(data);
            log("Inventory updated");
          } else if (data.type === 'location') {
            log("Location Tag: " + data.tag);
          } else if (data.type === 'arm') {
            log("Arm Event: " + JSON.stringify(data));
          } else if (data.type === 'status') {
            updateDeviceStatus(data);
          }
        } catch (e) {
          log("Received: " + event.data);
        }
      };
      
      ws.onclose = () => {
        log("WebSocket disconnected. Reconnecting...");
        setTimeout(connectWebSocket, 3000);
      };
      
      ws.onerror = (error) => {
        log("WebSocket error: " + error);
      };
    }

    function log(message) {
      const timestamp = new Date().toLocaleTimeString();
      logOutput.textContent += `[${timestamp}] ${message}\n`;
      logOutput.scrollTop = logOutput.scrollHeight;
    }

    function clearLogs() {
      logOutput.textContent = '';
    }

    function updateInventoryStatus(data) {
      if (data.rackA !== undefined) {
        document.getElementById('rackA-status').textContent = 
          `${data.rackA}/%RACKA_CAPACITY%`;
      }
      if (data.rackB !== undefined) {
        document.getElementById('rackB-status').textContent = 
          `${data.rackB}/%RACKB_CAPACITY%`;
      }
      if (data.rackC !== undefined) {
        document.getElementById('rackC-status').textContent = 
          `${data.rackC}/%RACKC_CAPACITY%`;
      }
    }

    function updateDeviceStatus(data) {
      if (data.connectedDevices) {
        document.getElementById('deviceCount').textContent = 
          data.connectedDevices.length;
      }
      if (data.freeHeap) {
        document.getElementById('freeMemory').textContent = 
          Math.round(data.freeHeap/1024) + " KB";
      }
    }

    // Periodically fetch system status
    function fetchStatus() {
      fetch('/api/status')
        .then(res => res.json())
        .then(data => {
          updateDeviceStatus(data);
          updateInventoryStatus(data.inventory);
        })
        .catch(err => log("Status fetch error: " + err));
    }

    // Highlight current page in navigation
    function setActiveNav() {
      const path = window.location.pathname;
      const navLinks = document.querySelectorAll('nav a');
      navLinks.forEach(link => {
        link.classList.remove('active');
        if (link.getAttribute('href') === path) {
          link.classList.add('active');
        }
      });
    }

    // Initialize
    window.onload = () => {
      connectWebSocket();
      setActiveNav();
      fetchStatus();
      setInterval(fetchStatus, 5000);
    };
  </script>
  </body>
  </html>
  )rawliteral";

  html.replace("%TEAM_NAME%", config.teamName);
  html.replace("%LINE_FOLLOWERS%", String(config.lineFollowers));
  html.replace("%ROBOTIC_ARMS%", String(config.roboticArms));
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
    <title>Robot Control</title>
    <style>
      body { font-family: Arial; margin: 20px; }
      .control-panel { 
        border: 1px solid #ddd;
        padding: 20px;
        border-radius: 5px;
        max-width: 500px;
      }
      button {
        padding: 10px 15px;
        margin: 5px;
        background-color: #4CAF50;
        color: white;
        border: none;
        border-radius: 4px;
        cursor: pointer;
      }
      button:hover { background-color: #45a049; }
      #commandLog {
        margin-top: 20px;
        border: 1px solid #ddd;
        padding: 10px;
        height: 200px;
        overflow-y: scroll;
      }
    </style>
  </head>
  <body>
    <h1>Robotic Arm Control</h1>
    
    <div class="control-panel">
      <h2>Quick Commands</h2>
      <button onclick="sendCommand('pick')">Pick Item</button>
      <button onclick="sendCommand('place')">Place Item</button>
      <button onclick="sendCommand('home')">Return Home</button>
      
      <h2>Custom Command</h2>
      <input type="text" id="customCommand" placeholder="Enter command">
      <button onclick="sendCustomCommand()">Send</button>
      
      <div id="commandLog"></div>
    </div>

    <script>
      function sendCommand(cmd) {
        fetch('/api/command', {
          method: 'POST',
          body: cmd
        })
        .then(response => {
          logMessage(`Command sent: ${cmd}`);
        })
        .catch(error => {
          logMessage(`Error: ${error}`);
        });
      }

      function sendCustomCommand() {
        const cmd = document.getElementById('customCommand').value;
        if(cmd.trim() !== '') {
          sendCommand(cmd);
          document.getElementById('customCommand').value = '';
        }
      }

      function logMessage(msg) {
        const log = document.getElementById('commandLog');
        const entry = document.createElement('div');
        entry.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
        log.appendChild(entry);
        log.scrollTop = log.scrollHeight;
      }

      // WebSocket for receiving arm status updates
      const ws = new WebSocket(`ws://${location.hostname}:81`);
      
      ws.onmessage = (event) => {
        const data = JSON.parse(event.data);
        if(data.type === 'arm') {
          logMessage(`Arm status: ${JSON.stringify(data)}`);
        }
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
    <title>System Settings</title>
    <style>
      body { font-family: Arial; margin: 20px; }
      .settings-form { 
        border: 1px solid #ddd;
        padding: 20px;
        border-radius: 5px;
        max-width: 500px;
      }
      .form-group {
        margin-bottom: 15px;
      }
      label {
        display: block;
        margin-bottom: 5px;
        font-weight: bold;
      }
      input, select {
        width: 100%;
        padding: 8px;
        box-sizing: border-box;
      }
      button {
        padding: 10px 15px;
        background-color: #4CAF50;
        color: white;
        border: none;
        border-radius: 4px;
        cursor: pointer;
      }
      button:hover { background-color: #45a049; }
      #statusMessage {
        margin-top: 15px;
        padding: 10px;
        border-radius: 4px;
        display: none;
      }
    </style>
  </head>
  <body>
    <h1>System Configuration</h1>
    
    <div class="settings-form">
      <div class="form-group">
        <label for="teamName">Team Name:</label>
        <input type="text" id="teamName" value="%TEAM_NAME%">
      </div>
      
      <div class="form-group">
        <label for="lineFollowers">Number of Line Followers:</label>
        <input type="number" id="lineFollowers" min="1" max="10" value="%LINE_FOLLOWERS%">
      </div>
      
      <div class="form-group">
        <label for="roboticArms">Number of Robotic Arms:</label>
        <input type="number" id="roboticArms" min="1" max="5" value="%ROBOTIC_ARMS%">
      </div>
      
      <button onclick="saveSettings()">Save Settings</button>
      
      <div id="statusMessage"></div>
    </div>

    <script>
      function saveSettings() {
        const settings = {
          team: document.getElementById('teamName').value,
          lineFollowers: parseInt(document.getElementById('lineFollowers').value),
          roboticArms: parseInt(document.getElementById('roboticArms').value)
        };

        fetch('/api/settings', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify(settings)
        })
        .then(response => response.json())
        .then(data => {
          showStatus('Settings saved successfully!', 'success');
        })
        .catch(error => {
          showStatus('Error saving settings: ' + error, 'error');
        });
      }

      function showStatus(message, type) {
        const statusDiv = document.getElementById('statusMessage');
        statusDiv.textContent = message;
        statusDiv.style.display = 'block';
        statusDiv.style.backgroundColor = type === 'success' ? '#dff0d8' : '#f2dede';
        statusDiv.style.color = type === 'success' ? '#3c763d' : '#a94442';
        
        setTimeout(() => {
          statusDiv.style.display = 'none';
        }, 3000);
      }
    </script>
  </body>
  </html>
  )rawliteral";

    // Replace placeholders with current config values
    html.replace("%TEAM_NAME%", config.teamName);
    html.replace("%LINE_FOLLOWERS%", String(config.lineFollowers));
    html.replace("%ROBOTIC_ARMS%", String(config.roboticArms));

  server.send(200, "text/html", html);
}

void serveInventoryPage() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>Inventory Management</title>
      <style>
        body { font-family: Arial; margin: 20px; }
        .rack { 
          border: 1px solid #ddd; 
          padding: 15px; 
          margin-bottom: 15px;
          border-radius: 5px;
        }
        .capacity-bar {
          height: 20px;
          background-color: #f0f0f0;
          border-radius: 3px;
          margin-top: 5px;
        }
        .fill {
          height: 100%;
          border-radius: 3px;
          background-color: #4CAF50;
        }
      </style>
    </head>
    <body>
      <h1>Current Inventory Status</h1>
      
      <div class="rack">
        <h2>Rack A</h2>
        <p>Items: <span id="rackA-count">%RACKA_COUNT%</span>/%RACKA_CAPACITY%</p>
        <div class="capacity-bar">
          <div class="fill" id="rackA-bar" style="width: %RACKA_PERCENT%%"></div>
        </div>
      </div>

      <div class="rack">
        <h2>Rack B</h2>
        <p>Items: <span id="rackB-count">%RACKB_COUNT%</span>/%RACKB_CAPACITY%</p>
        <div class="capacity-bar">
          <div class="fill" id="rackB-bar" style="width: %RACKB_PERCENT%%"></div>
        </div>
      </div>

      <div class="rack">
        <h2>Rack C</h2>
        <p>Items: <span id="rackC-count">%RACKC_COUNT%</span>/%RACKC_CAPACITY%</p>
        <div class="capacity-bar">
          <div class="fill" id="rackC-bar" style="width: %RACKC_PERCENT%%"></div>
        </div>
      </div>

      <script>
        // WebSocket for real-time updates
        const ws = new WebSocket(`ws://${location.hostname}:81`);
        
        ws.onmessage = (event) => {
          const data = JSON.parse(event.data);
          if(data.type === 'inventory') {
            updateInventory(data);
          }
        };

        function updateInventory(data) {
          if(data.rackA !== undefined) {
            document.getElementById('rackA-count').textContent = data.rackA;
            document.getElementById('rackA-bar').style.width = 
              Math.min(100, (data.rackA/%RACKA_CAPACITY%)*100) + '%';
          }
          // Similar updates for rackB and rackC
        }
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
  
  saveInventory(); // Save the updated inventory to SD card

  // Broadcast update to all clients
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
  
  // Send to WebSocket (assuming robots are connected via WebSocket)
  webSocket.broadcastTXT(json);
  
  // Alternatively, could send via serial if using physical connections
  // NanoSerial.println(json);
}

void completePlacement(String robotId, String boxTag, String rack) {
  // Remove from pending
  pendingPlacements.erase(
    std::remove_if(pendingPlacements.begin(), pendingPlacements.end(),
      [&](const PendingPlacement& p) {
        return p.robotId == robotId && p.boxTag == boxTag && p.rack == rack;
      }),
    pendingPlacements.end()
  );
  
  // Update inventory
  updateInventory(rack, 1);
  
  // Notify robot
  notifyRobot(robotId, "placement_success", boxTag);
}

void setup() {
  Serial.begin(115200);
  while(!Serial); // Wait for serial port to connect (for USB debugging)
  Serial.println("\n\nStarting Warehouse System Debug");

  NanoSerial.begin(9600, SERIAL_8N1, SERIAL_RX, SERIAL_TX);
  SPI.begin();
  rfid.PCD_Init();
  initializeSD();

  WiFi.softAP("WarehouseAP", "password123");
  Serial.println("AP Starting...");
  delay(100); // Short delay for AP to initialize

  if(!WiFi.softAPIP()) {
    Serial.println("AP Failed to Start!");
    while(1); // Halt if AP fails
  }

  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("MAC Address: ");
  Serial.println(WiFi.softAPmacAddress());
  Serial.println("AP IP: " + WiFi.softAPIP().toString());

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
  server.begin();

  webSocket.begin();
  webSocket.onEvent(handleWebSocket);

  if (!SD_MMC.begin()) 
  {
  Serial.println("SD Card Mount Failed");
  return; // Handle failure properly
  }
}

void loop() {
  server.handleClient();
  webSocket.loop();

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    processRFID();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  if (NanoSerial.available()) {
    processArmMessage(NanoSerial.readStringUntil('\n'));
  }

  static unsigned long lastSave = 0;
  if (millis() - lastSave > 30000) {
    saveDatabase();
    lastSave = millis();
  }
}

