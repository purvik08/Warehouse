#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include <WebServer.h>
#include <ElegantOTA.h>

WebServer otaServer(80);
bool otaStarted = false;
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define DEBUG 1

#if DEBUG
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif

#define WDT_TIMEOUT 10 // 10 seconds task watchdog
#define BATTERY_PIN 34 // Analog pin for battery voltage reading

// Server Connection Config
const String armID = "Arm-01";
String serverIP = "";
int httpFailures = 0;

// Servo Configuration
Servo baseServo;
Servo shoulderServo;
Servo elbowServo;
Servo wristServo;
Servo gripperServo;

const int basePin = 15;
const int shoulderPin = 2;
const int elbowPin = 4;
const int wristPin = 16;
const int gripperPin = 17;

String armStatus = "IDLE";
bool hasBox = false;
unsigned long lastCommandCheck = 0;

void setup() {
  Serial.begin(115200);

  // Initialize Watchdog Timer
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
  
  // Initialize ESP32 PWM timers
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  
  // Attach servos
  baseServo.setPeriodHertz(50);
  baseServo.attach(basePin);
  
  shoulderServo.setPeriodHertz(50);
  shoulderServo.attach(shoulderPin);
  
  elbowServo.setPeriodHertz(50);
  elbowServo.attach(elbowPin);
  
  wristServo.setPeriodHertz(50);
  wristServo.attach(wristPin);
  
  gripperServo.setPeriodHertz(50);
  gripperServo.attach(gripperPin);

  // Home position
  goHome();
  
  // Wi-Fi Configuration
  connectNetwork();
  resolveServer();
  
  if(!otaStarted) { otaServer.begin(); ElegantOTA.begin(&otaServer); otaStarted = true; }
  sendStatus();
}

void connectNetwork() {
  WiFiManager wifiManager;
  DEBUG_PRINTLN("Starting AP Config / Connecting to WiFi...");
  if (!wifiManager.autoConnect("WarehouseConfig_Arm01")) {
    DEBUG_PRINTLN("Failed to connect and hit timeout. Restarting...");
    delay(3000);
    ESP.restart();
  }
  DEBUG_PRINTLN("\nConnected to WiFi!");
}

void resolveServer() {
  DEBUG_PRINT("Resolving warehouse.local...");
  IPAddress resolvedIP;
  int retries = 0;
  
  if (!MDNS.begin(armID.c_str())) {
      DEBUG_PRINTLN("Error setting up mDNS");
  }
  
  while(serverIP == "" && retries < 15) {
    esp_task_wdt_reset(); // Feed watchdog during blocking server resolution
    resolvedIP = MDNS.queryHost("warehouse");
    if(resolvedIP.toString() != "0.0.0.0") {
       serverIP = resolvedIP.toString();
       DEBUG_PRINTLN(" Resolved: " + serverIP);
    } else {
       delay(1000);
       DEBUG_PRINT(".");
       retries++;
    }
  }
  
  if(serverIP == "") {
      DEBUG_PRINTLN("\nFailed to resolve server IP via mDNS. Using fallback dummy.");
      serverIP = "192.168.4.1"; // Fallback
  }
}

void loop() {
  esp_task_wdt_reset(); // Feed the watchdog
  otaServer.handleClient();
  ElegantOTA.loop();

  if (WiFi.status() != WL_CONNECTED) {
    connectNetwork();
  
    if(!otaStarted) { otaServer.begin(); ElegantOTA.begin(&otaServer); otaStarted = true; }
  }

  // Poll for commands like MobileRobot
  if (millis() - lastCommandCheck > 1000) {
    checkForCommands();
    lastCommandCheck = millis();
  }
  delay(10);
}

void checkForCommands() {
  if (WiFi.status() == WL_CONNECTED && serverIP != "") {
    HTTPClient http;
    String url = "http://" + serverIP + "/api/commands/" + armID;
    http.begin(url);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      httpFailures = 0; // Reset failures on success
      String payload = http.getString();
      if(payload.length() > 0) {
         processCommand(payload);
      }
    } else {
      httpFailures++;
      if (httpFailures > 5) {
         DEBUG_PRINTLN("Excessive HTTP failures. Re-resolving Server IP...");
         serverIP = ""; // Force re-resolve
         resolveServer();
         httpFailures = 0;
      }
    }
    http.end();
  }
}

void processCommand(String command) {
  // Try to parse as JSON first (legacy backend pattern)
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, command);
  
  String cmd = command;
  if (!error && doc.containsKey("command")) {
    cmd = doc["command"].as<String>();
  }
  cmd.trim();
  
  DEBUG_PRINT("Executing command: ");
  DEBUG_PRINTLN(cmd);
  
  if (cmd == "pick") {
    pickBox();
  } else if (cmd == "place") {
    placeBox();
  } else if (cmd == "home") {
    goHome();
  } else if (cmd == "status") {
    sendStatus();
  } else {
    DEBUG_PRINTLN("UNKNOWN_COMMAND");
  }
}

void notifyServerEvent(String eventName) {
  if (WiFi.status() == WL_CONNECTED && serverIP != "") {
    HTTPClient http;
    String url = "http://" + serverIP + "/api/status"; // general status endpoint or relay
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<128> doc;
    doc["device"] = armID;
    doc["event"] = eventName;
    doc["status"] = armStatus;
    doc["hasBox"] = hasBox;
    
    // Read and append battery percentage
    int rawVal = analogRead(BATTERY_PIN);
    int battPercent = map(rawVal, 0, 4095, 0, 100);
    doc["battery"] = constrain(battPercent, 0, 100);
    
    String payload;
    serializeJson(doc, payload);
    
    http.POST(payload);
    http.end();
  }
}

void pickBox() {
  armStatus = "PICKING";
  sendStatus();
  
  // Pick sequence
  moveToPosition(90, 45, 135, 90); // Approach
  openGripper();
  delay(500);
  moveToPosition(90, 60, 120, 90); // Grab
  closeGripper();
  delay(500);
  
  // Verify pickup
  hasBox = true;
  
  // Return home
  goHome();
  armStatus = "HOLDING";
  sendStatus();
  notifyServerEvent("pickup_complete");
}

void placeBox() {
  if (!hasBox) {
    DEBUG_PRINTLN("ERROR: NO_BOX");
    return;
  }
  
  armStatus = "PLACING";
  sendStatus();
  
  // Place sequence
  moveToPosition(90, 45, 135, 90); // Approach
  openGripper();
  delay(500);
  
  // Return home
  goHome();
  hasBox = false;
  armStatus = "IDLE";
  sendStatus();
  notifyServerEvent("place_complete");
}

void goHome() {
  moveToPosition(90, 90, 90, 90);
  armStatus = "IDLE";
}

void moveToPosition(int basePos, int shoulderPos, int elbowPos, int wristPos) {
  baseServo.write(basePos);
  shoulderServo.write(shoulderPos);
  elbowServo.write(elbowPos);
  wristServo.write(wristPos);
  delay(1000); // Wait for movement
}

void openGripper() {
  gripperServo.write(180);
}

void closeGripper() {
  gripperServo.write(0);
}

void sendStatus() {
  notifyServerEvent("status_update");
}