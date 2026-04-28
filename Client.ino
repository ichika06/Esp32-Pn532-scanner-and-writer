#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Preferences.h>
#include <time.h>

// Provide the token generation process info.
#include "addons/TokenHelper.h"

// Provide the RTDB payload printing info and other helper functions.
#include "addons/RTDBHelper.h"

// Define GPIO pins for the switches and buttons
#define SWITCH_PIN_UP 14
#define SWITCH_PIN_DOWN 12
#define SWITCH_PIN_SELECT 27
#define LED_GREEN 33
#define LED_YELLOW 32
#define LED_RED 4

// Define NVS namespace
#define NVS_NAMESPACE "wifi"

// Define Firebase project API Key
#define API_KEY ""

// Define Firebase Realtime Database URL
#define DATABASE_URL ""

// Define Firebase Firestore Project ID (extract from your database URL)
#define FIREBASE_PROJECT_ID ""

// Define Firebase authentication email and password
#define FIREBASE_EMAIL "example@gmail.com"
#define FIREBASE_PASSWORD "password"

// Define PN532 pins
#define PN532_SCK   18
#define PN532_MISO  19
#define PN532_MOSI  23
#define PN532_SS    5
#define SDA_LCD 21
#define SCL_LCD 22
#define BUZZER      13
#define BATTERY_PIN 34  // ADC pin for battery voltage
#define MAX_VOLTAGE 12.6  // Full battery voltage
#define MIN_VOLTAGE 3.0   // Empty battery voltage

hd44780_I2Cexp lcd;

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
String textToWrite = "";
bool writeRequested = false;
bool readRequested = false;
bool continuousReadMode = false;
unsigned long lastReadTime = 0;

// Firebase data objects
FirebaseData fbdo;
FirebaseData fbdo_stream; // Separate object for streaming
FirebaseConfig config;
FirebaseAuth auth;

Preferences preferences;
bool wifiMode = false; // Flag to track Wi-Fi mode
bool serialUsbMode = false; // Flag to track Serial-USB mode
bool firebaseInitialized = false; // Flag to track Firebase initialization
bool wifiConnected = false; // Flag to track WiFi connection status

// Heartbeat state variable (now global, not static in loop)
bool heartbeatInitialized = false;

void pNote(int frequency, int duration) {
    tone(BUZZER, frequency, duration);
    delay(duration * 1.3);
    noTone(BUZZER);
}

void updateBatteryDisplay(int batteryPercent) {
    lcd.setCursor(15, 0);
    lcd.print(batteryPercent);
    lcd.print("%");
}

bool connectToWiFi(const char* ssid, const char* password) {
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nFailed to connect.");
    WiFi.disconnect();
    return false;
  }
}

// Game Boy-style startup tones: A4 and A5
int startupMelody[][2] = {
  {440, 200},  // A4 - 440 Hz
  {880, 400}   // A5 - 880 Hz
};

void playStartupTone() {
  for (int i = 0; i < 2; i++) {
    pNote(startupMelody[i][0], startupMelody[i][1]);
    delay(50); // Short pause between tones
  }
}

// --- Forward declaration for device status update ---
void setDeviceStatus(const String& status);

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32 NFC Reader/Writer Ready");

    // Initialize GPIO pins
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_YELLOW, OUTPUT);
    pinMode(LED_RED, OUTPUT);
    pinMode(SWITCH_PIN_UP, INPUT_PULLUP);
    pinMode(SWITCH_PIN_DOWN, INPUT_PULLUP);
    pinMode(SWITCH_PIN_SELECT, INPUT_PULLUP);
    

    Wire.begin(SDA_LCD, SCL_LCD);
    lcd.begin(16, 2);
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("System Init...");
    delay(1000);

    // Initialize NVS
    preferences.begin(NVS_NAMESPACE, false);

    nfc.begin();
    uint32_t versiondata = nfc.getFirmwareVersion();
    if (!versiondata) {
        Serial.println("Didn't find PN53x board");
        lcd.clear();
        lcd.print("NFC Error!");
        while (1);
    }

    Serial.println("PN532 Initialized");
    lcd.clear();
    lcd.print("Scanner Ready");
    delay(1000);

    // Configure Firebase but don't initialize yet
    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    auth.user.email = FIREBASE_EMAIL;
    auth.user.password = FIREBASE_PASSWORD;
    config.token_status_callback = tokenStatusCallback;
    config.max_token_generation_retry = 5;

    lcd.clear();
    lcd.print("PEMSS");
    updateBatteryDisplay(50); // Initial battery display

}

