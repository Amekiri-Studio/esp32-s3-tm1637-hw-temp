#include <Arduino.h>
#include <Preferences.h>
#include <TM1637Display.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// 定义连接数码管的引脚
#define CLK 11
#define DIO 12

constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long DATA_TIMEOUT_MS = 5000;
constexpr unsigned long DISPLAY_STARTUP_DELAY_MS = 250;
constexpr unsigned long WIFI_SUCCESS_DISPLAY_MS = 1500;
constexpr size_t SERIAL_BUFFER_SIZE = 192;
constexpr size_t WIFI_FIELD_BUFFER_SIZE = 65;
constexpr unsigned int WIFI_UDP_PORT = 4210;
constexpr char WIFI_PREFS_NAMESPACE[] = "wifi";
constexpr char WIFI_PREFS_SSID_KEY[] = "ssid";
constexpr char WIFI_PREFS_PASS_KEY[] = "pass";

const uint8_t DASH_SEGMENTS[] = {SEG_G, SEG_G, SEG_G, SEG_G};
const uint8_t WIFI_CONNECTED_SEGMENTS[] = {
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
};

// 初始化显示对象
TM1637Display display(CLK, DIO);
Preferences preferences;
WiFiUDP udpReceiver;
char serialBuffer[SERIAL_BUFFER_SIZE];
size_t serialLength = 0;
char wifiSsid[WIFI_FIELD_BUFFER_SIZE] = "";
char wifiPassword[WIFI_FIELD_BUFFER_SIZE] = "";
float latestSerialTempC = 0.0f;
float latestWiFiTempC = 0.0f;
int latestDisplayedTemp = 0;
unsigned long lastSerialReadingMs = 0;
unsigned long lastWiFiReadingMs = 0;
unsigned long wifiSuccessDisplayUntilMs = 0;
bool waitingForData = true;
wl_status_t previousWiFiStatus = WL_IDLE_STATUS;
bool hasPersistedWiFiCredentials = false;
bool udpListenerRunning = false;
bool hasSerialTemperature = false;
bool hasWiFiTemperature = false;
bool hasWiFiOwnerLock = false;
IPAddress wifiOwnerIp;
unsigned long wifiOwnerLastSeenMs = 0;

enum ActiveTemperatureSource {
    ACTIVE_TEMPERATURE_SOURCE_NONE,
    ACTIVE_TEMPERATURE_SOURCE_SERIAL,
    ACTIVE_TEMPERATURE_SOURCE_WIFI,
};

ActiveTemperatureSource activeTemperatureSource = ACTIVE_TEMPERATURE_SOURCE_NONE;

char *trimWhitespace(char *text) {
    while (*text != '\0' && isspace(static_cast<unsigned char>(*text))) {
        ++text;
    }

    size_t length = strlen(text);
    while (length > 0 && isspace(static_cast<unsigned char>(text[length - 1]))) {
        text[length - 1] = '\0';
        --length;
    }

    return text;
}

bool copyCommandValue(char *destination, size_t destinationSize, const char *value, const char *label) {
    size_t valueLength = strlen(value);
    if (valueLength >= destinationSize) {
        Serial.printf("%s is too long, maximum supported length is %u characters\n",
                      label,
                      static_cast<unsigned>(destinationSize - 1));
        return false;
    }

    snprintf(destination, destinationSize, "%s", value);
    return true;
}

const char *wifiStatusToString(wl_status_t status) {
    switch (status) {
        case WL_NO_SHIELD:
            return "NO_SHIELD";
        case WL_IDLE_STATUS:
            return "IDLE";
        case WL_NO_SSID_AVAIL:
            return "NO_SSID";
        case WL_SCAN_COMPLETED:
            return "SCAN_COMPLETED";
        case WL_CONNECTED:
            return "CONNECTED";
        case WL_CONNECT_FAILED:
            return "CONNECT_FAILED";
        case WL_CONNECTION_LOST:
            return "CONNECTION_LOST";
        case WL_DISCONNECTED:
            return "DISCONNECTED";
        default:
            return "UNKNOWN";
    }
}

void showWaitingPattern() {
    display.setSegments(DASH_SEGMENTS);
}

