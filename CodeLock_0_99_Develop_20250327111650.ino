#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <time.h>  // Include time library
#include <ESP8266WebServer.h>
#include <ElegantOTA.h>

#include <PubSubClient.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <Keypad.h>

#define JSON_FILENAME "/config.json"

/*
[env:esp12e]
platform = espressif8266
board = esp12e
framework = arduino
lib_deps =
    bbx10/DNSServer
    bblanchon/ArduinoJson
    tzapu/WiFiManager
    arduino-libraries/NTPClient
    knolleary/PubSubClient
    chris--a/Keypad

This is beta code for CodeLock 0.99 with: 
    WiFi connectivity.
    Password protected admin and OTA update web pages.
    MQTT connectivity for automations with Home Assistant or similar.
    Supports 8 multiple individual codes, consisting of the digits 0 through 9, * and #, with a maximum length of 8 digits.
    Different codes can be valid at specific hours of the day.
    Unlock the lock using the keypad by entering a valid code, or
    unlock from admin page, or unlock from MQTT.
    If "AlwaysOpen" is set, the lock will be unlocked all the time.
    Automatically connects to an MQTT broker if one is configured.
    Unlock and set AlwaysOpen from MQTT.
    The program updates an MQTT server/topic with activity, by whom the door was unlocked, commands published from MQTT or admin page, and input from the keypad.
    A reed switch connected to D0 detects changes if used in lock (locked/unlocked) or door (open/closed). 
    Serial interface enabled in USB, 9600 baud.
*/
ESP8266WebServer server(80);
// AsyncWebServer server(80);

// Admin credentials
String adminUser = "admin";

struct Code {
  String code;
  int validFrom;
  int validTo;
  String remark;
  int counter = 0;       // Counter keeping track of correctly entered keypad digits
};

// Default values
const String version = "0.99g";
const char* defaultDoorName = "Door";
const char* defaultAdminPassword = "adminpass";
const char* defaultCode = "12345678";
const int defaultValidFrom = 0;
const int defaultValidTo = 0;
const char* defaultRemark = "Default";
const int relayPin = D8; // GPIO15 (D8) // Using this pin does not activate relay during boot
const int defaultRelayPullTime = 1000;
const int reedSwitchPin = D0;  // GPIO15 (D8) Detects if handle is engaged or door is opened
// const int tz = +2;
const char* defaultNtpServer = "pool.ntp.org"; 
const char* defaultTimeZone = "CET-1CEST,M3.5.0,M10.5.0/3";  // See https://gist.github.com/alwynallan/24d96091655391107939

char topic[300];  // Ensure this buffer is large enough for your topic string
bool alwaysOpen = false;  // State variable
bool unLocked = true;     // State variable, assume unlocked after a power on
bool reedSwitchState;     // Reed switch is closed(true) or opened(false)
bool lastState;           // Track last reed switch state

String mqttServer = "";  // These are configurable/changable options
String mqttUser = "";
String mqttPassword = "";
String mqttTopic = "";
String adminPassword = defaultAdminPassword;
String doorName = defaultDoorName;
String message = "";
int relayPullTime = defaultRelayPullTime;
// const char* ntpServer = defaultNtpServer;
// const char* timeZone = defaultTimeZone;
char ntpServer[50];  // Adjust size as needed
char timeZone[50];

unsigned long lastWifiAttempt = millis();  // Store the last attempt time for WiFi reconnection
const unsigned long wifiRetryInterval = 60000;  // Retry every 60 seconds

WiFiClient espClient;
PubSubClient client(espClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServer, 0, 60000);  

Code accessCodes[8]; // Create an array to store user codes