// --- Function forward declarations ---
bool connectWithSavedCredentials();
void checkSerialCommand();
void detectAndWriteNFC();
void detectAndReadNFC();
void handleModeSelection();
void enterWiFiMode();
void scanAndConnectToWiFi();
void initializeFirebase();
void updateFirebase(const String& data);
String readNFC();
bool formatNFC();
bool writeNDEFText(const char* text);
bool writeNDEFMessage(uint8_t* message, uint8_t length);
void updateModeDisplay(int mode);
void streamCallback(FirebaseStream data);
void streamTimeoutCallback(bool timeout);
void uploadToRTDB(const String& tagData);
void setDeviceStatus(const String& status); // Forward declaration for device status update

void loop() {
    checkSerialCommand();
    handleModeSelection();

    // Check WiFi connection status periodically
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 30000) { // Check every 30 seconds
        lastWifiCheck = millis();
        if (wifiConnected && WiFi.status() != WL_CONNECTED) {
            // WiFi was connected but now disconnected - try to reconnect
            Serial.println("WiFi connection lost. Attempting to reconnect...");
            connectWithSavedCredentials();
        }
    }

    if (wifiMode) {
        enterWiFiMode();
        wifiMode = false; // Reset the flag after entering Wi-Fi mode
    }

    // --- Refactored NFC waiting logic ---
    static bool waitingForWrite = false;
    static bool waitingForRead = false;
    static bool waitingForContinuousRead = false;
    static unsigned long lastNfcCheck = 0;
    static unsigned long nfcCheckInterval = 100; // Check every 100ms

    if (writeRequested) {
        waitingForWrite = true;
        writeRequested = false;
        lcd.setCursor(0, 0);
        lcd.clear();
        lcd.print("Waiting for");
        lcd.setCursor(0, 1);
        lcd.print("NFC Card...");
        Serial.println("Waiting for NFC card to write...");
    }
    if (readRequested) {
        waitingForRead = true;
        readRequested = false;
        lcd.setCursor(0, 0);
        lcd.clear();
        lcd.print("Waiting for");
        lcd.setCursor(0, 1);
        lcd.print("NFC Card...");
        Serial.println("Waiting for NFC card to read...");
    }
    if (continuousReadMode) {
        waitingForContinuousRead = true;
    } else {
        waitingForContinuousRead = false;
    }

    // Non-blocking NFC card check
    if (millis() - lastNfcCheck > nfcCheckInterval) {
        lastNfcCheck = millis();
        uint8_t uid[7];
        uint8_t uidLength;
        if (waitingForWrite) {
            if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
                detectAndWriteNFC();
                waitingForWrite = false;
            }
        } else if (waitingForRead) {
            if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
                detectAndReadNFC();
                waitingForRead = false;
            }
        } else if (waitingForContinuousRead && (millis() - lastReadTime >= 300)) {
            if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
                detectAndReadNFC();
                lastReadTime = millis();
            }
        }
    }

    int adcBattery = analogRead(BATTERY_PIN);
    float batteryVoltage = (adcBattery / 4095.0) * 3.3 * ((100.0 + 27.0) / 27.0);
    int batteryPercent = map(batteryVoltage * 10, MIN_VOLTAGE * 10, MAX_VOLTAGE * 10, 0, 100);
    batteryPercent = constrain(batteryPercent, 0, 100);

    updateBatteryDisplay(batteryPercent);

    static unsigned long lastStatusCheck = 0;
    if (millis() - lastStatusCheck > 10000) { // Check every 10 seconds
        lastStatusCheck = millis();
        if (wifiConnected && firebaseInitialized && WiFi.status() == WL_CONNECTED) {
            setDeviceStatus("online");
        } else if (wifiConnected && firebaseInitialized) {
            setDeviceStatus("offline");
        }
    }

    // Heartbeat: update last_seen timestamp in RTDB every 8-10 seconds (randomized)
    static unsigned long lastHeartbeat = 0;
    static unsigned long heartbeatInterval = 8000 + random(0, 2001); // 8000-10000 ms
    if (!heartbeatInitialized && wifiConnected && firebaseInitialized && WiFi.status() == WL_CONNECTED && Firebase.ready()) {
        // Set last_seen to 0 at the start of heartbeat
        Firebase.RTDB.setInt(&fbdo, "/PEMSS_Client_2/last_seen", 0);
        heartbeatInitialized = true;
    }
    if (millis() - lastHeartbeat > heartbeatInterval) {
        lastHeartbeat = millis();
        heartbeatInterval = 8000 + random(0, 2001); // Randomize next interval
        if (wifiConnected && firebaseInitialized && WiFi.status() == WL_CONNECTED && Firebase.ready()) {
            // Store current time as last_seen (seconds since epoch)
            time_t now;
            time(&now);
            Firebase.RTDB.setInt(&fbdo, "/PEMSS_Client_2/last_seen", now);
        }
    }

    static unsigned long lastStatusAutoUpdate = 0;
    if (millis() - lastStatusAutoUpdate > 8000) {
        lastStatusAutoUpdate = millis();
        if (wifiConnected && firebaseInitialized && WiFi.status() == WL_CONNECTED) {
            setDeviceStatus("online");
        } else if (wifiConnected && firebaseInitialized) {
            setDeviceStatus("offline");
        }
    }
}


