#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Preferences.h>
#include <TM1637Display.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>

// 定义连接数码管的引脚
#define CLK 11
#define DIO 12

constexpr uint8_t BOOT_BUTTON_PIN = 0;
constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long DATA_TIMEOUT_MS = 5000;
constexpr unsigned long DISPLAY_STARTUP_DELAY_MS = 250;
constexpr unsigned long WIFI_SUCCESS_DISPLAY_MS = 1500;
constexpr unsigned long BLUETOOTH_PAIRING_DISPLAY_MS = 1500;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 50;
constexpr size_t SERIAL_BUFFER_SIZE = 192;
constexpr size_t WIFI_FIELD_BUFFER_SIZE = 65;
constexpr unsigned int WIFI_UDP_PORT = 4210;
constexpr char WIFI_PREFS_NAMESPACE[] = "wifi";
constexpr char WIFI_PREFS_SSID_KEY[] = "ssid";
constexpr char WIFI_PREFS_PASS_KEY[] = "pass";
constexpr char BLE_DEVICE_NAME[] = "HWTempDisplay";
constexpr char BLE_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_CHARACTERISTIC_UUID_RX[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";

const uint8_t DASH_SEGMENTS[] = {SEG_G, SEG_G, SEG_G, SEG_G};
const uint8_t WIFI_CONNECTED_SEGMENTS[] = {
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G),
};
const uint8_t BLUETOOTH_DISCONNECTED_SEGMENTS[] = {
    static_cast<uint8_t>(SEG_B | SEG_C | SEG_D | SEG_E | SEG_G),
    static_cast<uint8_t>(SEG_B | SEG_C | SEG_D | SEG_E | SEG_G),
    static_cast<uint8_t>(SEG_B | SEG_C | SEG_D | SEG_E | SEG_G),
    static_cast<uint8_t>(SEG_B | SEG_C | SEG_D | SEG_E | SEG_G),
};
const uint8_t BLUETOOTH_PAIRING_SEGMENTS[] = {
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_A | SEG_B | SEG_E | SEG_F | SEG_G),
};
const uint8_t BLUETOOTH_CONNECTED_SEGMENTS[] = {
    static_cast<uint8_t>(SEG_C | SEG_D | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_C | SEG_D | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_C | SEG_D | SEG_E | SEG_F | SEG_G),
    static_cast<uint8_t>(SEG_C | SEG_D | SEG_E | SEG_F | SEG_G),
};

enum WirelessMode {
    WIRELESS_MODE_WIFI,
    WIRELESS_MODE_BLUETOOTH,
};

enum ActiveTemperatureSource {
    ACTIVE_TEMPERATURE_SOURCE_NONE,
    ACTIVE_TEMPERATURE_SOURCE_SERIAL,
    ACTIVE_TEMPERATURE_SOURCE_WIFI,
    ACTIVE_TEMPERATURE_SOURCE_BLUETOOTH,
};

enum TemperatureTransport {
    TEMPERATURE_TRANSPORT_SERIAL,
    TEMPERATURE_TRANSPORT_WIFI,
    TEMPERATURE_TRANSPORT_BLUETOOTH,
};

TM1637Display display(CLK, DIO);
Preferences preferences;
WiFiUDP udpReceiver;
BLEServer *bleServer = nullptr;
BLECharacteristic *bleRxCharacteristic = nullptr;
char serialBuffer[SERIAL_BUFFER_SIZE];
size_t serialLength = 0;
char wifiSsid[WIFI_FIELD_BUFFER_SIZE] = "";
char wifiPassword[WIFI_FIELD_BUFFER_SIZE] = "";
float latestSerialTempC = 0.0f;
float latestWiFiTempC = 0.0f;
float latestBluetoothTempC = 0.0f;
float pendingBluetoothTempC = 0.0f;
int latestDisplayedTemp = 0;
unsigned long lastSerialReadingMs = 0;
unsigned long lastWiFiReadingMs = 0;
unsigned long lastBluetoothReadingMs = 0;
unsigned long wifiSuccessDisplayUntilMs = 0;
unsigned long bluetoothPairingDisplayUntilMs = 0;
unsigned long bootButtonLastChangeMs = 0;
bool waitingForData = true;
wl_status_t previousWiFiStatus = WL_IDLE_STATUS;
bool hasPersistedWiFiCredentials = false;
bool udpListenerRunning = false;
bool hasSerialTemperature = false;
bool hasWiFiTemperature = false;
bool hasBluetoothTemperature = false;
bool hasWiFiOwnerLock = false;
bool bleInitialized = false;
bool bluetoothClientConnected = false;
bool bluetoothAdvertising = false;
bool bluetoothShouldRestartAdvertising = false;
bool hasPendingBluetoothTemperature = false;
bool bootButtonLastReading = true;
bool bootButtonStableState = true;
IPAddress wifiOwnerIp;
unsigned long wifiOwnerLastSeenMs = 0;
portMUX_TYPE bluetoothTempMux = portMUX_INITIALIZER_UNLOCKED;
WirelessMode wirelessMode = WIRELESS_MODE_WIFI;
ActiveTemperatureSource activeTemperatureSource = ACTIVE_TEMPERATURE_SOURCE_NONE;