// Matris-keypad configuration
const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};
byte rowPins[ROWS] = {D1, D2, D3, D4};  // The ESP8266 pins connect to the row pins
byte colPins[COLS] = {D5, D6, D7}; // The ESP8266 pins connect to the column pins
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Function to initialize the JSON file
void initializeJson() {
  // Serial.println("Initializing JSON file...");
  message = "Initializing JSON file...";
  Serial.println(message.c_str());
  snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
  client.publish(topic, message.c_str());


  StaticJsonDocument<3096> doc;

  // Populate default values for codes
  JsonArray codesArray = doc.createNestedArray("codes");
  for (int i = 0; i < 8; i++) {
    JsonObject codeObj = codesArray.createNestedObject();
    if (i == 0) { // Default first code
      codeObj["code"] = defaultCode;
      codeObj["validFrom"] = defaultValidFrom;
      codeObj["validTo"] = defaultValidTo;
      codeObj["remark"] = defaultRemark;
    } else {
      codeObj["code"] = "";
      codeObj["validFrom"] = defaultValidFrom;
      codeObj["validTo"] = defaultValidTo;
      codeObj["remark"] = "";
    }
  }

  // Add MQTT and admin settings
  doc["mqttServer"] = "";
  doc["mqttUser"] = "";
  doc["mqttPassword"] = "";
  doc["mqttTopic"] = "";
  doc["adminPassword"] = defaultAdminPassword;
  doc["doorName"] = defaultDoorName;
  // **Add NTP, TimeZone, and Relay Pull Time**
  doc["ntpServer"] = defaultNtpServer;
  doc["timeZone"] = defaultTimeZone;
  doc["relayPullTime"] = defaultRelayPullTime;  // Use a safe default integer

  // Save to LittleFS
  File file = LittleFS.open(JSON_FILENAME, "w");
  if (!file) {
    Serial.println("Failed to create JSON file.");
    return;
  }
  serializeJson(doc, file);
  file.close();
  // Serial.println("JSON file initialized.");
  message = "JSON file initialized.";
  Serial.println(message.c_str());
  snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
  client.publish(topic, message.c_str());
}

// Function to load JSON configuration
void loadJson() {
  File file = LittleFS.open(JSON_FILENAME, "r");
  if (!file) {
    Serial.println("JSON file not found. Initializing...");
    initializeJson();
    file = LittleFS.open(JSON_FILENAME, "r");
    if (!file) {
      // Serial.println("Failed to read JSON file after initialization.");
      message = "Failed to read JSON file after initialization.";
      Serial.println(message.c_str());
      snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
      client.publish(topic, message.c_str());
      return;
    }
  }

  StaticJsonDocument<3096> doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    message = "Failed to parse JSON. Reinitializing...";
    Serial.println(message.c_str());
    snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(topic, message.c_str());
    initializeJson();
    return;
  }

  // Load codes
  JsonArray codesArray = doc["codes"];
  for (size_t i = 0; i < codesArray.size(); i++) {
    JsonObject codeObj = codesArray[i];
    accessCodes[i].code = codeObj["code"].as<String>();
    accessCodes[i].validFrom = codeObj["validFrom"];
    accessCodes[i].validTo = codeObj["validTo"];
    accessCodes[i].remark = codeObj["remark"].as<String>();
  }

  // Load MQTT settings
  mqttServer = doc["mqttServer"].as<String>();
  mqttUser = doc["mqttUser"].as<String>();
  mqttPassword = doc["mqttPassword"].as<String>();
  mqttTopic = doc["mqttTopic"].as<String>();
  adminPassword = doc["adminPassword"].as<String>();  // Load admin password
  doorName = doc["doorName"].as<String>();     // Load door name
  // Load NTP & TimeZone with Safety Checks
  strlcpy(ntpServer, doc["ntpServer"] | defaultNtpServer, sizeof(ntpServer));
  strlcpy(timeZone, doc["timeZone"] | defaultTimeZone, sizeof(timeZone));
  // Ensure relayPullTime is a valid integer
  relayPullTime = doc["relayPullTime"].is<int>() ? doc["relayPullTime"].as<int>() : defaultRelayPullTime;

  file.close();
}

// Function to handle the /opendoor request
void handleOpenDoor() {
  unlockDoor();  // Call the unlock routine
  server.send(200, "text/plain", "Door unlocked successfully");  // Send response
  String timestamp = getFormattedTime();
  message = "Door unlocked by admin";
  Serial.print(timestamp.c_str());
  Serial.print(" ");
  Serial.println(message.c_str());
  snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
  client.publish(topic, message.c_str());
}

// Function to handle the /deleteConfig request
void handleDeleteConfig() {
  deleteConfig();  // Call the delete routine
  server.send(200, "text/plain", "JSON config file deleted successfully");  // Send response
  message = "JSON config file deleted from admin page.";
  Serial.println(message.c_str());
  snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
  // client.publish(topic, message.c_str());
}

// Function to handle the /reboot request
void handleReboot() {
  server.send(200, "text/plain", "Rebooting....");  // Send response
  message = "Rebooting....";
  Serial.println(message.c_str());
  snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
  client.publish(topic, message.c_str());
  delay(500);
  reBoot();  // Call the Reboot routine
}

