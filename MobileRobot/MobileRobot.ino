#include <WiFi.h>
#include <WebServer.h>
#include <ElegantOTA.h>

WebServer otaServer(80);
bool otaStarted = false;
#include <HTTPClient.h>
#include <MFRC522.h>
#include <NewPing.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Preferences.h>
Preferences preferences;
#define BATTERY_PIN 36 // True ADC voltage pin

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

#define BOX_PREFIX "BOX"  // Define box tag prefix

// Hardware Configuration
#define SS_PIN 5
#define RST_PIN 0
MFRC522 rfid(SS_PIN, RST_PIN);

#define BOX_TAG_PREFIX "BOX"
#define LOCATION_TAG_PREFIX "LOC"
#define TAG_LENGTH 10 // 3 chars prefix + 7 chars ID

// Ultrasonic Sensor
#define TRIGGER_PIN 14
#define ECHO_PIN 15
#define MAX_DISTANCE 200 // cm
NewPing sonar(TRIGGER_PIN, ECHO_PIN, MAX_DISTANCE);

// IR sensors
const int irLeft = 34;
const int irRight = 35;

// Motor control
const int motorLeftF = 12;
const int motorLeftB = 13;
const int motorRightF = 16;
const int motorRightB = 17;

// LED and Buzzer
const int ledRed = 25;
const int ledGreen = 26;
const int ledBlue = 27;
const int buzzer = 32;

// Server Connection Config
const char* ssid = "WarehouseAP";
const char* password = "password123";
const String robotID = "LF-01";
const String serverIP = "192.168.4.1";

// Navigation Constants
const unsigned long rfidCooldown = 3000;
const unsigned long statusUpdateInterval = 10000;
const unsigned long destinationTimeout = 300000; // 5 minutes timeout

// RFID Tags (Initialize empty, will be learned)
String HOME_TAG = "195 142 194 228"; //C3 8E C2 E4
String PICKUP_TAG = "99 184 168 228"; //63 B8 A8 E4
String RACK1_TAG = "131 72 28 229";  //83 48 1C E5
String RACK2_TAG = "86 136 17 3"; //56 88 11 03

String readRFIDTag() {
  String tag = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    tag += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    tag += String(rfid.uid.uidByte[i], HEX);
  }
  tag.toUpperCase();
  return tag;
}

// Navigation Variables
String currentLocation = "Unknown";
String destination = "";
String currentBoxTag = "";  // <-- Declared globally
bool hasBox = false;
bool isLearningMode = false;
unsigned long lastRFIDScan = 0;
unsigned long lastStatusUpdate = 0;
unsigned long destinationStartTime = 0;

enum PlacementState {
  PLACEMENT_IDLE,
  PLACEMENT_IN_PROGRESS,
  PLACEMENT_CONFIRMED,
  PLACEMENT_FAILED
};

PlacementState currentPlacement = PLACEMENT_IDLE;
unsigned long placementStartTime;
const unsigned long PLACEMENT_TIMEOUT = 30000; // 30 seconds


float checkBatteryLevel() {
  int raw = analogRead(BATTERY_PIN);
  return (raw / 4095.0) * 3.3 * 2.0; // Read true voltage
}

bool checkObstacle() {
  unsigned int distance = sonar.ping_cm();
  if (distance == 0) distance = MAX_DISTANCE;
  
  if (distance < 30) {
    DEBUG_PRINT("Obstacle detected: ");
    DEBUG_PRINT(distance);
    DEBUG_PRINTLN("cm");
    return true;
  }
  return false;
}

bool sendPlacementRequest() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + serverIP + "/api/placement_request";
    http.begin(url);
    
    DynamicJsonDocument doc(256);
    doc["robot"] = robotID;
    doc["location"] = currentLocation;
    doc["box"] = currentBoxTag;
    
    String payload;
    serializeJson(doc, payload);
    
    int httpCode = http.POST(payload);
    http.end();
    return (httpCode == HTTP_CODE_OK);
  }
  return false;
}

bool isValidBoxTag(String tag) {
  if (!tag.startsWith(BOX_TAG_PREFIX)) return false;
  if (tag.length() != TAG_LENGTH) return false;
  
  // Check that remaining characters are alphanumeric
  for (unsigned int i = 3; i < tag.length(); i++) {
    char c = tag.charAt(i);
    if (!isAlphaNumeric(c)) return false;
  }
  return true;
}