char *trimWhitespace(char *text);
bool parseTemperatureLine(const char *line, float *tempC);
void updateDisplayedTemperatureSource();
void startBluetoothAdvertising(const char *reason, bool showPairingPattern);

class BluetoothServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override {
        bluetoothClientConnected = true;
        bluetoothAdvertising = false;
        bluetoothPairingDisplayUntilMs = 0;
        Serial.println("Bluetooth client connected");
        updateDisplayedTemperatureSource();
    }

    void onDisconnect(BLEServer *server) override {
        bluetoothClientConnected = false;
        Serial.println("Bluetooth client disconnected");

        if (bluetoothShouldRestartAdvertising) {
            startBluetoothAdvertising("client disconnected", true);
        } else {
            bluetoothAdvertising = false;
            bluetoothPairingDisplayUntilMs = 0;
        }

        updateDisplayedTemperatureSource();
    }
};

class BluetoothTemperatureCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        std::string value = characteristic->getValue();
        if (value.empty()) {
            return;
        }

        if (value.length() >= SERIAL_BUFFER_SIZE) {
            Serial.println("Discarded overlong Bluetooth payload");
            return;
        }

        char buffer[SERIAL_BUFFER_SIZE];
        memcpy(buffer, value.data(), value.length());
        buffer[value.length()] = '\0';

        char *line = trimWhitespace(buffer);
        if (*line == '\0') {
            return;
        }

        float parsedTempC = 0.0f;
        if (!parseTemperatureLine(line, &parsedTempC)) {
            Serial.printf("Ignoring invalid Bluetooth payload: %s\n", line);
            return;
        }

        portENTER_CRITICAL(&bluetoothTempMux);
        pendingBluetoothTempC = parsedTempC;
        hasPendingBluetoothTemperature = true;
        portEXIT_CRITICAL(&bluetoothTempMux);
    }
};

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