// Function to handle the /alwaysOpen request
void handleAlwaysOpen() {
  alwaysOpen = !alwaysOpen;  // Toggle state

  String timestamp = getFormattedTime();
  if (alwaysOpen == true) {
    message = timestamp + " AlwaysOpen is now on.";
    lastState = !reedSwitchState;
  } else {
    message = timestamp + " AlwaysOpen is now off.";
  }
  Serial.print(timestamp.c_str());
  Serial.print(" ");    
  Serial.println(message.c_str());
  snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
  client.publish(topic, message.c_str());

  server.send(200, "text/plain", alwaysOpen ? "Always Open Enabled" : "Always Open Disabled");
//server.sendContent(""); // Sends no content, but toggles as long button is pressed.

}

// Function to handle configuration page
void handleConfigPage() {
  // Basic authentication handler
  if (!server.authenticate(adminUser.c_str(), adminPassword.c_str())) {
    return server.requestAuthentication();
  }
  String timestamp = getFormattedTime();
  String html = "<html><body>";
  html += "<h1>" + doorName + " CodeLock Configuration"; "</h1>";
  html += "<h6>" + version + " " + timestamp; "</h6>";
  // html += "</br>";
  // html += "<h6>" " relayPullTime(mS): " + relayPullTime; "</h6>";
  
  // Existing form fields for codes, MQTT settings, etc.
  html += "<form action=\"/save\" method=\"post\">";  // Correct form start

  // New buttons with properly escaped attributes
  // html += "<button type=\"button\" onclick=\"saveConfig()\">Save</button>";
  html += "<button type=\"button\" id=\"toggleLockBtn\" onclick=\"openDoor()\">Unlock</button>";


  // Button with dynamic text based on `alwaysOpen` state
  html += "<button type=\"button\" id=\"alwaysOpenBtn\" onclick=\"toggleAlwaysOpen()\">"
          + String(alwaysOpen ? "Disable Always Unlocked" : "Enable Always Unlocked") + "</button>";


  html += "<button type=\"button\" onclick=\"deleteConfig()\">Delete Config File</button>";
  html += "<button type=\"button\" onclick=\"reBoot()\">Reboot</button>";

  // JavaScript functions for buttons
  html += "<script>";
  
  // JavaScript functions for Open Door button
  html += "function openDoor() {";
  html += "  fetch('/opendoor')";
  html += "  .then(response => response.text())";
  html += "  .then(text => {";
  html += "    updateToggleLockBtn(text);";  // Update button text and color
  html += "  });";
  html += "}";

   
  html += "function updateToggleLockBtn(state) {";
  html += "  let btn = document.getElementById('updateToggleLockBtn');";
  html += "  if (state.includes('Enabled')) {";
  html += "    btn.innerText = 'Disable Always Unlocked';";
  html += "    btn.style.backgroundColor = '#77dd77';";  // Red when enabled
  html += "  } else {";
  html += "    btn.innerText = 'Enable Always Unlocked';";
  html += "    btn.style.backgroundColor = '#ff6961';";  // Green when disabled
  html += "  }";
  html += "}";


  // JavaScript to toggle the Always Open button
  html += "function toggleAlwaysOpen() {";
  html += "  fetch('/alwaysopen')";  // Send request to toggle state
  html += "  .then(response => response.text())";
  html += "  .then(text => {";
  html += "    alert(text);";  // Display the status message
  html += "    updateButton(text);";  // Update button text and color
  html += "  });";
  html += "}";

  // Function to update the AlwaysOpen button based on response
  html += "function updateButton(state) {";
  html += "  let btn = document.getElementById('alwaysOpenBtn');";
  html += "  if (state.includes('Enabled')) {";
  html += "    btn.innerText = 'Disable Always Unlocked';";
  html += "    btn.style.backgroundColor = '#77dd77';";  // Red when enabled
  html += "  } else {";
  html += "    btn.innerText = 'Enable Always Unlocked';";
  html += "    btn.style.backgroundColor = '#ff6961';";  // Green when disabled
  html += "  }";
  html += "}";

  // JavaScript functions for Delete Config button
  html += "function deleteConfig() {";
  html += "  fetch('/deleteconfig')";
  html += "  .then(response => response.text())";
  html += "  .then(alert);";
  html += "}";

  html += "function forgetWiFi() {";
  html += "  if (confirm('Are you sure you want to forget the current WiFi settings? This will restart the device.')) {";
  html += "    fetch('/forgetwifi')";
  html += "    .then(response => response.text())";
  html += "    .then(alert);";
  html += "  }";
  html += "}";

  // JavaScript functions for Reboot button
  html += "function reBoot() {";
  html += "  fetch('/reboot')";
  html += "  .then(response => response.text())";
  html += "  .then(alert);";
  html += "}";

  html += "window.onload = function() {";
  html += "  updateButton('" + String(alwaysOpen ? "Enabled" : "Disabled") + "');";
  html += "}";
  
  html += "</script>";
  
  // Form fields for codes
  for (int i = 0; i < 8; i++) {
    html += "<h6></br></h6>";
    html += "Code" + String(i + 1) + ": <input type=\"text\" name=\"code" + String(i) + "\" value=\"" + accessCodes[i].code + "\" maxlength=\"8\"><br>";
    html += "Valid From (0[:00]-23[:00]): <input type=\"number\" name=\"validFrom" + String(i) + "\" value=\"" + String(accessCodes[i].validFrom) + "\" min=\"0\" max=\"23\"><br>";
    html += "Valid To (0[:00]-23[:00]): <input type=\"number\" name=\"validTo" + String(i) + "\" value=\"" + String(accessCodes[i].validTo) + "\" min=\"0\" max=\"23\"><br>";
    html += "Remark: <input type=\"text\" name=\"remark" + String(i) + "\" value=\"" + accessCodes[i].remark + "\" maxlength=\"10\"><br>";
  }


  // NTP and Time Settings fields
  html += "<h3>NTP & Time Settings</h3>";
  html += "NTP Server: <input type=\"text\" name=\"ntpServer\" value=\"" + String(ntpServer) + "\"><br>";
  html += "Time Zone: <input type=\"text\" name=\"timeZone\" value=\"" + String(timeZone) + "\"><br>";
  html += "Relay Pull Time (ms): <input type=\"number\" name=\"relayPullTime\" value=\"" + String(relayPullTime) + "\" min=\"100\" max=\"5000\"><br>";


  // MQTT settings fields
  html += "<h3>MQTT Settings</h3>";
  html += "Server: <input type=\"text\" name=\"mqttServer\" value=\"" + mqttServer + "\"><br>";
  html += "User: <input type=\"text\" name=\"mqttUser\" value=\"" + mqttUser + "\"><br>";
  html += "Password: <input type=\"password\" name=\"mqttPassword\" value=\"" + mqttPassword + "\"><br>";
  html += "Topic: <input type=\"text\" name=\"mqttTopic\" value=\"" + mqttTopic + "\"><br>";

  // Admin password field
  html += "<h3>Admin Password</h3>";
  html += "Password: <input type=\"text\" name=\"adminPassword\" value=\"" + adminPassword + "\"><br>";

  html += "<h3>Door Name</h3>";  // Door name field
  html += "Door Name: <input type=\"text\" name=\"doorName\" value=\"" + doorName + "\"><br>";

  html += "<br><input type=\"submit\" value=\"Save\"></form>";  // Submit button

  html += "<button type=\"button\" onclick=\"forgetWiFi()\">Forget WiFi</button>"; // Forget WiFi button
  html += "</body></html>";
  server.send(200, "text/html", html);
}