bool connectWithSavedCredentials() {
    String ssid = preferences.getString("ssid", "");
    String password = preferences.getString("pass", "");

    if (ssid != "" && password != "") {
        Serial.println("Found saved credentials");
        Serial.print("SSID: ");
        Serial.println(ssid);

        lcd.clear();
        lcd.print("Connecting to");
        lcd.setCursor(0, 1);
        lcd.print(ssid);

        digitalWrite(LED_YELLOW, HIGH); // Yellow LED while connecting
        bool connectionResult = connectToWiFi(ssid.c_str(), password.c_str());

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            Serial.print(".");
            digitalWrite(LED_YELLOW, !digitalRead(LED_YELLOW)); // Blink yellow LED
            attempts++;
        }

        digitalWrite(LED_YELLOW, LOW);

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\nConnected with saved credentials");
            lcd.clear();
            lcd.print("Connected to");
            lcd.setCursor(0, 1);
            lcd.print(ssid);

            digitalWrite(LED_GREEN, HIGH); // Green LED for success
            delay(1000);
            digitalWrite(LED_GREEN, LOW);

            wifiConnected = true;
            if (!firebaseInitialized) {
                initializeFirebase();
                firebaseInitialized = true;
            }
            return true;
        } else {
            Serial.println("\nFailed to connect with saved credentials");
            lcd.clear();
            lcd.print("Connection");
            lcd.setCursor(0, 1);
            lcd.print("Failed");

            digitalWrite(LED_RED, HIGH); // Red LED for failure
            delay(1000);
            digitalWrite(LED_RED, LOW);

            wifiConnected = false;
            return false;
        }
    } else {
        Serial.println("No saved credentials found");
        wifiConnected = false;
        return false;
    }
}