const char *wirelessModeToString(WirelessMode mode) {
    switch (mode) {
        case WIRELESS_MODE_WIFI:
            return "WiFi";
        case WIRELESS_MODE_BLUETOOTH:
            return "Bluetooth";
        default:
            return "Unknown";
    }
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

void showBluetoothDisconnectedPattern() {
    display.setSegments(BLUETOOTH_DISCONNECTED_SEGMENTS);
}

void showBluetoothPairingPattern() {
    display.setSegments(BLUETOOTH_PAIRING_SEGMENTS);
}

void showBluetoothConnectedPattern() {
    display.setSegments(BLUETOOTH_CONNECTED_SEGMENTS);
}

bool isBluetoothPairingPatternActive() {
    if (wirelessMode != WIRELESS_MODE_BLUETOOTH) {
        return false;
    }

    if (!bluetoothAdvertising || bluetoothClientConnected) {
        return false;
    }

    if (bluetoothPairingDisplayUntilMs == 0) {
        return false;
    }

    return millis() < bluetoothPairingDisplayUntilMs;
}

void showIdlePattern() {
    if (wirelessMode == WIRELESS_MODE_BLUETOOTH) {
        if (isBluetoothPairingPatternActive()) {
            showBluetoothPairingPattern();
        } else if (bluetoothClientConnected) {
            showBluetoothConnectedPattern();
        } else {
            showBluetoothDisconnectedPattern();
        }
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        showWiFiConnectedPattern();
    } else {
        showWaitingPattern();
    }
}

void refreshDisplay() {
    unsigned long now = millis();
    if (wirelessMode == WIRELESS_MODE_WIFI && wifiSuccessDisplayUntilMs != 0 && now < wifiSuccessDisplayUntilMs) {
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
    Serial.println("  WIFI LOCK CLEAR");
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
    Serial.printf("Wireless mode: %s\n", wirelessModeToString(wirelessMode));
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
    bool bluetoothIsFresh = hasBluetoothTemperature && now - lastBluetoothReadingMs <= DATA_TIMEOUT_MS;

    ActiveTemperatureSource nextSource = ACTIVE_TEMPERATURE_SOURCE_NONE;
    if (serialIsFresh) {
        nextSource = ACTIVE_TEMPERATURE_SOURCE_SERIAL;
    } else if (wirelessMode == WIRELESS_MODE_WIFI && wifiIsFresh) {
        nextSource = ACTIVE_TEMPERATURE_SOURCE_WIFI;
    } else if (wirelessMode == WIRELESS_MODE_BLUETOOTH && bluetoothIsFresh) {
        nextSource = ACTIVE_TEMPERATURE_SOURCE_BLUETOOTH;
    }

    if (nextSource != activeTemperatureSource) {
        if (nextSource == ACTIVE_TEMPERATURE_SOURCE_SERIAL) {
            Serial.println("Display source switched to serial");
        } else if (nextSource == ACTIVE_TEMPERATURE_SOURCE_WIFI) {
            Serial.println("Display source switched to WiFi");
        } else if (nextSource == ACTIVE_TEMPERATURE_SOURCE_BLUETOOTH) {
            Serial.println("Display source switched to Bluetooth");
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
    } else if (activeTemperatureSource == ACTIVE_TEMPERATURE_SOURCE_BLUETOOTH) {
        latestDisplayedTemp = static_cast<int>(lroundf(latestBluetoothTempC));
    }
}

void applyReceivedTemperature(float tempC, TemperatureTransport transport, const char *sourceLabel) {
    int displayedTemp = static_cast<int>(lroundf(tempC));
    unsigned long now = millis();

    if (transport == TEMPERATURE_TRANSPORT_SERIAL) {
        latestSerialTempC = tempC;
        lastSerialReadingMs = now;
        hasSerialTemperature = true;
    } else if (transport == TEMPERATURE_TRANSPORT_WIFI) {
        latestWiFiTempC = tempC;
        lastWiFiReadingMs = now;
        hasWiFiTemperature = true;
    } else {
        latestBluetoothTempC = tempC;
        lastBluetoothReadingMs = now;
        hasBluetoothTemperature = true;
    }

    Serial.printf("%s Temperature: %.2f C (rounded: %d)\n",
                  sourceLabel,
                  tempC,
                  displayedTemp);

    updateDisplayedTemperatureSource();
}

void startUdpListener() {
    if (wirelessMode != WIRELESS_MODE_WIFI || udpListenerRunning || WiFi.status() != WL_CONNECTED) {
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

void stopWiFiMode() {
    wifiSuccessDisplayUntilMs = 0;
    stopUdpListener();
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    previousWiFiStatus = WiFi.status();
}

void beginWiFiConnection() {
    if (wirelessMode != WIRELESS_MODE_WIFI) {
        Serial.println("Cannot connect WiFi while Bluetooth mode is active. Press BOOT to switch back to WiFi mode.");
        return;
    }

    if (wifiSsid[0] == '\0') {
        Serial.println("Cannot connect WiFi: SSID is empty");
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.disconnect(false, false);
    delay(100);

    Serial.printf("Connecting to WiFi SSID: %s\n", wifiSsid);
    WiFi.begin(wifiSsid, wifiPassword);
    previousWiFiStatus = WiFi.status();
}

void activateWiFiMode(bool autoConnectSavedCredentials) {
    wirelessMode = WIRELESS_MODE_WIFI;
    bluetoothPairingDisplayUntilMs = 0;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    previousWiFiStatus = WiFi.status();

    if (autoConnectSavedCredentials && wifiSsid[0] != '\0') {
        beginWiFiConnection();
    }

    updateDisplayedTemperatureSource();
    refreshDisplay();
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

bool ensureBluetoothInitialized() {
    if (bleInitialized) {
        return true;
    }

    Serial.printf("Starting Bluetooth BLE server: %s\n", BLE_DEVICE_NAME);
    BLEDevice::init(BLE_DEVICE_NAME);
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new BluetoothServerCallbacks());

    BLEService *service = bleServer->createService(BLE_SERVICE_UUID);
    bleRxCharacteristic = service->createCharacteristic(
        BLE_CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    bleRxCharacteristic->setCallbacks(new BluetoothTemperatureCallbacks());
    service->start();

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setScanResponse(false);
    advertising->setMinPreferred(0x06);
    advertising->setMinPreferred(0x12);

    bleInitialized = true;
    return true;
}

void startBluetoothAdvertising(const char *reason, bool showPairingPattern) {
    if (!bleInitialized || bleServer == nullptr) {
        return;
    }

    bleServer->startAdvertising();
    bluetoothAdvertising = true;
    if (showPairingPattern) {
        bluetoothPairingDisplayUntilMs = millis() + BLUETOOTH_PAIRING_DISPLAY_MS;
    } else {
        bluetoothPairingDisplayUntilMs = 0;
    }

    Serial.printf("Bluetooth advertising started (%s)\n", reason);
}

void stopBluetoothMode() {
    portENTER_CRITICAL(&bluetoothTempMux);
    hasPendingBluetoothTemperature = false;
    pendingBluetoothTempC = 0.0f;
    portEXIT_CRITICAL(&bluetoothTempMux);

    if (!bleInitialized) {
        bluetoothClientConnected = false;
        bluetoothAdvertising = false;
        bluetoothPairingDisplayUntilMs = 0;
        bluetoothShouldRestartAdvertising = false;
        return;
    }

    bluetoothShouldRestartAdvertising = false;
    bluetoothClientConnected = false;
    bluetoothAdvertising = false;
    bluetoothPairingDisplayUntilMs = 0;

    BLEDevice::deinit(false);
    bleServer = nullptr;
    bleRxCharacteristic = nullptr;
    bleInitialized = false;
    Serial.println("Bluetooth BLE stopped");
}

void activateBluetoothMode() {
    wirelessMode = WIRELESS_MODE_BLUETOOTH;
    wifiSuccessDisplayUntilMs = 0;
    stopWiFiMode();

    if (!ensureBluetoothInitialized()) {
        Serial.println("Failed to initialize Bluetooth BLE");
        return;
    }

    bluetoothShouldRestartAdvertising = true;
    startBluetoothAdvertising("Bluetooth mode active", true);
    updateDisplayedTemperatureSource();
    refreshDisplay();
}

void toggleWirelessMode() {
    if (wirelessMode == WIRELESS_MODE_WIFI) {
        Serial.println("BOOT pressed, switching wireless mode to Bluetooth");
        activateBluetoothMode();
        return;
    }

    Serial.println("BOOT pressed, switching wireless mode to WiFi");
    stopBluetoothMode();
    activateWiFiMode(true);
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

    if (strcmp(command, "LOCK CLEAR") == 0) {
        if (hasWiFiOwnerLock) {
            clearWiFiOwnerLock("manual clear");
        } else {
            Serial.println("WiFi sender lock is already clear");
        }
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

    if (!parseTemperatureLine(line, tempC)) {
        Serial.printf("Ignoring invalid serial input: %s\n", line);
        return false;
    }

    return true;
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
    if (wirelessMode != WIRELESS_MODE_WIFI || !udpListenerRunning) {
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

bool consumePendingBluetoothTemperature(float *tempC) {
    bool hasPending = false;

    portENTER_CRITICAL(&bluetoothTempMux);
    if (hasPendingBluetoothTemperature) {
        *tempC = pendingBluetoothTempC;
        hasPendingBluetoothTemperature = false;
        hasPending = true;
    }
    portEXIT_CRITICAL(&bluetoothTempMux);

    return hasPending;
}

void handleWiFiStatusChanges() {
    if (wirelessMode != WIRELESS_MODE_WIFI) {
        return;
    }

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

void handleBootButton() {
    bool reading = digitalRead(BOOT_BUTTON_PIN) == HIGH;
    if (reading != bootButtonLastReading) {
        bootButtonLastReading = reading;
        bootButtonLastChangeMs = millis();
    }

    if (millis() - bootButtonLastChangeMs < BUTTON_DEBOUNCE_MS) {
        return;
    }

    if (reading != bootButtonStableState) {
        bootButtonStableState = reading;
        if (!bootButtonStableState) {
            toggleWirelessMode();
        }
    }
}

void setup() {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    bootButtonLastReading = digitalRead(BOOT_BUTTON_PIN) == HIGH;
    bootButtonStableState = bootButtonLastReading;
    bootButtonLastChangeMs = millis();

    Serial.begin(SERIAL_BAUD);
    Serial.println("System Initializing...");
    Serial.println("Waiting for host CPU temperature readings over serial...");
    Serial.println("Use WIFI HELP over serial to configure WiFi credentials.");
    Serial.println("Press BOOT to switch between WiFi mode and Bluetooth mode.");

    if (loadSavedWiFiCredentials()) {
        Serial.printf("Loaded saved WiFi SSID from flash: %s\n", wifiSsid);
    } else {
        Serial.println("No saved WiFi credentials found in flash");
    }

    activateWiFiMode(true);

    // Some TM1637 boards ignore the first commands after a cold power-on.
    delay(DISPLAY_STARTUP_DELAY_MS);
    display.setBrightness(0x0f);
    refreshDisplay();
}

void loop() {
    handleBootButton();

    float receivedTempC = 0.0f;
    if (readTemperatureFromSerial(&receivedTempC)) {
        applyReceivedTemperature(receivedTempC, TEMPERATURE_TRANSPORT_SERIAL, "Host CPU Serial");
    }

    if (readTemperatureFromWiFi(&receivedTempC)) {
        applyReceivedTemperature(receivedTempC, TEMPERATURE_TRANSPORT_WIFI, "Host CPU WiFi");
    }

    if (consumePendingBluetoothTemperature(&receivedTempC)) {
        applyReceivedTemperature(receivedTempC, TEMPERATURE_TRANSPORT_BLUETOOTH, "Host CPU Bluetooth");
    }

    handleWiFiStatusChanges();
    refreshWiFiOwnerLock();
    updateDisplayedTemperatureSource();
    refreshDisplay();
    delay(50);
}