void showWiFiConnectedPattern() {
    display.setSegments(WIFI_CONNECTED_SEGMENTS);
}

void showIdlePattern() {
    if (WiFi.status() == WL_CONNECTED) {
        showWiFiConnectedPattern();
    } else {
        showWaitingPattern();
    }
}

void refreshDisplay() {
    unsigned long now = millis();
    if (wifiSuccessDisplayUntilMs != 0 && now < wifiSuccessDisplayUntilMs) {
        showWiFiConnectedPattern();
        return;
    }

    wifiSuccessDisplayUntilMs = 0;

    if (waitingForData) {
        showIdlePattern();
        return;
    }

    display.showNumberDec(latestDisplayedTemp, false);
}

void printWiFiHelp() {
    Serial.println("WiFi serial commands:");
    Serial.println("  WIFI SSID <your-ssid>");
    Serial.println("  WIFI PASS <your-password>");
    Serial.println("  WIFI CONNECT");
    Serial.println("  WIFI STATUS");
    Serial.println("  WIFI CLEAR");
    Serial.println("  WIFI HELP");
}

bool openWiFiPreferences(bool readOnly) {
    if (preferences.begin(WIFI_PREFS_NAMESPACE, readOnly)) {
        return true;
    }

    Serial.println("Failed to open WiFi credential storage");
    return false;
}

void saveWiFiCredentialsToFlash() {
    if (wifiSsid[0] == '\0') {
        return;
    }

    if (openWiFiPreferences(true)) {
        String savedSsid = preferences.getString(WIFI_PREFS_SSID_KEY, "");
        String savedPassword = preferences.getString(WIFI_PREFS_PASS_KEY, "");
        preferences.end();

        if (savedSsid.equals(wifiSsid) && savedPassword.equals(wifiPassword)) {
            hasPersistedWiFiCredentials = true;
            return;
        }
    }

    if (!openWiFiPreferences(false)) {
        return;
    }

    preferences.putString(WIFI_PREFS_SSID_KEY, wifiSsid);
    preferences.putString(WIFI_PREFS_PASS_KEY, wifiPassword);
    preferences.end();

    hasPersistedWiFiCredentials = true;
    Serial.println("Saved WiFi credentials to flash");
}

void clearSavedWiFiCredentials() {
    if (!openWiFiPreferences(false)) {
        return;
    }

    preferences.remove(WIFI_PREFS_SSID_KEY);
    preferences.remove(WIFI_PREFS_PASS_KEY);
    preferences.end();

    hasPersistedWiFiCredentials = false;
    Serial.println("Cleared saved WiFi credentials from flash");
}

bool loadSavedWiFiCredentials() {
    if (!openWiFiPreferences(true)) {
        return false;
    }

    String savedSsid = preferences.getString(WIFI_PREFS_SSID_KEY, "");
    String savedPassword = preferences.getString(WIFI_PREFS_PASS_KEY, "");
    preferences.end();

    if (savedSsid.isEmpty()) {
        hasPersistedWiFiCredentials = false;
        return false;
    }

    if (savedSsid.length() >= sizeof(wifiSsid) || savedPassword.length() >= sizeof(wifiPassword)) {
        Serial.println("Saved WiFi credentials are too long and were ignored");
        hasPersistedWiFiCredentials = false;
        return false;
    }

    snprintf(wifiSsid, sizeof(wifiSsid), "%s", savedSsid.c_str());
    snprintf(wifiPassword, sizeof(wifiPassword), "%s", savedPassword.c_str());
    hasPersistedWiFiCredentials = true;
    return true;
}

