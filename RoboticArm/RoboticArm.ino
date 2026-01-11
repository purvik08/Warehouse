#include <Servo.h>
#include <ArduinoJson.h>

// Servo Configuration
Servo baseServo;
Servo shoulderServo;
Servo elbowServo;
Servo wristServo;
Servo gripperServo;

const int basePin = 2;
const int shoulderPin = 3;
const int elbowPin = 4;
const int wristPin = 5;
const int gripperPin = 6;

String armStatus = "IDLE";
bool hasBox = false;

void setup() {
  Serial.begin(9600);
  
  // Attach servos
  baseServo.attach(basePin);
  shoulderServo.attach(shoulderPin);
  elbowServo.attach(elbowPin);
  wristServo.attach(wristPin);
  gripperServo.attach(gripperPin);
  
  // Home position
  goHome();
  sendStatus();
}

void loop() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    processCommand(command);
  }
  delay(10);
}

void processCommand(String command) {
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, command);
  
  if (error) {
    sendError("PARSE_ERROR");
    return;
  }
  
  if (!doc.containsKey("command")) {
    sendError("INVALID_COMMAND");
    return;
  }
  
  String cmd = doc["command"];
  
  if (cmd == "pick") {
    pickBox();
  } else if (cmd == "place") {
    placeBox();
  } else if (cmd == "home") {
    goHome();
  } else if (cmd == "status") {
    sendStatus();
  } else {
    sendError("UNKNOWN_COMMAND");
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
  
  // Notify server
  StaticJsonDocument<128> response;
  response["event"] = "pickup_complete";
  serializeJson(response, Serial);
  Serial.println();
}

void placeBox() {
  if (!hasBox) {
    sendError("NO_BOX");
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
  
  // Notify server
  StaticJsonDocument<128> response;
  response["event"] = "place_complete";
  serializeJson(response, Serial);
  Serial.println();
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
  StaticJsonDocument<128> doc;
  doc["status"] = armStatus;
  doc["hasBox"] = hasBox;
  serializeJson(doc, Serial);
  Serial.println();
}

void sendError(String error) {
  StaticJsonDocument<128> doc;
  doc["error"] = error;
  serializeJson(doc, Serial);
  Serial.println();
}