// Function to save configuration
void handleSave() {
  StaticJsonDocument<3096> doc;
  Serial.println("Saving JSON file.");

  JsonArray codesArray = doc.createNestedArray("codes");
  for (int i = 0; i < 8; i++) {
    JsonObject codeObj = codesArray.createNestedObject();
    codeObj["code"] = server.arg("code" + String(i));
    codeObj["validFrom"] = server.arg("validFrom" + String(i)).toInt();
    codeObj["validTo"] = server.arg("validTo" + String(i)).toInt();
    codeObj["remark"] = server.arg("remark" + String(i));
  }

  doc["mqttServer"] = server.arg("mqttServer");
  doc["mqttUser"] = server.arg("mqttUser");
  doc["mqttPassword"] = server.arg("mqttPassword");
  doc["mqttTopic"] = server.arg("mqttTopic");
  doc["adminPassword"] = server.arg("adminPassword");
  doc["doorName"] = server.arg("doorName");
  doc["ntpServer"] = server.arg("ntpServer");
  doc["timeZone"] = server.arg("timeZone");
  // Ensure valid relayPullTime
  int tempRelayPullTime = server.arg("relayPullTime").toInt();
  if (tempRelayPullTime <= 0) {
    tempRelayPullTime = defaultRelayPullTime;  // Reset to default if invalid
  }
  doc["relayPullTime"] = tempRelayPullTime;

  File file = LittleFS.open(JSON_FILENAME, "w");
  if (!file) {
    server.send(500, "text/plain", "Failed to save configuration.");
    return;
  }

  serializeJson(doc, file);
  file.close();

  loadJson(); // Reload saved configuration
  server.sendHeader("Location", "/");
  server.send(303);
}