void printWiFiStatus() {
    unsigned long now = millis();
    wl_status_t status = WiFi.status();
    Serial.printf("WiFi status: %s\n", wifiStatusToString(status));
    Serial.printf("Stored SSID: %s\n", wifiSsid[0] == '\0' ? "<empty>" : wifiSsid);
    Serial.printf("Stored password: %s\n", wifiPassword[0] == '\0' ? "<empty>" : "<set>");
    Serial.printf("Saved to flash: %s\n", hasPersistedWiFiCredentials ? "yes" : "no");
    Serial.printf("WiFi UDP port: %u\n", WIFI_UDP_PORT);
    if (hasWiFiOwnerLock && now - wifiOwnerLastSeenMs <= DATA_TIMEOUT_MS) {
        Serial.printf("WiFi sender lock: %s (expires in %lu ms)\n",
                      wifiOwnerIp.toString().c_str(),
                      DATA_TIMEOUT_MS - (now - wifiOwnerLastSeenMs));
    } else {
        Serial.println("WiFi sender lock: <unlocked>");
    }

    if (status == WL_CONNECTED) {
        String ipAddress = WiFi.localIP().toString();
        Serial.printf("WiFi IP: %s\n", ipAddress.c_str());
    }
}

void clearWiFiOwnerLock(const char *reason) {
    if (!hasWiFiOwnerLock) {
        return;
    }

    Serial.printf("Released WiFi sender lock for %s (%s)\n",
                  wifiOwnerIp.toString().c_str(),
                  reason);
    hasWiFiOwnerLock = false;
    wifiOwnerIp = IPAddress();
    wifiOwnerLastSeenMs = 0;
}

void refreshWiFiOwnerLock() {
    if (!hasWiFiOwnerLock) {
        return;
    }

    if (millis() - wifiOwnerLastSeenMs > DATA_TIMEOUT_MS) {
        clearWiFiOwnerLock("timeout");
    }
}

void updateDisplayedTemperatureSource() {
    unsigned long now = millis();
    bool serialIsFresh = hasSerialTemperature && now - lastSerialReadingMs <= DATA_TIMEOUT_MS;
    bool wifiIsFresh = hasWiFiTemperature && now - lastWiFiReadingMs <= DATA_TIMEOUT_MS;

    ActiveTemperatureSource nextSource = ACTIVE_TEMPERATURE_SOURCE_NONE;
    if (serialIsFresh) {
        nextSource = ACTIVE_TEMPERATURE_SOURCE_SERIAL;
    } else if (wifiIsFresh) {
        nextSource = ACTIVE_TEMPERATURE_SOURCE_WIFI;
    }

    if (nextSource != activeTemperatureSource) {
        if (nextSource == ACTIVE_TEMPERATURE_SOURCE_SERIAL) {
            Serial.println("Display source switched to serial");
        } else if (nextSource == ACTIVE_TEMPERATURE_SOURCE_WIFI) {
            Serial.println("Display source switched to WiFi");
        } else if (activeTemperatureSource != ACTIVE_TEMPERATURE_SOURCE_NONE) {
            Serial.println("No host temperature updates received recently");
        }
    }

    activeTemperatureSource = nextSource;
    waitingForData = activeTemperatureSource == ACTIVE_TEMPERATURE_SOURCE_NONE;

    if (activeTemperatureSource == ACTIVE_TEMPERATURE_SOURCE_SERIAL) {
        latestDisplayedTemp = static_cast<int>(lroundf(latestSerialTempC));
    } else if (activeTemperatureSource == ACTIVE_TEMPERATURE_SOURCE_WIFI) {
        latestDisplayedTemp = static_cast<int>(lroundf(latestWiFiTempC));
    }
}

void applyReceivedTemperature(float tempC, bool fromSerial, const char *sourceLabel) {
    int displayedTemp = static_cast<int>(lroundf(tempC));
    unsigned long now = millis();

    if (fromSerial) {
        latestSerialTempC = tempC;
        lastSerialReadingMs = now;
        hasSerialTemperature = true;
    } else {
        latestWiFiTempC = tempC;
        lastWiFiReadingMs = now;
        hasWiFiTemperature = true;
    }

    Serial.printf("%s Temperature: %.2f C (rounded: %d)\n",
                  sourceLabel,
                  tempC,
                  displayedTemp);

    updateDisplayedTemperatureSource();
}

void startUdpListener() {
    if (udpListenerRunning) {
        return;
    }

    if (udpReceiver.begin(WIFI_UDP_PORT) == 1) {
        udpListenerRunning = true;
        Serial.printf("WiFi UDP listener started on port %u\n", WIFI_UDP_PORT);
        return;
    }

    Serial.printf("Failed to start WiFi UDP listener on port %u\n", WIFI_UDP_PORT);
}