bool isValidLocationTag(String tag) {
  if (!tag.startsWith(LOCATION_TAG_PREFIX)) return false;
  if (tag.length() != TAG_LENGTH) return false;
  
  // Check that remaining characters are alphanumeric
  for (unsigned int i = 3; i < tag.length(); i++) {
    char c = tag.charAt(i);
    if (!isAlphaNumeric(c)) return false;
  }
  return true;
}

bool sendPlacementAttempt(String rackLocation, String boxTag) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + serverIP + "/api/box_placed";
    http.begin(url);
    
    DynamicJsonDocument doc(256);
    doc["robot"] = robotID;
    doc["rack"] = rackLocation;
    doc["box"] = boxTag;
    
    String payload;
    serializeJson(doc, payload);
    
    int httpCode = http.POST(payload);
    http.end();
    return (httpCode == HTTP_CODE_OK);
  }
  return false;
}

bool confirmPlacement(String location, String boxTag) {
  int retries = 3;
  while(retries-- > 0) {
    if(sendPlacementAttempt(location, boxTag)) {
      return true;
    }
    delay(1000);
  }
  return false;
}

bool shouldPlaceBox() {
  // Check if we're at a rack location and have a box
  return (currentLocation == "Rack1" || currentLocation == "Rack2") && 
         hasBox && 
         currentBoxTag != "" &&
         currentPlacement == PLACEMENT_IDLE;
}

void initializeIndicators() {
  pinMode(ledRed, OUTPUT);
  pinMode(ledGreen, OUTPUT);
  pinMode(ledBlue, OUTPUT);
  pinMode(buzzer, OUTPUT);
  setLED(0, 0, 255); // Blue for initialization
}

void sendBoxPlacementConfirmation(String rackLocation, String boxTag) {
  int retries = 3;
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + serverIP + "/api/box_placed";
    http.begin(url);
    
    while (retries-- > 0) {
    if (sendPlacementAttempt(rackLocation, boxTag)) {
      DEBUG_PRINTLN("Box placement confirmed");
      http.end();
      return;
    }
    DEBUG_PRINTLN("Retrying placement confirmation...");
    delay(1000);
  }

    DynamicJsonDocument doc(256);
    doc["robot"] = robotID;
    doc["rack"] = rackLocation;
    doc["box"] = boxTag;
    doc["timestamp"] = millis();
    
    String payload;
    serializeJson(doc, payload);
    
    int httpCode = http.POST(payload);
    if (httpCode == HTTP_CODE_OK) {
      DEBUG_PRINTLN("Box placement confirmed: " + payload);
      beep(200); beep(200); // Confirmation beeps
    } else {
      DEBUG_PRINT("Box placement update failed. Error: ");
      DEBUG_PRINTLN(http.errorToString(httpCode));
      indicateError();
    }
    http.end();
  }
}

void initializeMotors() {
  pinMode(motorLeftF, OUTPUT);
  pinMode(motorLeftB, OUTPUT);
  pinMode(motorRightF, OUTPUT);
  pinMode(motorRightB, OUTPUT);
  stopMotors();
}

void initializeSensors() {
  pinMode(irLeft, INPUT);
  pinMode(irRight, INPUT);
}

void initializeRFID() {
  SPI.begin();
  rfid.PCD_Init();
  delay(4);
}

void setLED(int r, int g, int b) {
  analogWrite(ledRed, r);
  analogWrite(ledGreen, g);
  analogWrite(ledBlue, b);
}

void indicateReady() {
  setLED(0, 255, 0); // Green
  beep(200);
}

void indicateError() {
  setLED(255, 0, 0); // Red
  for(int i=0; i<3; i++) {
    beep(100);
    delay(100);
  }
}

void indicateArrival() {
  setLED(0, 255, 255); // Cyan
  beep(500);
  delay(1000);
  indicateReady();
}

void beep(int duration) {
  digitalWrite(buzzer, HIGH);
  delay(duration);
  digitalWrite(buzzer, LOW);
}

void startLearningMode() {
  isLearningMode = true;
  setLED(255, 255, 0); // Yellow
  DEBUG_PRINTLN("Entering learning mode...");
  DEBUG_PRINTLN("Scan tags in this order: Home, Pickup, Rack1, Rack2");
  beep(1000);
}