void checkSerialCommand() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();

        if (command.startsWith("WRITE:")) {
            textToWrite = command.substring(6);
            Serial.println("Received text to write: " + textToWrite);
            writeRequested = true;
        } else if (command == "READ") {
            readRequested = true;
        } else if (command == "READ ON") {
            continuousReadMode = true;
            Serial.println("Continuous read mode ON");
        } else if (command == "READ OFF") {
            continuousReadMode = false;
            Serial.println("Continuous read mode OFF");
        } else if (command == "RESET WIFI") {
            // Command to clear saved WiFi credentials
            preferences.remove("ssid");
            preferences.remove("pass");
            Serial.println("WiFi credentials cleared. Restarting...");
            ESP.restart();
        }
    }
}

void detectAndWriteNFC() {
    // Write mode is disabled for non-admins
    lcd.clear();
    lcd.print("Not an Admin");
    delay(3000);
    lcd.clear();
    lcd.print("");
    // Serial.println("Write mode is disabled: Not an Admin");
    // (Original write logic commented out below)
    /*
    uint8_t uid[7];
    uint8_t uidLength;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
        Serial.println("NFC Detected! Formatting and Writing...");
        pNote(859, 200);
        lcd.clear();
        lcd.print("Formatting NFC...");
        lcd.setCursor(0, 1);
        lcd.print("Don't remove card!");
        for (int i = 0; i < 3; i++) {
            digitalWrite(LED_YELLOW, HIGH);
            delay(200);
            digitalWrite(LED_YELLOW, LOW);
            delay(200);
        }
        bool formatResult = formatNFC();
        if (formatResult) {
            Serial.println("Format successful!");
            digitalWrite(LED_YELLOW, HIGH);
            bool writeResult = writeNDEFText(textToWrite.c_str());
            digitalWrite(LED_YELLOW, LOW);
            if (writeResult) {
                Serial.println("Write successful!");
                lcd.setCursor(0, 0);
                lcd.clear();
                lcd.print("Write Success:");
                lcd.setCursor(0, 1);
                lcd.print("PEMSS");
                digitalWrite(LED_GREEN, HIGH);
                delay(2000);
                digitalWrite(LED_GREEN, LOW);
            } else {
                Serial.println("Write failed!");
                lcd.setCursor(0, 0);
                lcd.clear();
                lcd.print("Write Failed!");
                delay(2000);
            }
        } else {
            Serial.println("Format failed!");
            lcd.setCursor(0, 0);
            lcd.clear();
            lcd.print("Format Failed!");
            digitalWrite(LED_YELLOW, HIGH);
            delay(2000);
            digitalWrite(LED_YELLOW, LOW);
        }
    } else {
        Serial.println("No NFC detected. Try again.");
    }
    */
}

void detectAndReadNFC() {
    uint8_t uid[7];
    uint8_t uidLength;

    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
        Serial.println("NFC Card Detected! Reading...");
        digitalWrite(LED_YELLOW, HIGH);
        delay(300);
        digitalWrite(LED_YELLOW, LOW);

        pNote(859, 200);

        String tagData = readNFC();
        if (!tagData.isEmpty()) {
            Serial.println("Read successful: " + tagData);
            lcd.setCursor(0, 0);
            lcd.clear();
            lcd.print("Read Success:");
            digitalWrite(LED_GREEN, HIGH);
            delay(500);
            digitalWrite(LED_GREEN, LOW);
            lcd.setCursor(0, 1);
            lcd.print(tagData);

            // Upload to RTDB (for data storage)
            if (wifiConnected && firebaseInitialized) {
                if (Firebase.ready() && WiFi.status() == WL_CONNECTED) {
                    uploadToRTDB(tagData);
                }
            }
            // Removed: last_seen update on NFC tap (now handled only by heartbeat)
            delay(2000);
        } else {
            Serial.println("Read failed!");
            lcd.setCursor(0, 0);
            lcd.clear();

            lcd.print("Read Failed!");
            digitalWrite(LED_RED, HIGH);
            delay(2000);
            digitalWrite(LED_RED, LOW);
        }
    } else {
        Serial.println("No NFC detected. Try again.");
    }
}