void SerPrintAndPubMess(String mess) {
  String timestamp = getFormattedTime();
  Serial.print(timestamp.c_str());
  Serial.print(" ");
  Serial.println(mess.c_str());
  snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
  client.publish(topic, mess.c_str());
}

void unlockDoor() {
  Serial.println(relayPullTime);
  digitalWrite(relayPin, HIGH);  // Pull relay
  delay(relayPullTime);                    // in milliSeconds.
  digitalWrite(relayPin, LOW);   // Release relay
  unLocked = true;  // True until reedSwitch indicates door handle has been pressed, or door has been opened.
}

void deleteConfig() {
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount LittleFS.");
    return;
  }
  // Check if the file exists
  if (LittleFS.exists(JSON_FILENAME)) {
    // Delete the file
    if (LittleFS.remove(JSON_FILENAME)) {
      Serial.println("JSON file deleted successfully.");
    } else {
      Serial.println("Failed to delete JSON file.");
    }
  } else {
    Serial.println("JSON file does not exist.");
  }
  loadJson();
}

void handleForgetWiFi() {
    WiFi.disconnect(true);
    server.send(200, "text/plain", "WiFi credentials erased. Restarting...");
    delay(1000);
    ESP.restart();
}

void reBoot() {
  Serial.println("Rebooting...");
  delay(100);  // Short delay to allow the message to be printed
  ESP.restart();  // Reboot the ESP8266
}

