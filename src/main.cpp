#include <Arduino.h>
#include <TM1637Display.h>
#include <math.h>
#include <stdlib.h>

// 定义连接数码管的引脚
#define CLK 11
#define DIO 12

constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long DATA_TIMEOUT_MS = 5000;
constexpr size_t SERIAL_BUFFER_SIZE = 32;
const uint8_t DASH_SEGMENTS[] = {0x40, 0x40, 0x40, 0x40};

// 初始化显示对象
TM1637Display display(CLK, DIO);
char serialBuffer[SERIAL_BUFFER_SIZE];
size_t serialLength = 0;
float latestTempC = 0.0f;
unsigned long lastReadingMs = 0;
bool waitingForData = true;

void showWaitingPattern() {
    display.setSegments(DASH_SEGMENTS);
}

bool readTemperatureFromSerial(float *tempC) {
    while (Serial.available() > 0) {
        char ch = static_cast<char>(Serial.read());

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            serialBuffer[serialLength] = '\0';
            serialLength = 0;

            if (serialBuffer[0] == '\0') {
                return false;
            }

            char *endPtr = nullptr;
            float parsedValue = strtof(serialBuffer, &endPtr);
            if (endPtr == serialBuffer || *endPtr != '\0') {
                Serial.printf("Ignoring invalid host temperature: %s\n", serialBuffer);
                return false;
            }

            *tempC = parsedValue;
            return true;
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

void setup() {
    Serial.begin(SERIAL_BAUD);
    Serial.println("System Initializing...");
    Serial.println("Waiting for host CPU temperature readings over serial...");

    display.setBrightness(0x0f);
    showWaitingPattern();
}

void loop() {
    float receivedTempC;
    if (readTemperatureFromSerial(&receivedTempC)) {
        latestTempC = receivedTempC;
        lastReadingMs = millis();
        waitingForData = false;

        int displayedTemp = static_cast<int>(lroundf(latestTempC));
        display.showNumberDec(displayedTemp, false);
        Serial.printf("Host CPU Temperature: %.2f C (displaying: %d)\n", latestTempC, displayedTemp);
    }

    if (!waitingForData && millis() - lastReadingMs > DATA_TIMEOUT_MS) {
        waitingForData = true;
        showWaitingPattern();
        Serial.println("No host temperature updates received recently");
    }

    delay(50);
}