void handleLearningMode() {
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String tag = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      tag += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
      tag += String(rfid.uid.uidByte[i], HEX);
    }
    tag.toUpperCase();
    
    if(HOME_TAG == "") {
      HOME_TAG = tag;
      DEBUG_PRINTLN("Home tag learned: " + tag);
      beep(200);
    } else if(PICKUP_TAG == "") {
      PICKUP_TAG = tag;
      DEBUG_PRINTLN("Pickup tag learned: " + tag);
      beep(200);
    } else if(RACK1_TAG == "") {
      RACK1_TAG = tag;
      DEBUG_PRINTLN("Rack1 tag learned: " + tag);
      beep(200);
    } else if(RACK2_TAG == "") {
      RACK2_TAG = tag;
      DEBUG_PRINTLN("Rack2 tag learned: " + tag);
      saveLearnedTags();
      isLearningMode = false;
      DEBUG_PRINTLN("All tags learned!");
      indicateReady();
      registerRobot();
    }
    rfid.PICC_HaltA();
  }
}

void handleLocationArrival(String location) {
  if (destination == location) {
    destinationReached();
    
    // Let loop() evaluate handleBoxPlacement() 
    // when it cycles and reads "shouldPlaceBox()" -> true
  }
}

void saveLearnedTags() {
  preferences.begin("rfid", false);
  preferences.putString("home", HOME_TAG);
  preferences.putString("pickup", PICKUP_TAG);
  preferences.putString("rack1", RACK1_TAG);
  preferences.putString("rack2", RACK2_TAG);
  preferences.end();
  DEBUG_PRINTLN("Tags genuinely saved to ESP32 Flash");
}

void loadLearnedTags() {
  preferences.begin("rfid", true);
  HOME_TAG = preferences.getString("home", "");
  PICKUP_TAG = preferences.getString("pickup", "");
  RACK1_TAG = preferences.getString("rack1", "");
  RACK2_TAG = preferences.getString("rack2", "");
  preferences.end();
  DEBUG_PRINTLN("Tags retrieved from flash memory");
}

/* Movement Functions */
void stopMotors() {
  digitalWrite(motorLeftF, LOW);
  digitalWrite(motorLeftB, LOW);
  digitalWrite(motorRightF, LOW);
  digitalWrite(motorRightB, LOW);
}

void moveForward() {
  digitalWrite(motorLeftF, HIGH);
  digitalWrite(motorLeftB, LOW);
  digitalWrite(motorRightF, HIGH);
  digitalWrite(motorRightB, LOW);
}

void turnLeft() {
  digitalWrite(motorLeftF, LOW);
  digitalWrite(motorLeftB, HIGH);
  digitalWrite(motorRightF, HIGH);
  digitalWrite(motorRightB, LOW);
  delay(200);
  stopMotors();
}

void turnRight() {
  digitalWrite(motorLeftF, HIGH);
  digitalWrite(motorLeftB, LOW);
  digitalWrite(motorRightF, LOW);
  digitalWrite(motorRightB, HIGH);
  delay(200);
  stopMotors();
}

void emergencyStop() {
  stopMotors();
  indicateError();
  DEBUG_PRINTLN("EMERGENCY STOP");
  while (true) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(200);
  }
  indicateReady();
}

void checkDestinationTimeout() {
  if(destination != "" && millis() - destinationStartTime > destinationTimeout) {
    DEBUG_PRINTLN("Timeout reached for destination: " + destination);
    indicateError();
    destination = "";
    stopMotors();
  }
}

void handleObstacle() {
  static unsigned long lastObstacleTime = 0;
  static int attemptCount = 0;
  
  if(millis() - lastObstacleTime > 1000) {
    attemptCount = 0;
  }
  
  if(attemptCount++ < 3) {
    DEBUG_PRINTLN("Obstacle detected - attempting avoidance");
    setLED(255, 165, 0); // Orange
    stopMotors();
    delay(500);
    turnRight();
    delay(500);
  } else {
    emergencyStop();
    attemptCount = 0;
  }
  
  lastObstacleTime = millis();
}

void checkRFID() {
  if (millis() - lastRFIDScan > rfidCooldown) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String tag = readRFIDTag();
      processRFIDTag(tag);
      rfid.PICC_HaltA();
      lastRFIDScan = millis();
    }
  }
}