void stopUdpListener() {
    if (!udpListenerRunning) {
        return;
    }

    udpReceiver.stop();
    udpListenerRunning = false;
    clearWiFiOwnerLock("listener stopped");
    Serial.println("WiFi UDP listener stopped");
}

void beginWiFiConnection() {
    if (wifiSsid[0] == '\0') {
        Serial.println("Cannot connect WiFi: SSID is empty");
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(100);

    Serial.printf("Connecting to WiFi SSID: %s\n", wifiSsid);
    WiFi.begin(wifiSsid, wifiPassword);
}

void clearWiFiCredentials() {
    wifiSsid[0] = '\0';
    wifiPassword[0] = '\0';
    wifiSuccessDisplayUntilMs = 0;
    stopUdpListener();
    WiFi.disconnect(false, false);
    clearSavedWiFiCredentials();
    Serial.println("Cleared WiFi credentials and disconnected WiFi");
}

void processWiFiCommand(char *commandText) {
    char *command = trimWhitespace(commandText);

    if (*command == '\0' || strcmp(command, "HELP") == 0) {
        printWiFiHelp();
        return;
    }

    if (strncmp(command, "SSID ", 5) == 0) {
        char *value = trimWhitespace(command + 5);
        if (*value == '\0') {
            Serial.println("WIFI SSID requires a value");
            return;
        }

        if (copyCommandValue(wifiSsid, sizeof(wifiSsid), value, "WiFi SSID")) {
            Serial.printf("Stored WiFi SSID: %s\n", wifiSsid);
        }
        return;
    }

    if (strncmp(command, "PASS ", 5) == 0) {
        char *value = trimWhitespace(command + 5);
        if (*value == '\0') {
            Serial.println("WIFI PASS requires a value");
            return;
        }

        if (copyCommandValue(wifiPassword, sizeof(wifiPassword), value, "WiFi password")) {
            Serial.printf("Stored WiFi password (%u characters)\n", static_cast<unsigned>(strlen(wifiPassword)));
        }
        return;
    }

    if (strcmp(command, "CONNECT") == 0) {
        beginWiFiConnection();
        return;
    }

    if (strcmp(command, "STATUS") == 0) {
        printWiFiStatus();
        return;
    }

    if (strcmp(command, "CLEAR") == 0) {
        clearWiFiCredentials();
        return;
    }

    Serial.printf("Unknown WiFi command: %s\n", command);
    printWiFiHelp();
}

bool parseTemperatureLine(const char *line, float *tempC) {
    char *endPtr = nullptr;
    float parsedValue = strtof(line, &endPtr);
    if (endPtr == line || *endPtr != '\0') {
        Serial.printf("Ignoring invalid serial input: %s\n", line);
        return false;
    }

    *tempC = parsedValue;
    return true;
}

bool processSerialLine(float *tempC) {
    serialBuffer[serialLength] = '\0';
    serialLength = 0;

    char *line = trimWhitespace(serialBuffer);
    if (*line == '\0') {
        return false;
    }

    if (strcmp(line, "WIFI") == 0) {
        printWiFiHelp();
        return false;
    }

    if (strncmp(line, "WIFI ", 5) == 0) {
        processWiFiCommand(line + 5);
        return false;
    }

    return parseTemperatureLine(line, tempC);
}

bool readTemperatureFromSerial(float *tempC) {
    while (Serial.available() > 0) {
        char ch = static_cast<char>(Serial.read());

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            return processSerialLine(tempC);
        }

        if (serialLength < SERIAL_BUFFER_SIZE - 1) {
            serialBuffer[serialLength++] = ch;
        } else {
            serialLength = 0;
            Serial.println("Discarded overlong serial input");
        }
    }

    return false;
}