String readNFC() {
    String tagData = "";
    uint8_t raw[128] = {0};
    int idx = 0;
    bool foundNdef = false;
    // Read all user pages (4-41)
    for (uint8_t i = 4; i < 42; i++) {
        uint8_t data[4] = {0};
        if (nfc.ntag2xx_ReadPage(i, data)) {
            for (int j = 0; j < 4; j++) {
                if (data[j] == 0xFE) { // NDEF end marker
                    foundNdef = true;
                    break;
                }
                if (idx < 128) {
                    raw[idx++] = data[j];
                }
            }
        }
        if (foundNdef) break;
    }
    // Parse NDEF text record
    if (raw[0] == 0x03) {
        uint8_t ndefLen = raw[1];
        if (raw[2] == 0xD1 && raw[5] == 'T') {
            uint8_t payloadLen = raw[4];
            uint8_t status = raw[6];
            uint8_t langLen = status & 0x3F;
            uint8_t textStart = 7 + langLen;
            uint8_t textLen = payloadLen - 1 - langLen;
            for (uint8_t i = 0; i < textLen; i++) {
                if ((textStart + i) < 128) {
                    char c = (char)raw[textStart + i];
                    if (c >= 32 && c <= 126) {
                        tagData += c;
                    }
                }
            }
        }
    }
    return tagData;
}

bool formatNFC() {
    uint8_t emptyNDEF[4] = {0x03, 0x01, ' ', 0xFE}; // NDEF Start, Length 1, Space, End of NDEF
    uint8_t emptyPage[4] = {0x00, 0x00, 0x00, 0x00};

    // Write initial space as a placeholder
    if (!nfc.ntag2xx_WritePage(4, emptyNDEF)) {
        return false;
    }

    // Wipe remaining user memory (Pages 5-41)
    for (uint8_t i = 5; i < 42; i++) {
        if (!nfc.ntag2xx_WritePage(i, emptyPage)) {
            return false;
        }
    }

    return true;
}

bool writeNDEFText(const char* text) {
    uint8_t ndefMessage[128];
    memset(ndefMessage, 0, sizeof(ndefMessage));  // Ensure buffer is cleared

    uint8_t textLen = strlen(text);
    const char* langCode = "en";
    uint8_t langLen = 2; // Length of "en"
    uint8_t payloadLen = 1 + langLen + textLen; // status + lang + text
    uint8_t recordLen = 5 + payloadLen; // header + type + payload

    if (recordLen > 120) return false;

    ndefMessage[0] = 0x03;  // NDEF Start
    ndefMessage[1] = recordLen;
    ndefMessage[2] = 0xD1;  // Short Record, TNF_WELL_KNOWN
    ndefMessage[3] = 0x01;  // Type Length
    ndefMessage[4] = payloadLen;  // Payload Length
    ndefMessage[5] = 'T';  // 'T' for Text Record
    ndefMessage[6] = langLen; // Status byte: UTF-8, length of lang code
    memcpy(ndefMessage + 7, langCode, langLen); // Language code
    memcpy(ndefMessage + 7 + langLen, text, textLen); // Actual text
    ndefMessage[7 + langLen + textLen] = 0xFE;  // NDEF End Marker

    // Overwrite any remaining space with 0x00 to remove old data
    for (int i = 7 + langLen + textLen + 1; i < 128; i++) {
        ndefMessage[i] = 0x00;
    }

    return writeNDEFMessage(ndefMessage, 128);
}

bool writeNDEFMessage(uint8_t* message, uint8_t length) {
    // Write mode is disabled for non-admins
    // Do nothing and always return false
    // Serial.println("writeNDEFMessage: Not an Admin, write blocked");
    return false;
    /*
    uint8_t pageBuffer[4];
    for (uint8_t i = 0; i < length; i += 4) {
        memset(pageBuffer, 0, 4);
        memcpy(pageBuffer, &message[i], min(4, length - i));
        if (!nfc.ntag2xx_WritePage(4 + (i / 4), pageBuffer)) {
            return false;
        }
    }
    return true;
    */
}

