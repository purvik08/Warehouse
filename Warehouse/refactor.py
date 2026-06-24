import os
import re

def refactor_server():
    path = r'server2\server2.ino'
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()

    # 1. Includes and debug macros
    includes_old = """#include "FS.h"
#include "SD_MMC.h"
#include <MFRC522.h>"""
    includes_new = """#include <LittleFS.h>
#include <MFRC522.h>

#define DEBUG 1

#if DEBUG
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif"""
    content = content.replace(includes_old, includes_new)

    # 2. NanoSerial defs
    nanoserial_defs = """#define SERIAL_RX 13
#define SERIAL_TX 12
HardwareSerial NanoSerial(1);"""
    content = content.replace(nanoserial_defs, "")

    # 3. SD_MMC globally -> LittleFS
    content = content.replace('SD_MMC', 'LittleFS')

    # 4. initializeSD()
    init_sd_old = """void initializeSD() {
  Serial.println("Initializing SD card...");

  if (!LittleFS.begin()) {
    Serial.println("SD Card Mount Failed");
    Serial.println("Check: ");
    Serial.println("1. Is card inserted?");
    Serial.println("2. Is it formatted as FAT32?");
    Serial.println("3. Are pins properly connected?");
    return;
  }

  Serial.println("SD Card mounted successfully");

  uint8_t cardType = LittleFS.cardType();

  if(cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }
  
  Serial.print("SD Card Type: ");

  if(cardType == CARD_MMC) Serial.println("MMC");
  else if(cardType == CARD_SD) Serial.println("SDSC");
  else if(cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("UNKNOWN");"""

    init_sd_new = """void initializeStorage() {
  DEBUG_PRINTLN("Initializing LittleFS...");

  if (!LittleFS.begin(true)) {
    DEBUG_PRINTLN("LittleFS Mount Failed");
    return;
  }
  DEBUG_PRINTLN("LittleFS mounted successfully");"""
    
    # We do a smart replace since we already replaced SD_MMC to LittleFS globally
    content = content.replace(init_sd_old, init_sd_new)
    
    # 5. NanoSerial usage
    content = content.replace('NanoSerial.println(command);', 'notifyRobot("Arm-01", command, "");')
    content = content.replace('NanoSerial.begin(9600, SERIAL_8N1, SERIAL_RX, SERIAL_TX);', '')
    content = content.replace('initializeSD();', 'initializeStorage();')

    # Remove NanoSerial available block
    sn = content.find('if (NanoSerial.available()) {')
    if sn != -1:
        en = content.find('}', sn) + 1
        content = content[:sn] + content[en:]

    # Remove rogue begin() failure that used SD_MMC
    content = content.replace('''  if (!LittleFS.begin()) 
  {
  Serial.println("SD Card Mount Failed");
  return; // Handle failure properly
  }''', '')

    # 6. Global replace Serial.* to DEBUG_PRINT*
    content = re.sub(r'\bSerial\.print\(', r'DEBUG_PRINT(', content)
    content = re.sub(r'\bSerial\.println\(', r'DEBUG_PRINTLN(', content)
    content = re.sub(r'\bSerial\.printf\(', r'DEBUG_PRINTF(', content)
    
    # Fix the Serial.begin that got regex replaced
    content = content.replace('DEBUG_PRINTLN(115200);', 'Serial.begin(115200);')
    content = content.replace('while(!Serial)', 'while(!Serial)') # unaffected

    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)

def refactor_robot():
    path = r'MobileRobot\MobileRobot.ino'
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()

    debug_macros = """
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
"""
    content = content.replace('#include <SPI.h>\n', '#include <SPI.h>\n' + debug_macros)
    content = re.sub(r'\bSerial\.print\(', r'DEBUG_PRINT(', content)
    content = re.sub(r'\bSerial\.println\(', r'DEBUG_PRINTLN(', content)
    content = re.sub(r'\bSerial\.printf\(', r'DEBUG_PRINTF(', content)
    content = content.replace('DEBUG_PRINTLN(115200);', 'Serial.begin(115200);')

    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)

refactor_server()
refactor_robot()
print("Success")
