#include <SPI.h>
#include <MFRC522.h>

#define RST_PIN 0
#define SS_PIN 5

MFRC522 rfid(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;

void setup() {
  Serial.begin(115200);
  SPI.begin();
  rfid.PCD_Init();

  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF; // Standard factory key
  }
  
  Serial.println("\n===========================================");
  Serial.println("  Warehouse RFID Provisioning Utility");
  Serial.println("===========================================");
  Serial.println("Standard Prefixes: BOXA_, BOXB_, BOXC_, LOC_");
  Serial.println("1. Type your desired identifier (e.g. 'BOXA_001') in the Serial box above.");
  Serial.println("2. Hit Enter/Send.");
  Serial.println("3. Hold a blank RFID tag to the reader to burn the payload.");
  Serial.println("-------------------------------------------\n");
}

String pendingPayload = "";

void loop() {
  // Read target payload from Serial Monitor
  if (Serial.available() > 0) {
    pendingPayload = Serial.readStringUntil('\n');
    pendingPayload.trim();
    if(pendingPayload.length() > 0) {
      Serial.print(">>> Target Payload Ready: [");
      Serial.print(pendingPayload);
      Serial.println("]. Present a MIFARE tag now...");
    }
  }

  if (pendingPayload.length() == 0) {
    return; // Fast loop, waiting for user input
  }

  // Look for a tag
  if ( ! rfid.PICC_IsNewCardPresent() || ! rfid.PICC_ReadCardSerial()) {
    return;
  }

  Serial.println("Card Detected...");

  // Prepare the 16-byte buffer for Block 4
  byte blockAddr = 4;
  byte dataBlock[16];
  for(int i=0; i<16; i++) dataBlock[i] = 0x20; // Pre-fill with spaces (ASCII 32)
  
  for(int i=0; i<pendingPayload.length() && i<16; i++) {
    dataBlock[i] = pendingPayload[i];
  }

  MFRC522::StatusCode status;
  
  // Authenticate Block 4
  status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(rfid.uid));
  if (status != MFRC522::STATUS_OK) {
    Serial.print("PCD_Authenticate() failed: ");
    Serial.println(rfid.GetStatusCodeName(status));
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  // Write buffer to block
  status = rfid.MIFARE_Write(blockAddr, dataBlock, 16);
  if (status != MFRC522::STATUS_OK) {
    Serial.print("MIFARE_Write() failed: ");
    Serial.println(rfid.GetStatusCodeName(status));
  } else {
    Serial.println("SUCCESS: Tag provisioned with payload!");
    pendingPayload = ""; // Reset for next tag
    Serial.println("\nReady for the next tag. Type a new payload...");
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(1500); // Debounce
}