bool readTemperatureFromWiFi(float *tempC) {
    if (!udpListenerRunning) {
        return false;
    }

    refreshWiFiOwnerLock();

    int packetSize = udpReceiver.parsePacket();
    if (packetSize <= 0) {
        return false;
    }

    IPAddress remoteIp = udpReceiver.remoteIP();
    uint16_t remotePort = udpReceiver.remotePort();

    if (hasWiFiOwnerLock && remoteIp != wifiOwnerIp) {
        while (udpReceiver.available() > 0) {
            udpReceiver.read();
        }

        Serial.printf("Ignoring WiFi temperature from %s:%u because sender lock is held by %s\n",
                      remoteIp.toString().c_str(),
                      remotePort,
                      wifiOwnerIp.toString().c_str());
        return false;
    }

    int bytesToRead = packetSize;
    if (bytesToRead >= static_cast<int>(SERIAL_BUFFER_SIZE)) {
        bytesToRead = static_cast<int>(SERIAL_BUFFER_SIZE) - 1;
    }

    int bytesRead = udpReceiver.read(serialBuffer, bytesToRead);
    serialBuffer[bytesRead] = '\0';

    while (udpReceiver.available() > 0) {
        udpReceiver.read();
    }

    char *line = trimWhitespace(serialBuffer);
    if (*line == '\0') {
        return false;
    }

    if (!parseTemperatureLine(line, tempC)) {
        Serial.printf("Ignoring invalid WiFi payload from %s:%u\n",
                      remoteIp.toString().c_str(),
                      remotePort);
        return false;
    }

    if (!hasWiFiOwnerLock) {
        wifiOwnerIp = remoteIp;
        hasWiFiOwnerLock = true;
        Serial.printf("Acquired WiFi sender lock for %s\n", wifiOwnerIp.toString().c_str());
    }

    wifiOwnerLastSeenMs = millis();

    return true;
}

void handleWiFiStatusChanges() {
    wl_status_t currentStatus = WiFi.status();
    if (currentStatus == previousWiFiStatus) {
        return;
    }

    wl_status_t oldStatus = previousWiFiStatus;
    previousWiFiStatus = currentStatus;

    Serial.printf("WiFi state changed: %s -> %s\n",
                  wifiStatusToString(oldStatus),
                  wifiStatusToString(currentStatus));

    if (currentStatus != WL_CONNECTED && oldStatus == WL_CONNECTED) {
        stopUdpListener();
    }

    if (currentStatus == WL_CONNECTED) {
        String ipAddress = WiFi.localIP().toString();
        Serial.printf("WiFi connected successfully, IP: %s\n", ipAddress.c_str());
        if (wifiSsid[0] != '\0') {
            saveWiFiCredentialsToFlash();
        }
        startUdpListener();
        wifiSuccessDisplayUntilMs = millis() + WIFI_SUCCESS_DISPLAY_MS;
        return;
    }

    if (currentStatus == WL_CONNECT_FAILED) {
        Serial.println("WiFi connection failed, please verify SSID and password");
        return;
    }

    if (currentStatus == WL_NO_SSID_AVAIL) {
        Serial.println("WiFi SSID not found");
        return;
    }

    if (currentStatus == WL_CONNECTION_LOST) {
        Serial.println("WiFi connection lost");
        return;
    }

    if (currentStatus == WL_DISCONNECTED && oldStatus == WL_CONNECTED) {
        Serial.println("WiFi disconnected");
    }
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    Serial.println("System Initializing...");
    Serial.println("Waiting for host CPU temperature readings over serial...");
    Serial.println("Use WIFI HELP over serial to configure WiFi credentials.");

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    if (loadSavedWiFiCredentials()) {
        Serial.printf("Loaded saved WiFi SSID from flash: %s\n", wifiSsid);
        beginWiFiConnection();
    } else {
        Serial.println("No saved WiFi credentials found in flash");
    }

    previousWiFiStatus = WiFi.status();

    // Some TM1637 boards ignore the first commands after a cold power-on.
    delay(DISPLAY_STARTUP_DELAY_MS);
    display.setBrightness(0x0f);
    refreshDisplay();
}

void loop() {
    float receivedTempC;
    if (readTemperatureFromSerial(&receivedTempC)) {
        applyReceivedTemperature(receivedTempC, true, "Host CPU Serial");
    }

    if (readTemperatureFromWiFi(&receivedTempC)) {
        applyReceivedTemperature(receivedTempC, false, "Host CPU WiFi");
    }

    handleWiFiStatusChanges();
    refreshWiFiOwnerLock();
    updateDisplayedTemperatureSource();

    refreshDisplay();
    delay(50);
}