void enterWiFiMode() {
    Serial.println("Entering Wi-Fi mode...");

    // First try to connect with saved credentials
    if (!connectWithSavedCredentials()) {
        // If no saved credentials or connection failed, scan for networks
        scanAndConnectToWiFi();
    }
    // Only proceed if WiFi is connected
    if (wifiConnected) {
        if (!firebaseInitialized) {
            initializeFirebase();
            firebaseInitialized = true;
        }
    }
}

void scanAndConnectToWiFi() {
    Serial.println("Scanning for Wi-Fi networks...");
    lcd.clear();
    lcd.print("Scanning WiFi...");
    digitalWrite(LED_YELLOW, HIGH);

    int n = WiFi.scanNetworks();
    Serial.println("Scan done");
    digitalWrite(LED_YELLOW, LOW);

    if (n == 0) {
        Serial.println("No networks found");
        lcd.clear();
        lcd.print("No networks found");
        digitalWrite(LED_RED, HIGH);
        delay(1000);
        digitalWrite(LED_RED, LOW);
        return;
    }

    Serial.print(n);
    Serial.println(" networks found");
    lcd.clear();
    lcd.print("Networks found:");
    lcd.setCursor(0, 1);
    lcd.print(String(n));
    delay(1000);

    int selectedNetwork = 0;
    while (true) {
        lcd.clear();
        lcd.print(WiFi.SSID(selectedNetwork));
        lcd.setCursor(0, 1);
        lcd.print(WiFi.RSSI(selectedNetwork));
        lcd.print(" dBm");

        // Wait for button press
        while (true) {
            if (digitalRead(SWITCH_PIN_UP) == LOW) {
                delay(200); // Debounce delay
                selectedNetwork = (selectedNetwork - 1 + n) % n;
                break;
            }
            if (digitalRead(SWITCH_PIN_DOWN) == LOW) {
                delay(200); // Debounce delay
                selectedNetwork = (selectedNetwork + 1) % n;
                break;
            }
            if (digitalRead(SWITCH_PIN_SELECT) == LOW) {
                delay(200); // Debounce delay
                String selectedSSID = WiFi.SSID(selectedNetwork);
                lcd.clear();
                lcd.print("Enter password");
                lcd.setCursor(0, 1);
                lcd.print("for ");
                lcd.print(selectedSSID);

                // Wait for password input via Serial
                Serial.println("Enter password for " + selectedSSID + ":");
                while (Serial.available() == 0) {
                    // Wait for user input
                    delay(100);
                }
                String selectedPassword = Serial.readStringUntil('\n');
                selectedPassword.trim();

                lcd.clear();
                lcd.print("Connecting to");
                lcd.setCursor(0, 1);
                lcd.print(selectedSSID);

                if (connectToWiFi(selectedSSID.c_str(), selectedPassword.c_str())) {
                    // Save credentials to NVS
                    preferences.putString("ssid", selectedSSID);
                    preferences.putString("pass", selectedPassword);
                    wifiConnected = true;

                    // Debug: Print saved credentials
                    Serial.println(F("Credentials saved successfully"));

                    // Initialize Firebase only if WiFi is connected
                    if (!firebaseInitialized) {
                        initializeFirebase();
                        firebaseInitialized = true;
                    }

                    digitalWrite(LED_GREEN, HIGH); // Success indicator
                    delay(1000);
                    digitalWrite(LED_GREEN, LOW);
                } else {
                    wifiConnected = false;
                    digitalWrite(LED_RED, HIGH); // Failure indicator
                    delay(1000);
                    digitalWrite(LED_RED, LOW);
                }

                return;
            }
        }
    }
}

