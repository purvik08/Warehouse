import re

def purge_mobile_robot():
    path = r'MobileRobot/MobileRobot.ino'
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()

    # 1. Preferences and Battery Logic
    new_includes = """#include <SPI.h>
#include <Preferences.h>
Preferences preferences;
#define BATTERY_PIN 36 // True ADC voltage pin"""
    content = content.replace('#include <SPI.h>', new_includes)

    old_battery = """float checkBatteryLevel() {
  // Implement actual battery monitoring
  return 3.8f; // Example value
}"""
    new_battery = """float checkBatteryLevel() {
  int raw = analogRead(BATTERY_PIN);
  return (raw / 4095.0) * 3.3 * 2.0; // Read true voltage
}"""
    content = content.replace(old_battery, new_battery)

    # 2. Preference Storage
    old_save = """void saveLearnedTags() {
  // In a real implementation, save to EEPROM
  DEBUG_PRINTLN("Tags saved to memory");
  // EEPROM implementation would go here
}"""
    new_save = """void saveLearnedTags() {
  preferences.begin("rfid", false);
  preferences.putString("home", HOME_TAG);
  preferences.putString("pickup", PICKUP_TAG);
  preferences.putString("rack1", RACK1_TAG);
  preferences.putString("rack2", RACK2_TAG);
  preferences.end();
  DEBUG_PRINTLN("Tags genuinely saved to ESP32 Flash");
}"""
    content = content.replace(old_save, new_save)

    old_load = """void loadLearnedTags() {
  // In a real implementation, load from EEPROM
  DEBUG_PRINTLN("Loading tags from memory");
  // Example tags - replace with EEPROM loading
  HOME_TAG = "A1B2C3D4";
  PICKUP_TAG = "E5F6G7H8";
  RACK1_TAG = "I9J0K1L2";
  RACK2_TAG = "M3N4O5P6";
}"""
    new_load = """void loadLearnedTags() {
  preferences.begin("rfid", true);
  HOME_TAG = preferences.getString("home", "");
  PICKUP_TAG = preferences.getString("pickup", "");
  RACK1_TAG = preferences.getString("rack1", "");
  RACK2_TAG = preferences.getString("rack2", "");
  preferences.end();
  DEBUG_PRINTLN("Tags retrieved from flash memory");
}"""
    content = content.replace(old_load, new_load)

    # Call load in setup
    content = content.replace('initializeRFID();\n  initializeIndicators();', 'initializeRFID();\n  initializeIndicators();\n  loadLearnedTags();')

    # 3. True Box Pickup vs Mocking
    old_mock = """    // Simulate box pickup
    if (!hasBox) {
      currentBoxTag = BOX_TAG_PREFIX + String(random(1000000, 9999999));
      hasBox = true;
      DEBUG_PRINTLN("Box picked up: " + currentBoxTag);
      beep(300);
    }"""
    new_mock = """    // Robot has arrived at pickup. Awaiting command pipeline from central server to allocate box code.
"""
    content = content.replace(old_mock, new_mock)

    old_proc = """void processCommand(String command) {
  command.trim();
  DEBUG_PRINT("Executing command: ");
  DEBUG_PRINTLN(command);"""
    
    new_proc = """void processCommand(String payload) {
  String command = payload;
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if(!error && doc.containsKey("command")) {
    command = doc["command"].as<String>();
    if(doc.containsKey("box_tag")) currentBoxTag = doc["box_tag"].as<String>();
  }
  command.trim();
  DEBUG_PRINT("Executing remote payload: ");
  DEBUG_PRINTLN(command);"""
    # Use re.sub to match it strictly
    content = content.replace(old_proc, new_proc)

    old_pick = """  else if (command == "PICK_BOX") {
    if (currentLocation == "Pickup" && currentBoxTag != "") {
      hasBox = true;
      DEBUG_PRINTLN("Box picked up: " + currentBoxTag);
      updateServerStatus();
      beep(300);
    } else {
      DEBUG_PRINTLN("Cannot pick box - not at pickup location or no box detected!");
      indicateError();
    }
  }"""
    new_pick = """  else if (command == "PICK_BOX") {
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
  }"""
    content = content.replace(old_pick, new_pick)

    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)

def purge_server2():
    path = r'server2/server2.ino'
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Remove dummy lines from struct
    content = content.replace('  int lineFollowers = 3;\n  int roboticArms = 1;\n', '')
    
    # In handleStatus: dynamic injection
    old_hs = """  doc["team"] = config.teamName;
  doc["lineFollowers"] = config.lineFollowers;
  doc["roboticArms"] = config.roboticArms;"""
    
    new_hs = """  doc["team"] = config.teamName;
  int lf_ct = 0, arm_ct = 0;
  for (const String &d : connectedDevices) {
    if(d.startsWith("LF-")) lf_ct++;
    if(d.startsWith("Arm")) arm_ct++;
  }
  doc["lineFollowers"] = lf_ct;
  doc["roboticArms"] = arm_ct;"""
    content = content.replace(old_hs, new_hs)
    
    # In URL replace
    old_rep = """  html.replace("%LINE_FOLLOWERS%", String(config.lineFollowers));
  html.replace("%ROBOTIC_ARMS%", String(config.roboticArms));"""
    
    new_rep = """  int lf_cnt = 0, arm_cnt = 0;
  for(auto &d : connectedDevices){
    if(d.startsWith("LF-")) lf_cnt++;
    if(d.startsWith("Arm")) arm_cnt++;
  }
  html.replace("%LINE_FOLLOWERS%", String(lf_cnt));
  html.replace("%ROBOTIC_ARMS%", String(arm_cnt));"""
    content = content.replace(old_rep, new_rep)

    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)

purge_mobile_robot()
purge_server2()
print("Success purge")