void processRFIDTag(String tag) {
  DEBUG_PRINT("RFID Scanned: ");
  DEBUG_PRINTLN(tag);

  // Update current location
  if (tag == HOME_TAG) {
    currentLocation = "Home";
    handleLocationArrival("Home");
  } else if (tag == PICKUP_TAG) {
    currentLocation = "Pickup";
    handleLocationArrival("Pickup");

    // Robot has arrived at pickup. Awaiting command pipeline from central server to allocate box code.


  } else if (tag == RACK1_TAG) {
    currentLocation = "Rack1";
    handleLocationArrival("Rack1");
  } else if (tag == RACK2_TAG) {
    currentLocation = "Rack2";
    handleLocationArrival("Rack2");
  } else {
    DEBUG_PRINTLN("Unknown tag scanned: " + tag);
  }
}

void destinationReached() {
  stopMotors();
  indicateArrival();
  DEBUG_PRINTLN("Reached destination: " + destination);
  destination = "";
  destinationStartTime = 0;
}

void connectToServerAP() {
  WiFi.begin(ssid, password);
  DEBUG_PRINT("Connecting to server AP");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(500);
    DEBUG_PRINT(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_PRINTLN("\nConnected to server AP");
    DEBUG_PRINT("IP Address: ");
    DEBUG_PRINTLN(WiFi.localIP());
  } else {
    DEBUG_PRINTLN("\nFailed to connect to server AP");
    emergencyStop();
  }
}

void maintainConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN("Connection lost - reconnecting");
    WiFi.reconnect();
    delay(1000);
    
    if (WiFi.status() != WL_CONNECTED) {
      emergencyStop();
    } else {
      if(!otaStarted) { otaServer.begin(); ElegantOTA.begin(&otaServer); otaStarted = true; }
    }
  }
}

void registerRobot() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + serverIP + "/api/register?robot=" + robotID;
    http.begin(url);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      DEBUG_PRINTLN("Successfully registered with server");
    } else {
      DEBUG_PRINT("Registration failed. Error: ");
      DEBUG_PRINTLN(http.errorToString(httpCode));
    }
    http.end();
  }
}

void updateServerStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN("WiFi not connected, cannot update status");
    return;
  }

  HTTPClient http;
  String url = "http://" + serverIP + "/api/status";
  http.begin(url);
  http.addHeader("Content-Type", "application/json"); // Required header
  
  DynamicJsonDocument doc(256);
  doc["robot"] = robotID;
  doc["location"] = currentLocation;
  doc["hasBox"] = hasBox;
  doc["destination"] = destination;
  doc["battery"] = checkBatteryLevel();
  
  String payload;
  serializeJson(doc, payload);
  
  DEBUG_PRINT("[Robot] Sending status update: ");
  DEBUG_PRINTLN(payload);
  
  int httpCode = http.POST(payload);
  
  if (httpCode == HTTP_CODE_OK) {
    DEBUG_PRINTLN("Status updated successfully");
  } else {
    DEBUG_PRINT("Status update failed. HTTP Code: ");
    DEBUG_PRINTLN(httpCode);
    DEBUG_PRINT("Error: ");
    DEBUG_PRINTLN(http.errorToString(httpCode));
    
    // Print server response if available
    String response = http.getString();
    if (response.length() > 0) {
      DEBUG_PRINT("Server response: ");
      DEBUG_PRINTLN(response);
    }
  }
  http.end();
}

void checkForCommands() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + serverIP + "/api/commands/" + robotID;
    http.begin(url);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      processCommand(payload);
    }
    http.end();
  }
}

void processCommand(String payload) {
  String command = payload;
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if(!error && doc.containsKey("command")) {
    command = doc["command"].as<String>();
    if(doc.containsKey("box_tag")) currentBoxTag = doc["box_tag"].as<String>();
  }
  command.trim();
  DEBUG_PRINT("Executing remote payload: ");
  DEBUG_PRINTLN(command);
  
  if (command == "STOP") {
    emergencyStop();
  } 
  else if (command == "HOME") {
    setDestination("Home");
  }
  else if (command == "PICKUP") {
    setDestination("Pickup");
  }
  else if (command == "RACK1") {
    setDestination("Rack1");
  }
  else if (command == "RACK2") {
    setDestination("Rack2");
  }
  else if (command == "PICK_BOX") {
    if (currentLocation == "Pickup") {
      if(currentBoxTag == "") currentBoxTag = "BOX-" + String(millis()); // Fallback tracking ID
      hasBox = true;
      DEBUG_PRINTLN("Box technically secured: " + currentBoxTag);
      updateServerStatus();
      beep(300);
    } else {
      DEBUG_PRINTLN("Cannot execute pick - physical mismatch!");
      indicateError();
    }
  }
}