void initializeFirebase() {
    Serial.println(F("Initializing Firebase..."));
    lcd.clear();
    lcd.print("Connecting to");
    lcd.setCursor(0, 1);
    lcd.print("Firebase...");

    // REMOVED: syncTime();

    // Enable auto-reconnect
    Firebase.reconnectWiFi(true);

    // Configure Firebase
    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    auth.user.email = FIREBASE_EMAIL;
    auth.user.password = FIREBASE_PASSWORD;

    // Initialize Firebase
    Firebase.begin(&config, &auth);

    // Set timeouts for RTDB
    Firebase.RTDB.setReadTimeout(&fbdo, 1000);
    Firebase.RTDB.setwriteSizeLimit(&fbdo, "tiny");
    Firebase.setFloatDigits(2);
    Firebase.setDoubleDigits(6);

    // Wait for connection with timeout
    unsigned long startTime = millis();
    bool connected = false;
    while (!connected && (millis() - startTime < 15000)) {
        if (Firebase.ready()) {
            connected = true;
            Serial.println("Firebase connected successfully!");
            break;
        }
        Serial.println("Waiting for Firebase...");
        delay(500);
    }

    if (!connected) {
        Serial.println("Firebase connection timeout");
        lcd.clear();
        lcd.print("Firebase timeout");
        return;
    }

    Serial.println("Firebase connected successfully");
    lcd.clear();
    lcd.print("Firebase");
    lcd.setCursor(0, 1);
    lcd.print("Connected!");
    delay(2000);

    // Only set device status if in Wi-Fi mode
    if (wifiMode) {
        setDeviceStatus("online");
    }

    // Set heartbeatInitialized to false so heartbeat logic in loop() takes over
    extern bool heartbeatInitialized;
    heartbeatInitialized = false;

    // Set up Firebase RTDB stream to listen for commands (RTDB for commands only)
    if (!Firebase.RTDB.beginStream(&fbdo_stream, "/PEMSS_Client_2/command")) {
        Serial.println("Failed to begin RTDB stream for commands");
        Serial.println(fbdo_stream.errorReason());
    } else {
        Serial.println("RTDB stream for commands started successfully");
        // Set up stream callback functions only if stream begins successfully
        Firebase.RTDB.setStreamCallback(&fbdo_stream, streamCallback, streamTimeoutCallback);
    }
}

void updateFirebase(const String& data) {
    // This function is kept for compatibility but not used for NFC data
    // Commands still use RTDB, data goes to Firestore
    if (Firebase.ready() && WiFi.status() == WL_CONNECTED) {
        if (Firebase.RTDB.setString(&fbdo, "/PEMSS_Client_2/status", "Device Active")) {
            Serial.println("Status updated successfully");
        } else {
            Serial.println("Failed to update status");
            Serial.println(fbdo.errorReason());
        }
    }
}

void handleModeSelection() {
    static unsigned long holdStartTime = 0;
    static bool holdDetected = false;
    static bool modeSelectionActive = false;
    static int selectedMode = 0;
    static bool waitForSelectRelease = false;
    static bool upPrevState = HIGH;
    static bool downPrevState = HIGH;

    // Check if the select button is held for 3 seconds
    if (digitalRead(SWITCH_PIN_SELECT) == LOW) {
        if (!holdDetected && !modeSelectionActive) {
            holdStartTime = millis();
            holdDetected = true;
        } else if (!modeSelectionActive && (millis() - holdStartTime >= 3000)) {
            modeSelectionActive = true;
            updateModeDisplay(selectedMode);
            selectedMode = 0;
            lcd.clear();
            lcd.print("Mode Selection");
            holdDetected = false;
            waitForSelectRelease = true; // Wait for release before accepting OK
        }
    } else {
        holdDetected = false;
        holdStartTime = 0;
    }

    // Wait for SELECT to be released before allowing OK
    if (modeSelectionActive && waitForSelectRelease) {
        if (digitalRead(SWITCH_PIN_SELECT) == HIGH) {
            waitForSelectRelease = false;
        }
        return; // Don't process mode selection until released
    }

    // Handle mode selection if active
    if (modeSelectionActive && !waitForSelectRelease) {
        // UP button edge detection
        bool upState = digitalRead(SWITCH_PIN_UP);
        if (upPrevState == HIGH && upState == LOW) {
            selectedMode = (selectedMode - 1 + 2) % 2; // Cycle through modes
            updateModeDisplay(selectedMode);
            delay(200); // Debounce
        }
        upPrevState = upState;

        // DOWN button edge detection
        bool downState = digitalRead(SWITCH_PIN_DOWN);
        if (downPrevState == HIGH && downState == LOW) {
            selectedMode = (selectedMode + 1) % 2; // Cycle through modes
            updateModeDisplay(selectedMode);
            delay(200); // Debounce
        }
        downPrevState = downState;

        // SELECT button for OK
        if (digitalRead(SWITCH_PIN_SELECT) == LOW) {
            delay(200); // Debounce delay
            if (selectedMode == 0) {
                serialUsbMode = true; // Set the Serial-USB mode flag
            } else if (selectedMode == 1) {
                wifiMode = true; // Set the Wi-Fi mode flag
            }
            modeSelectionActive = false;
            lcd.clear();
            lcd.print("Mode Selected");
            delay(1000);
            lcd.clear();
        }
    }
}

