#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>

#define WDT_TIMEOUT 10
#define BATTERY_PIN 34
#define TRIG_PIN 14
#define ECHO_PIN 27

// L298N Motor Pins
#define MOTOR_A_IN1 26
#define MOTOR_A_IN2 25
#define MOTOR_B_IN3 33
#define MOTOR_B_IN4 32

// MFRC522 RFID Pins (Floor tracking)
#define SS_PIN 5
#define RST_PIN 0

MFRC522 rfid(SS_PIN, RST_PIN);

const String robotID = "LF-AGV-01";
String serverIP = "";
String errorState = "OK";
String currentStatus = "IDLE";
unsigned long lastCommandCheck = 0;
unsigned long lastStatusUpdate = 0;
int httpFailures = 0;

void setup() {
  Serial.begin(115200);

  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  
  pinMode(MOTOR_A_IN1, OUTPUT);
  pinMode(MOTOR_A_IN2, OUTPUT);
  pinMode(MOTOR_B_IN3, OUTPUT);
  pinMode(MOTOR_B_IN4, OUTPUT);

  stopMotors();

  SPI.begin();
  rfid.PCD_Init();

  connectNetwork();
  resolveServer();
}

void connectNetwork() {
  WiFiManager wifiManager;
  Serial.println("Starting AP Config / Connecting to WiFi...");
  if (!wifiManager.autoConnect("WarehouseConfig_LF01")) {
    Serial.println("Failed to connect and hit timeout. Restarting...");
    delay(3000);
    ESP.restart();
  }
  Serial.println("\nConnected to WiFi!");
}

void resolveServer() {
  Serial.print("Resolving warehouse.local...");
  IPAddress resolvedIP;
  int retries = 0;
  
  if (!MDNS.begin(robotID.c_str())) {
      Serial.println("Error setting up mDNS");
  }
  
  while(serverIP == "" && retries < 15) {
    esp_task_wdt_reset(); // Feed watchdog during blocking server resolution
    resolvedIP = MDNS.queryHost("warehouse");
    if(resolvedIP.toString() != "0.0.0.0") {
       serverIP = resolvedIP.toString();
       Serial.println(" Resolved: " + serverIP);
    } else {
       delay(1000);
       Serial.print(".");
       retries++;
    }
  }
  if(serverIP == "") {
      Serial.println("\nmDNS Failed. Using fallback dummy.");
      serverIP = "192.168.4.1";
  }
}

void notifyServerEvent(String eventName, String rfidTag = "") {
  if (WiFi.status() == WL_CONNECTED && serverIP != "") {
    HTTPClient http;
    String url = "http://" + serverIP + "/api/status";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["device"] = robotID;
    doc["event"] = eventName;
    doc["status"] = currentStatus;
    doc["error"] = errorState;
    if(rfidTag != "") doc["location"] = rfidTag;
    
    int rawVal = analogRead(BATTERY_PIN);
    doc["battery"] = constrain(map(rawVal, 0, 4095, 0, 100), 0, 100);
    
    String payload;
    serializeJson(doc, payload);
    http.POST(payload);
    http.end();
  }
}

void checkObstacle() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
  if (duration == 0) return; // timeout
  
  float distance = duration * 0.034 / 2;
  
  if (distance > 0 && distance < 15.0) {
    if (errorState == "OK") {
      stopMotors();
      errorState = "OBSTACLE";
      notifyServerEvent("obstacle_detected");
      Serial.println("OBSTACLE DETECTED! Motors stalled.");
    }
  } else {
    if (errorState == "OBSTACLE") {
      errorState = "OK";
      notifyServerEvent("obstacle_cleared");
      Serial.println("Obstacle cleared. Resuming normal logic.");
    }
  }
}

void processRFID() {
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    
    MFRC522::MIFARE_Key key;
    for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF; // Default factory key
    
    byte buffer[18];
    byte len = 18;
    String payload = "";
    
    // Read Block 4 where we provision our strings
    MFRC522::StatusCode status;
    status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 4, &key, &(rfid.uid));
    
    if (status == MFRC522::STATUS_OK) {
       status = rfid.MIFARE_Read(4, buffer, &len);
       if (status == MFRC522::STATUS_OK) {
          for(uint8_t i=0; i<16; i++) {
             // Only capture printable ascii characters
             if(buffer[i] >= 32 && buffer[i] <= 126) payload += (char)buffer[i];
          }
       }
    }
    
    payload.trim();
    if(payload.length() > 0) {
      Serial.println("Floor Tag Scanned: " + payload);
      notifyServerEvent("location_update", payload);
    }
    
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }
}

void checkForCommands() {
  if (WiFi.status() == WL_CONNECTED && serverIP != "") {
    HTTPClient http;
    String url = "http://" + serverIP + "/api/commands/" + robotID;
    http.begin(url);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      httpFailures = 0; // Reset failures
      String payload = http.getString();
      if(payload.length() > 0 && payload != "{}") {
         StaticJsonDocument<128> doc;
         deserializeJson(doc, payload);
         
         String cmd = doc["command"].as<String>();
         cmd.trim();
         
         // Only move if we aren't blocked by an obstacle!
         if (errorState == "OK") {
           if(cmd == "move_forward") moveForward();
           else if(cmd == "turn_left") turnLeft();
           else if(cmd == "turn_right") turnRight();
           else if(cmd == "stop") stopMotors();
         }
      }
    } else {
      httpFailures++;
      if (httpFailures > 5) {
        Serial.println("Excessive HTTP timeouts. Re-resolving mDNS Server IP...");
        serverIP = "";
        resolveServer();
        httpFailures = 0;
      }
    }
    http.end();
  }
}

void moveForward() { currentStatus = "MOVING"; digitalWrite(MOTOR_A_IN1, HIGH); digitalWrite(MOTOR_A_IN2, LOW); digitalWrite(MOTOR_B_IN3, HIGH); digitalWrite(MOTOR_B_IN4, LOW); notifyServerEvent("moving"); }
void turnLeft() { currentStatus = "TURNING"; digitalWrite(MOTOR_A_IN1, LOW); digitalWrite(MOTOR_A_IN2, HIGH); digitalWrite(MOTOR_B_IN3, HIGH); digitalWrite(MOTOR_B_IN4, LOW); notifyServerEvent("turning"); }
void turnRight() { currentStatus = "TURNING"; digitalWrite(MOTOR_A_IN1, HIGH); digitalWrite(MOTOR_A_IN2, LOW); digitalWrite(MOTOR_B_IN3, LOW); digitalWrite(MOTOR_B_IN4, HIGH); notifyServerEvent("turning");}
void stopMotors() { currentStatus = "IDLE"; digitalWrite(MOTOR_A_IN1, LOW); digitalWrite(MOTOR_A_IN2, LOW); digitalWrite(MOTOR_B_IN3, LOW); digitalWrite(MOTOR_B_IN4, LOW); notifyServerEvent("stopped");}

void loop() {
  esp_task_wdt_reset();
  
  if (WiFi.status() != WL_CONNECTED) {
    connectNetwork();
  }
  
  checkObstacle();
  processRFID();
  
  if (millis() - lastCommandCheck > 1000) {
    checkForCommands();
    lastCommandCheck = millis();
  }
  
  if (millis() - lastStatusUpdate > 5000) {
    notifyServerEvent("heartbeat");
    lastStatusUpdate = millis();
  }
  
  delay(10);
}