// MQTT callback for commands
void callback(char* topic, byte* payload, unsigned int length) {
  String command = "";
  for (int i = 0; i < length; i++) {
    command += (char)payload[i];
  }

    if (command == "Unlock") {
      unlockDoor();
      String timestamp = getFormattedTime();
      message = "Door unlocked by MQTT";
      Serial.print(timestamp.c_str());
      Serial.print(" ");
      Serial.println(message.c_str());

      // Use a different topic buffer here
      char pub_topic[200];  // Publish topic buffer
      snprintf(pub_topic, sizeof(pub_topic), "%s/CodeLock/activity", mqttTopic.c_str());
      client.publish(pub_topic, message.c_str());

    }
  if (command == "AlwaysOpenOn") {
    alwaysOpen = true;
    lastState = !reedSwitchState;
    String timestamp = getFormattedTime();
    message = "AlwaysOpenOn set by MQTT";
    Serial.print(timestamp.c_str());
    Serial.print(" ");
    Serial.println(message.c_str());

    // Use a different topic buffer here
    char pub_topic[200];  // Publish topic buffer
    snprintf(pub_topic, sizeof(pub_topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(pub_topic, message.c_str());

  }
  if (command == "AlwaysOpenOff") {
    alwaysOpen = false;
    String timestamp = getFormattedTime();
    message = "AlwaysOpenOff set via MQTT";
    Serial.print(timestamp.c_str());
    Serial.print(" ");
    Serial.println(message.c_str());

    // Use a different topic buffer here
    char pub_topic[200];  // Publish topic buffer
    snprintf(pub_topic, sizeof(pub_topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(pub_topic, message.c_str());

  }
  // Receiving a single char of 0-9,*,# will be accepted as buttons pressed
  if (command.length() == 1 && (isdigit(command.charAt(0)) || command.charAt(0) == '*' || command.charAt(0) == '#')) {
    String timestamp = getFormattedTime();
    message = "Keypress " + command + " received via MQTT";
    Serial.print(timestamp.c_str());
    Serial.print(" ");
    Serial.println(message.c_str());

    // Use a different topic buffer here
    char pub_topic[200];  // Publish topic buffer
    snprintf(pub_topic, sizeof(pub_topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(pub_topic, message.c_str());
    char key = command.charAt(0);
    handleKeyInput(key);
  }

  if (command.startsWith("SetTimeZone ")) { // Check if the command starts with "SetTimeZone "
    String tzValue = command.substring(12); // Extract everything after "SetTimeZone "

    // Convert String to char array
    static char newTimeZone[50];  // Buffer to store new timezone
    tzValue.toCharArray(newTimeZone, sizeof(newTimeZone));
    strlcpy(timeZone, newTimeZone, sizeof(timeZone));

    String timestamp = getFormattedTime();
    message = "TimeZone changed by MQTT to " + String(timeZone); // Reply with info
    Serial.print(timestamp.c_str());
    Serial.print(" ");
    Serial.println(message.c_str());

    // Use a different topic buffer here
    char pub_topic[200];  // Publish topic buffer
    snprintf(pub_topic, sizeof(pub_topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(pub_topic, message.c_str());

    // Apply the new timezone setting
    configTime(timeZone, ntpServer);

    delay(2000); // Wait a moment to allow time sync
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) { 
        Serial.println("Failed to set new timezone, reverting to default.");
        
        // timeZone = defaultTimeZone;  // Reset to default
        strlcpy(timeZone, defaultTimeZone, sizeof(timeZone));
        configTime(timeZone, ntpServer);

        // Notify about the fallback
        message = "TimeZone change failed, reverted to default: " + String(defaultTimeZone);
        Serial.println(message);
        client.publish(pub_topic, message.c_str());
    } else {
        Serial.println("Timezone updated successfully.");
    }
  }

  if (command.startsWith("SetNtpServer ")) { // Check if the command starts with "SetNtpServer "
    String ntpServerValue = command.substring(13); // Extract everything after "SetNtpServer "

    // Convert String to char array
    static char newNtpServer[50];  // Buffer to store new NtpServer
    ntpServerValue.toCharArray(newNtpServer, sizeof(newNtpServer));
    strlcpy(ntpServer, newNtpServer, sizeof(ntpServer));  // Update ntpServer pointer

    String timestamp = getFormattedTime();
    message = "NtpServer changed by MQTT to " + String(ntpServer); // Reply with info
    Serial.print(timestamp.c_str());
    Serial.print(" ");
    Serial.println(message.c_str());

    // Use a different topic buffer here
    char pub_topic[200];  // Publish topic buffer
    snprintf(pub_topic, sizeof(pub_topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(pub_topic, message.c_str());

    // Apply the new timezone setting
    configTime(timeZone, ntpServer);

    delay(2000); // Wait a moment to allow time sync
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) { 
        Serial.println("Failed to set new NtpServer, reverting to default.");
        
        strlcpy(ntpServer, defaultNtpServer, sizeof(ntpServer));  // Reset to default
        configTime(timeZone, ntpServer);

        // Notify about the fallback
        message = "NtpServer change failed, reverted to default: " + String(defaultNtpServer);
        Serial.println(message);
        client.publish(pub_topic, message.c_str());
    } else {
        Serial.println("NtpServer updated successfully.");
    }
  }

  
  if (command.startsWith("SetRelayPullTime ")) { // Check if the command starts with "SetRelayPullTime "
    String relayPullTimeValue = command.substring(17); // Extract value after the command

    // Convert String to integer safely
    relayPullTime = relayPullTimeValue.toInt(); 

    String timestamp = getFormattedTime();
    message = "RelayPullTime changed by MQTT to " + String(relayPullTime); // Reply with info
    Serial.print(timestamp.c_str());
    Serial.print(" ");
    Serial.println(message.c_str());

    // Use a different topic buffer here
    char pub_topic[200];  // Publish topic buffer
    snprintf(pub_topic, sizeof(pub_topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(pub_topic, message.c_str());
  }
}

// MQTT reconnect
void reconnect() {
    String clientId = "CodeLock-" + doorName; // Unique Client ID
    if (client.connect(clientId.c_str(), mqttUser.c_str(), mqttPassword.c_str())) {

//  if (client.connect("%sCodeLock", mqttUser.c_str(), mqttPassword.c_str())) {
      char sub_topic[300];  // Subscription topic buffer
      snprintf(sub_topic, sizeof(sub_topic), "%s/CodeLock/cmnd", mqttTopic.c_str());
//    snprintf(sub_topic, sizeof(sub_topic), "%s/CodeLock/%s/cmnd", mqttTopic.c_str(), doorName.c_str()); // unique cmnd topic?
      client.subscribe(sub_topic);  // Subscribe using sub_topic
      snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
      client.publish(topic, "Connected to MQTT");
    }
}


// Is now a valid time for this code?
bool isValidTime(int startHour, int endHour) {
    int currentHour = getFormattedTime().substring(11, 13).toInt();
    if (endHour >= startHour) {  // Valid period does not cross midnight
      if (currentHour >= startHour && currentHour < endHour) { // Time is after startTime and before stopTime
        return true;
      }
    }
    if (endHour <= startHour) {  // Valid period cross midnight
      if (currentHour >= startHour && currentHour < 24) { // Time is before midnight
        return true;
      }
      if (currentHour >= 0 && currentHour < endHour) { // Time is after midnight
        return true;
      }
    }
  Serial.println("startHour: " + String(startHour) + " currentHour: " + String(currentHour) + " endHour: " + String(endHour));
  return false;
}


void handleKeyInput(char key) {
  bool codeMatched = false;
  Serial.print("Key pressed: ");
  Serial.println(key);

  // Make single keypress to string and publish
  char keyStr[2];           // Buffer to hold the character and a null terminator
  keyStr[0] = key;          // Store the character
  keyStr[1] = '\0';         // Null terminator to make it a valid C-string
  // topic = String(mqttTopic.c_str()) + "/CodeLock/keypressed";
  snprintf(topic, sizeof(topic), "%s/CodeLock/keypressed", mqttTopic.c_str());
  client.publish(topic, keyStr);

  for (int i = 0; i < sizeof(accessCodes) / sizeof(accessCodes[0]); i++) {

    // Check if the key matches the expected character in the code sequence
    if (key == accessCodes[i].code[accessCodes[i].counter]) {
      accessCodes[i].counter++;  // Move to the next character
      Serial.print("Correct input for code ");
      Serial.print(i);
      Serial.print(". Counter: ");
      Serial.println(accessCodes[i].counter);

      // Check if the entire code has been entered correctly
      if (accessCodes[i].counter == accessCodes[i].code.length()) {
        if (isValidTime(accessCodes[i].validFrom, accessCodes[i].validTo)) {
          unlockDoor();  // Unlock if time is valid
          message = "Door unlocked by " + accessCodes[i].remark;
          //SerPrintAndPubMess(message); //Fråga ch varför detta inte funkar
          String timestamp = getFormattedTime();
          Serial.print(timestamp.c_str());
          Serial.print(" ");
          Serial.println(message.c_str());
          snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
          client.publish(topic, message.c_str());
          
          accessCodes[i].counter = 0;  // Reset counter after successful entry
          codeMatched = true;
        } else {
          String timestamp = getFormattedTime();
          message = "Code valid for " + accessCodes[i].remark + ", but not within allowed time. (" + timestamp + ")" ;
          Serial.println(message.c_str());
          snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
          client.publish(topic, message.c_str());
        }
      }
    } else {
      // Incorrect input, check if it matches the first digit of this code
      if (key == accessCodes[i].code[0]) {
        accessCodes[i].counter = 1; // Restart from the first digit
        Serial.print("Incorrect input, but matches first digit. Restarting counter for code ");
        Serial.println(i);
      } else {
        // Fully incorrect input, reset counter for this code
        if (accessCodes[i].counter > 0) {  // Only print if counter was non-zero
          Serial.print("Incorrect input. Resetting counter for code ");
          Serial.println(i);
        }
        accessCodes[i].counter = 0;
      }
    }
  }

  if (!codeMatched) {
    Serial.println("No code matched. Waiting for next input.");
  }
}

void setup() {
  Serial.begin(9600);
  unsigned long startTime = millis();
  while (!Serial && (millis() - startTime) < 3000) {  // Wait for 3 seconds
    // Optional: Blink LED to indicate waiting
  }
  if (Serial) {
    Serial.println("Serial monitor detected");
  } else {
    // Proceed without serial connection
    delay(3500);
  }

  message.reserve(300);  // Reserve space for up to 300 characters

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  // pinMode(reedSwitchPin, INPUT_PULLUP); // Pin connected via reedswitch to ground
  pinMode(reedSwitchPin, INPUT_PULLDOWN_16); // Pin connected via reedswitch to 3.3V or via resistor to Vcc
  reedSwitchState = digitalRead(reedSwitchPin); //Check if Reed switch is closed(true) or opened(false)
  lastState = reedSwitchState; // Save present state to see if state changes later.

  if (!LittleFS.begin()) {
    if (Serial) { Serial.println("Failed to mount LittleFS."); }
    return;
  }

  loadJson();

  //   // Wi-Fi & AP-konfiguration
  //   WiFiManager wifiManager;
  //   wifiManager.autoConnect("CodeLock-AP");  // Startar AP om nätverk saknas

  // while (WiFi.status() != WL_CONNECTED) {
  //   delay(500);
  //   Serial.print(".");
  // }

  // Wi-Fi & AP configuration
  WiFiManager wifiManager;
  // wifiManager.autoConnect("CodeLock-AP");  // Starts AP if no network is found

  wifiManager.setConfigPortalTimeout(60);  // 1-minute timeout for AP mode

  if (!wifiManager.autoConnect("CodeLock-AP")) {
    Serial.println("Failed to connect and no configuration entered. Continuing...");
  }


  // Non-blocking WiFi connection attempt
  WiFi.begin();
  unsigned long wifiTimeout = millis() + 10000; // 10-second timeout

  while (WiFi.status() != WL_CONNECTED && millis() < wifiTimeout) {
      delay(500);
      Serial.print(".");
  }

  // If WiFi is not connected, continue without blocking
  if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected!");
  } else {
      Serial.println("\nWiFi not connected, running in offline mode.");
  }


  // if (Serial) { 
  //   Serial.println("\nWiFi connected!"); 
  //   }
  Serial.println(adminPassword.c_str());
  Serial.println(mqttServer.c_str());
  Serial.println(mqttUser.c_str());
  Serial.println(mqttPassword.c_str());
  Serial.println(mqttTopic.c_str());

  Serial.println("");
  Serial.println(version);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println(ntpServer);
  Serial.println(timeZone);
  Serial.println(relayPullTime);


  ElegantOTA.begin(&server);    // Start ElegantOTA, http://<IP address>/update
  ElegantOTA.setAuth(adminUser.c_str(), adminPassword.c_str());  // Set Authentication Credentials
  server.begin();
  Serial.println("OTA HTTP server started");

  // Connect to MQTT
  client.setServer(mqttServer.c_str(), 1883);
  client.setCallback(callback);
  String topic = "CodeLock";

  // Configure time zone and start NTP
  configTime(0, 0, ntpServer);
  setenv("TZ", timeZone, 1);
  tzset();

  server.on("/", handleConfigPage);
  server.on("/save", HTTP_POST, handleSave);
  // Define the routine for unlocking the door
  server.on("/opendoor", HTTP_GET, handleOpenDoor);
  // Define the routine for deleting the JSON config file
  server.on("/deleteconfig", HTTP_GET, handleDeleteConfig);
  // Define the routine for reboot
  server.on("/reboot", HTTP_GET, handleReboot);
  // Define the routine for the alwaysOpen state
  server.on("/alwaysopen", HTTP_GET, handleAlwaysOpen);
  server.on("/forgetwifi", HTTP_GET, handleForgetWiFi); // Forget WiFi routine

  server.begin();  // Start the server

  String timestamp = getFormattedTime();
  message = timestamp + " HTTP server started:80";
  Serial.println(message.c_str());
}

// Function to print formatted time
String getFormattedTime() {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

void loop() {
  // Check if WiFi is lost and retry every 60 seconds
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt >= wifiRetryInterval) {
      Serial.println("WiFi lost, attempting to reconnect...");
      WiFi.begin();
      lastWifiAttempt = millis();
  }



  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  server.handleClient();  // Handle webrequests

// Handle state of Reed Switch
  reedSwitchState = digitalRead(reedSwitchPin); //Check if Reed switch is closed(true) or opened(false)

  if (alwaysOpen == true && reedSwitchState != lastState) {  // Reed switch has changed state
    // alwaysOpen is set and reed switch has changed, so unlock door again
    String timestamp = getFormattedTime();
    message = "Door unlocked by AlwaysOpen";
    Serial.print(timestamp.c_str());
    Serial.print(" ");
    Serial.println(message.c_str());
    snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(topic, message.c_str());
    //delay(200);
    unlockDoor();  // Call existing unlock function
  }

  if (reedSwitchState == false && reedSwitchState != lastState) {  // Reed switch has changed state
    // ReedSwitch changed state and is now open.
    String timestamp = getFormattedTime();
    message = "Reed switch open";
    Serial.print(timestamp.c_str());
    Serial.print(" ");
    Serial.println(message.c_str());
    snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(topic, message.c_str());
    //delay(200);
  }

  if (reedSwitchState == true && reedSwitchState != lastState) {  // Reed switch has changed state
    //ReedSwitch changed state, reed switch is now closed
    String timestamp = getFormattedTime();
    message = "Reed switch closed";
    Serial.print(timestamp.c_str());
    Serial.print(" ");
    Serial.println(message.c_str());
    snprintf(topic, sizeof(topic), "%s/CodeLock/activity", mqttTopic.c_str());
    client.publish(topic, message.c_str());
  }

  //delay(200);
  lastState = reedSwitchState; // Save state to compare with if something changes later.

// Repeatedly read buttons
  char key = keypad.getKey();
  if (key) {
    handleKeyInput(key);
  }
}