void updateModeDisplay(int mode) {
    lcd.setCursor(0, 1);
    lcd.print("                "); // Clear the line
    lcd.setCursor(0, 1);
    if (mode == 0) {
        lcd.print("> Serial-USB");
    } else if (mode == 1) {
        lcd.print("> Wi-Fi Mode");
    }
}

// Callback function to handle RTDB stream data (for commands only)
void streamCallback(FirebaseStream data) {
    if (data.dataType() != "null") {
        String path = data.dataPath();
        String command = data.stringData();
        Serial.print("Received command from RTDB path: ");
        Serial.println(path);
        Serial.print("Command value: ");
        Serial.println(command);

        // Process the command (commands still come from RTDB)
        if (command.startsWith("WRITE:")) {
            // Block write for non-admins: show Not an Admin and do not trigger write
            lcd.clear();
            lcd.print("Not an Admin");
            delay(3000);
            lcd.clear();
            lcd.print("PEMSS");
            Serial.println("Write command blocked: Not an Admin");
            // Do NOT set writeRequested or perform any write
        } else if (command == "READ") {
            readRequested = true;
        } else if (command == "READ ON") {
            continuousReadMode = true;
            Serial.println("Continuous read mode ON");
        } else if (command == "READ OFF") {
            continuousReadMode = false;
            Serial.println("Continuous read mode OFF");
        }
    }
}

// Callback function for stream timeout
void streamTimeoutCallback(bool timeout) {
    if (timeout) {
        Serial.println("RTDB Stream timeout, resuming...");
    }
}

void setDeviceStatus(const String& status) {
    static String lastStatus = "";
    lastStatus = status;
    // Always set the status immediately when called
    String statusPath = "/PEMSS_Client_2/status";
    if (Firebase.RTDB.setString(&fbdo, statusPath, status)) {
        Serial.println("Device status updated: " + status);
    } else {
        Serial.println("Failed to update device status: " + fbdo.errorReason());
    }
}

void uploadToRTDB(const String& tagData) {
    Serial.println("Uploading to RTDB...");
    String scanKey = "/PEMSS_Client_2/scanned_data/scan";
    FirebaseJson content;
    content.set("data", tagData);
    content.set("device_id", "PEMSS_Client_2");
    content.set("scan_id", "scan");
    if (Firebase.RTDB.setJSON(&fbdo, scanKey, &content)) {
        Serial.println("Successfully updated RTDB scan node!");
        Serial.println("Data: " + tagData);
        digitalWrite(LED_GREEN, HIGH);
        delay(100);
        digitalWrite(LED_GREEN, LOW);
    } else {
        Serial.println("Failed to update RTDB scan node");
        Serial.println("Error: " + fbdo.errorReason());
        digitalWrite(LED_RED, HIGH);
        delay(100);
        digitalWrite(LED_RED, LOW);
    }
}
