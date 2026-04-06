#include <Arduino.h>
#include <TM1637Display.h>
#include <WiFi.h>

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

const uint8_t DASH_SEGMENTS[] = {SEG_G, SEG_G, SEG_G, SEG_G};
const uint8_t WIFI_CONNECTED_SEGMENTS[] = {
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
};

// 初始化显示对象
TM1637Display display(CLK, DIO);
char serialBuffer[SERIAL_BUFFER_SIZE];
size_t serialLength = 0;
char wifiSsid[WIFI_FIELD_BUFFER_SIZE] = "";
char wifiPassword[WIFI_FIELD_BUFFER_SIZE] = "";
float latestTempC = 0.0f;
int latestDisplayedTemp = 0;
unsigned long lastReadingMs = 0;
unsigned long wifiSuccessDisplayUntilMs = 0;
bool waitingForData = true;
wl_status_t previousWiFiStatus = WL_IDLE_STATUS;

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

void printWiFiStatus() {
    wl_status_t status = WiFi.status();
    Serial.printf("WiFi status: %s\n", wifiStatusToString(status));
    Serial.printf("Stored SSID: %s\n", wifiSsid[0] == '\0' ? "<empty>" : wifiSsid);
    Serial.printf("Stored password: %s\n", wifiPassword[0] == '\0' ? "<empty>" : "<set>");

    if (status == WL_CONNECTED) {
        String ipAddress = WiFi.localIP().toString();
        Serial.printf("WiFi IP: %s\n", ipAddress.c_str());
    }
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
    WiFi.disconnect(false, false);
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

    if (currentStatus == WL_CONNECTED) {
        String ipAddress = WiFi.localIP().toString();
        Serial.printf("WiFi connected successfully, IP: %s\n", ipAddress.c_str());
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
    previousWiFiStatus = WiFi.status();

    // Some TM1637 boards ignore the first commands after a cold power-on.
    delay(DISPLAY_STARTUP_DELAY_MS);
    display.setBrightness(0x0f);
    refreshDisplay();
}

void loop() {
    float receivedTempC;
    if (readTemperatureFromSerial(&receivedTempC)) {
        latestTempC = receivedTempC;
        latestDisplayedTemp = static_cast<int>(lroundf(latestTempC));
        lastReadingMs = millis();
        waitingForData = false;

        Serial.printf("Host CPU Temperature: %.2f C (displaying: %d)\n", latestTempC, latestDisplayedTemp);
    }

    handleWiFiStatusChanges();

    if (!waitingForData && millis() - lastReadingMs > DATA_TIMEOUT_MS) {
        waitingForData = true;
        Serial.println("No host temperature updates received recently");
    }

    refreshDisplay();
    delay(50);
}