void setDestination(String newDestination) {
  destination = newDestination;
  destinationStartTime = millis();
  DEBUG_PRINTLN("New destination set: " + destination);
}

void handleBoxPlacement() {
  if (currentPlacement == PLACEMENT_IN_PROGRESS) {
    if (millis() - placementStartTime > PLACEMENT_TIMEOUT) {
      currentPlacement = PLACEMENT_FAILED;
      indicateError();
    }
    return;
  }

  if (shouldPlaceBox()) {
    currentPlacement = PLACEMENT_IN_PROGRESS;
    if (confirmPlacement(currentLocation, currentBoxTag)) {
      onPlacementConfirmed();
      currentPlacement = PLACEMENT_IDLE; // Reset state
    } else {
      onPlacementFailed();
    }
  }
}

void followLine() {
  int leftIR = digitalRead(irLeft);
  int rightIR = digitalRead(irRight);
  
  if (leftIR == LOW && rightIR == LOW) {
    moveForward();
  } else if (leftIR == HIGH) {
    turnRight();
  } else if (rightIR == HIGH) {
    turnLeft();
  }
}

void indicateSuccess() {
  setLED(0, 255, 0);  // Green
  beep(100);
  delay(100);
  beep(100);
  delay(100);
  beep(100);
}

void indicateFailure() {
  setLED(255, 0, 0);  // Red
  for(int i = 0; i < 3; i++) {
    beep(300);
    delay(200);
  }
}

void attemptRecovery() {
  setLED(255, 165, 0);  // Orange
  DEBUG_PRINTLN("Attempting recovery from failed placement");
  
  // Back up slightly
  digitalWrite(motorLeftF, LOW);
  digitalWrite(motorLeftB, HIGH);
  digitalWrite(motorRightF, LOW);
  digitalWrite(motorRightB, HIGH);
  delay(500);
  stopMotors();
  
  // Reset placement state
  currentPlacement = PLACEMENT_IDLE;
  indicateReady();
}

void onPlacementConfirmed() {
  currentPlacement = PLACEMENT_CONFIRMED;
  hasBox = false;
  currentBoxTag = "";
  indicateSuccess();  // Now properly defined
  DEBUG_PRINTLN("Box placement confirmed!");
}

void onPlacementFailed() {
  currentPlacement = PLACEMENT_FAILED;
  indicateFailure();  // Now properly defined
  attemptRecovery();  // Now properly defined
  DEBUG_PRINTLN("Box placement failed!");
}

void debugWiFiConnection() {
  DEBUG_PRINTLN("\n--- WiFi Debug Info ---");
  DEBUG_PRINT("Status: ");
  DEBUG_PRINTLN(WiFi.status());
  DEBUG_PRINT("SSID: ");
  DEBUG_PRINTLN(WiFi.SSID());
  DEBUG_PRINT("IP: ");
  DEBUG_PRINTLN(WiFi.localIP());
  DEBUG_PRINT("RSSI: ");
  DEBUG_PRINTLN(WiFi.RSSI());
  DEBUG_PRINTLN("----------------------");
}

void setup() {
   Serial.begin(115200);
  
  // Initialize hardware
  initializeMotors();
  initializeSensors();
  initializeRFID();
  initializeIndicators();
  loadLearnedTags();
  
  // Connect to network
  connectToServerAP();
  
  if(!otaStarted) { otaServer.begin(); ElegantOTA.begin(&otaServer); otaStarted = true; }
  debugWiFiConnection();
  
  // Register with server
  registerRobot();
  DEBUG_PRINTLN("Robot initialized with predefined tags");
  indicateReady();
}

void loop() {
  maintainConnection();
  
  otaServer.handleClient();
  ElegantOTA.loop();
  
  if(isLearningMode) {
    handleLearningMode();
    return;
  }
  
  if (checkObstacle()) {
    handleObstacle();
    return;
  }
  
  followLine();
  checkRFID();
  checkForCommands();
  checkDestinationTimeout();
  handleBoxPlacement();
  
  if (millis() - lastStatusUpdate > statusUpdateInterval) {
    lastStatusUpdate = millis();
    updateServerStatus();
  }
}

