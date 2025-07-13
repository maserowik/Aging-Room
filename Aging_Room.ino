#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <SD.h>

// --- NTP & Ethernet config ---
byte mac[] = { 0xA8, 0x61, 0x0A, 0xAE, 0x30, 0x21 };
EthernetUDP Udp;
unsigned int localPort = 5203;
const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];

unsigned long currentEpoch = 0;  // Last synced NTP epoch time (timezone adjusted)
unsigned long lastNtpCheck = 0;
const unsigned long ntpInterval = 86400000;  // 24 hours in milliseconds

// --- DHT22 and LCD setup ---
#define DHTTYPE DHT22
DHT dhtA(2, DHTTYPE), dhtB(3, DHTTYPE), dhtC(5, DHTTYPE), dhtD(6, DHTTYPE);

LiquidCrystal_I2C lcd(0x3F, 20, 4);
#define RED_LED_PIN 8
#define GREEN_LED_PIN 7
#define BUTTON_PIN 13

float tempThreshold = 22.0; // Default threshold
const float thresholdMargin = 3.0;
const unsigned long blinkIntervalNormal = 500;
const unsigned long blinkIntervalFast = 250;
const unsigned long sensorReadInterval = 2000;

unsigned long lastDisplaySwitch = 0;
unsigned long lastBlinkToggle = 0;
unsigned long lastSensorRead = 0;

int displayMode = 0;
bool blinkState = false;

float tA = NAN, tB = NAN, tC = NAN, tD = NAN;
float hA = NAN, hB = NAN, hC = NAN, hD = NAN;

// --- SD card ---
const int chipSelect = 4;  // change if needed
const unsigned long csvWriteInterval = 30000;  // Write CSV every 30 seconds
unsigned long lastCsvWrite = 0;

// --- NTP helper functions ---
void sendNTPpacket(IPAddress& address) {
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;
  packetBuffer[1] = 0;
  packetBuffer[2] = 6;
  packetBuffer[3] = 0xEC;
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  Udp.beginPacket(address, 123);
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

bool isLeapYear(int year) {
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

void epochToDateTime(unsigned long epoch, int &year, int &month, int &day, int &hour, int &minute, int &second, int &weekday) {
  unsigned long days = epoch / 86400UL;
  unsigned long secondsInDay = epoch % 86400UL;

  hour = secondsInDay / 3600;
  minute = (secondsInDay % 3600) / 60;
  second = secondsInDay % 60;

  year = 1970;
  while (true) {
    int daysInYear = isLeapYear(year) ? 366 : 365;
    if (days >= daysInYear) {
      days -= daysInYear;
      year++;
    } else {
      break;
    }
  }

  int daysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (isLeapYear(year)) {
    daysInMonth[1] = 29;
  }

  month = 0;
  while (days >= daysInMonth[month]) {
    days -= daysInMonth[month];
    month++;
  }
  day = days + 1;

  unsigned long daysSince1970 = (epoch / 86400UL);
  weekday = (daysSince1970 + 4) % 7;
}

bool isDST(int year, int month, int day, int weekday) {
  if (month < 3 || month > 11) return false;
  if (month > 3 && month < 11) return true;
  if (month == 3) {
    int wMarch1 = (weekday - (day - 1)) % 7;
    if (wMarch1 < 0) wMarch1 += 7;
    int secondSunday = 8 + ((7 - wMarch1) % 7);
    return day >= secondSunday;
  }
  if (month == 11) {
    int daysToNov1 = day - 1;
    int wNov1 = (weekday - daysToNov1) % 7;
    if (wNov1 < 0) wNov1 += 7;
    int firstSunday = 1 + ((7 - wNov1) % 7);
    return day < firstSunday;
  }
  return false;
}

void printDateTime(unsigned long epoch) {
  int year, month, day, hour, minute, second, weekday;
  epochToDateTime(epoch, year, month, day, hour, minute, second, weekday);

  // MM-DD-YYYY
  if ((month + 1) < 10) Serial.print("0");
  Serial.print(month + 1);
  Serial.print("-");
  if (day < 10) Serial.print("0");
  Serial.print(day);
  Serial.print("-");
  Serial.print(year);

  Serial.print(" ");

  // 24-hour format
  if (hour < 10) Serial.print("0");
  Serial.print(hour);
  Serial.print(":");
  if (minute < 10) Serial.print("0");
  Serial.print(minute);
  Serial.print(":");
  if (second < 10) Serial.print("0");
  Serial.print(second);

  Serial.print(" | ");

  // 12-hour format with AM/PM
  int hour12 = hour % 12;
  if (hour12 == 0) hour12 = 12;
  Serial.print(hour12);
  Serial.print(":");
  if (minute < 10) Serial.print("0");
  Serial.print(minute);
  Serial.print(":");
  if (second < 10) Serial.print("0");
  Serial.print(second);
  Serial.print(hour < 12 ? " AM" : " PM");

  Serial.println();
}

void requestNtpTime() {
  IPAddress ntpIP(129, 6, 15, 28);  // NIST NTP server IP
  Serial.println("Sending NTP request...");
  sendNTPpacket(ntpIP);

  unsigned long start = millis();
  while (millis() - start < 2000) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Serial.println("NTP response received!");
      Udp.read(packetBuffer, NTP_PACKET_SIZE);
      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
      unsigned long secsSince1900 = (highWord << 16) | lowWord;
      unsigned long epoch = secsSince1900 - 2208988800UL;

      int year, month, day, hour, minute, second, weekday;
      epochToDateTime(epoch, year, month, day, hour, minute, second, weekday);

      bool dstActive = isDST(year, month, day, weekday);
      int timeZoneOffset = dstActive ? -4 : -5;

      currentEpoch = epoch + (timeZoneOffset * 3600UL); // Apply timezone offset

      Serial.print("DST Active: ");
      Serial.println(dstActive ? "Yes (EDT)" : "No (EST)");
      Serial.print("NTP Unix Epoch Time (adjusted): ");
      Serial.println(currentEpoch);
      Serial.print("Local Date & Time: ");
      printDateTime(currentEpoch);

      return;
    }
  }
  Serial.println("NTP response timeout.");
}

// --- CSV Helper: format date as mm-dd-yyyy ---
String getDateString() {
  int year, month, day, hour, minute, second, weekday;
  epochToDateTime(currentEpoch, year, month, day, hour, minute, second, weekday);
  char buffer[11];
  snprintf(buffer, sizeof(buffer), "%02d-%02d-%04d", month + 1, day, year);
  return String(buffer);
}

// --- Write CSV header if file doesn't exist ---
void createCsvHeaderIfNeeded() {
  if (!SD.exists("temp.csv")) {
    File dataFile = SD.open("temp.csv", FILE_WRITE);
    if (dataFile) {
      dataFile.println("Date,Sensor A,Sensor B,Sensor C,Sensor D");
      dataFile.close();
      Serial.println("CSV header created.");
    } else {
      Serial.println("Failed to create CSV header.");
    }
  }
}

// --- Append sensor data line to CSV ---
void appendCsvData() {
  File dataFile = SD.open("temp.csv", FILE_WRITE);
  if (dataFile) {
    String line = getDateString() + ",";
    line += isnan(tA) ? "ERR" : String(tA, 1);
    line += ",";
    line += isnan(tB) ? "ERR" : String(tB, 1);
    line += ",";
    line += isnan(tC) ? "ERR" : String(tC, 1);
    line += ",";
    line += isnan(tD) ? "ERR" : String(tD, 1);

    dataFile.println(line);
    dataFile.close();
    Serial.print("Temperature data written to CSV: ");
    Serial.println(line);
  } else {
    Serial.println("Failed to open temp.csv for writing.");
  }
}

// ----- Setup and Loop -----

void setup() {
  Serial.begin(9600);
  while (!Serial);

  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

  // ----- STARTUP SEQUENCE -----
  for (int i = 0; i < 5; i++) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("System Booting");
    for (int dot = 0; dot <= i && dot < 3; dot++) lcd.print(".");
    delay(1000);
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Red/Green Stack");
  lcd.setCursor(0, 1); lcd.print("LED Testing");
  for (int i = 0; i < 2; i++) {
    digitalWrite(RED_LED_PIN, HIGH); delay(250);
    digitalWrite(RED_LED_PIN, LOW);  delay(250);
  }
  for (int i = 0; i < 2; i++) {
    digitalWrite(GREEN_LED_PIN, HIGH); delay(250);
    digitalWrite(GREEN_LED_PIN, LOW);  delay(250);
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("LCD Testing");
  delay(1000);
  lcd.clear();
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 20; col++) {
      lcd.setCursor(col, row);
      lcd.write(255);
      delay(125);
    }
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("System Ready");
  delay(10000);
  lcd.clear();
  // ----- END STARTUP SEQUENCE -----

  dhtA.begin(); dhtB.begin(); dhtC.begin(); dhtD.begin();

  // Load threshold from EEPROM
  EEPROM.get(0, tempThreshold);
  if (tempThreshold < 20.0 || tempThreshold > 50.0) {
    tempThreshold = 22.0; // fallback
  }

  // --- Ethernet setup for NTP ---
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);
  Ethernet.init(10);

  Serial.println("Starting Ethernet with DHCP...");
  if (Ethernet.begin(mac) == 0) {
    Serial.println("DHCP failed");
    while(true);
  }

  delay(1000);

  Serial.print("Ethernet IP: ");
  Serial.println(Ethernet.localIP());

  Udp.begin(localPort);

  requestNtpTime(); // initial NTP sync
  lastNtpCheck = millis();

  // --- SD card initialization ---
  if (!SD.begin(chipSelect)) {
    Serial.println("SD card initialization failed!");
  } else {
    Serial.println("SD card initialized.");
    createCsvHeaderIfNeeded();
  }

  lastDisplaySwitch = millis();
}

void loop() {
  unsigned long now = millis();

  // ----------- BUTTON HOLD → ADJUSTMENT MODE ------------
  if (digitalRead(BUTTON_PIN) == LOW) {
    unsigned long holdStart = millis();
    while (digitalRead(BUTTON_PIN) == LOW) {
      // Alternate LEDs during hold
      digitalWrite(RED_LED_PIN, (millis() / 250) % 2);
      digitalWrite(GREEN_LED_PIN, !((millis() / 250) % 2));
      if (millis() - holdStart >= 5000) break;
      delay(50);
    }

    if (millis() - holdStart >= 5000) {
      float oldThreshold = tempThreshold;
      unsigned long lastIncTime = 0;
      lcd.clear();

      while (digitalRead(BUTTON_PIN) == LOW) {
        // Blink line 0 only
        if (millis() - lastBlinkToggle >= 500) {
          blinkState = !blinkState;
          lastBlinkToggle = millis();
        }

        digitalWrite(GREEN_LED_PIN, blinkState);  // green blinks in adjustment
        digitalWrite(RED_LED_PIN, LOW);

        // Increment every 2 seconds
        if (millis() - lastIncTime >= 2000) {
          tempThreshold += 1.0;
          if (tempThreshold > 50.0) tempThreshold = 20.0;
          lastIncTime = millis();
        }

        // LCD Display
        if (blinkState) {
          lcd.setCursor(0, 0); lcd.print("Adjustment Mode   ");
        } else {
          lcd.setCursor(0, 0); lcd.print("                  ");
        }

        lcd.setCursor(0, 1); lcd.print("Adjusting...      ");
        lcd.setCursor(0, 2); lcd.print("New Threshold: ");
        lcd.print((int)tempThreshold);
        lcd.setCursor(0, 3); lcd.print("Release to exit   ");
        delay(50);
      }

      // Save to EEPROM
      EEPROM.put(0, tempThreshold);

      // Red LED blink after save
      for (int i = 0; i < 10; i++) {
        digitalWrite(RED_LED_PIN, HIGH);
        digitalWrite(GREEN_LED_PIN, LOW);
        delay(250);
        digitalWrite(RED_LED_PIN, LOW);
        delay(250);
      }

      // Confirmation screen + blink both LEDs
      lcd.clear();
      for (int i = 0; i < 20; i++) {
        lcd.setCursor(0, 0); lcd.print("Threshold Updated ");
        lcd.setCursor(0, 2); lcd.print("Old: ");
        lcd.print((int)oldThreshold);
        lcd.setCursor(0, 3); lcd.print("New: ");
        lcd.print((int)tempThreshold);

        digitalWrite(RED_LED_PIN, i % 2);
        digitalWrite(GREEN_LED_PIN, i % 2);
        delay(500);
      }

      lcd.clear();
      digitalWrite(RED_LED_PIN, LOW);
      digitalWrite(GREEN_LED_PIN, LOW);
    }
  }

  // ----------- SENSOR READINGS -------------
  if (now - lastSensorRead >= sensorReadInterval) {
    tA = dhtA.readTemperature(); if (isnan(tA)) { delay(500); tA = dhtA.readTemperature(); }
    tB = dhtB.readTemperature(); if (isnan(tB)) { delay(500); tB = dhtB.readTemperature(); }
    tC = dhtC.readTemperature(); if (isnan(tC)) { delay(500); tC = dhtC.readTemperature(); }
    tD = dhtD.readTemperature(); if (isnan(tD)) { delay(500); tD = dhtD.readTemperature(); }

    hA = dhtA.readHumidity(); if (isnan(hA)) { delay(500); hA = dhtA.readHumidity(); }
    hB = dhtB.readHumidity(); if (isnan(hB)) { delay(500); hB = dhtB.readHumidity(); }
    hC = dhtC.readHumidity(); if (isnan(hC)) { delay(500); hC = dhtC.readHumidity(); }
    hD = dhtD.readHumidity(); if (isnan(hD)) { delay(500); hD = dhtD.readHumidity(); }

    lastSensorRead = now;
  }

  bool tempError = isnan(tA) || isnan(tB) || isnan(tC) || isnan(tD);
  bool tempOutOfRange =
    (!isnan(tA) && abs(tA - tempThreshold) > thresholdMargin) ||
    (!isnan(tB) && abs(tB - tempThreshold) > thresholdMargin) ||
    (!isnan(tC) && abs(tC - tempThreshold) > thresholdMargin) ||
    (!isnan(tD) && abs(tD - tempThreshold) > thresholdMargin);

  unsigned long blinkInterval = tempError ? blinkIntervalFast : blinkIntervalNormal;
  if (now - lastBlinkToggle >= blinkInterval) {
    blinkState = !blinkState;
    lastBlinkToggle = now;
  }

  if (tempError || tempOutOfRange) {
    digitalWrite(RED_LED_PIN, blinkState ? HIGH : LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
  } else {
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, HIGH);
  }

  // --- LCD DISPLAY (Unchanged as per your request) ---
  if (now - lastDisplaySwitch >= 10000) {
    displayMode = !displayMode;
    lcd.clear();
    lastDisplaySwitch = now;
  }

  lcd.setCursor(0, 0); lcd.print("Seegrid Aging Room");

  if (displayMode == 0) {
    lcd.setCursor(0, 1); lcd.print("Temperature       ");
    lcd.setCursor(0, 2); lcd.print("A: ");
    lcd.print(isnan(tA) ? (blinkState ? "ERR  " : "     ") :
             (abs(tA - tempThreshold) > thresholdMargin && blinkState) ? "     " :
             String(tA, 1) + " C");

    lcd.setCursor(10, 2); lcd.print("B: ");
    lcd.print(isnan(tB) ? (blinkState ? "ERR  " : "     ") :
             (abs(tB - tempThreshold) > thresholdMargin && blinkState) ? "     " :
             String(tB, 1) + " C");

    lcd.setCursor(0, 3); lcd.print("C: ");
    lcd.print(isnan(tC) ? (blinkState ? "ERR  " : "     ") :
             (abs(tC - tempThreshold) > thresholdMargin && blinkState) ? "     " :
             String(tC, 1) + " C");

    lcd.setCursor(10, 3); lcd.print("D: ");
    lcd.print(isnan(tD) ? (blinkState ? "ERR  " : "     ") :
             (abs(tD - tempThreshold) > thresholdMargin && blinkState) ? "     " :
             String(tD, 1) + " C");

  } else {
    lcd.setCursor(0, 1); lcd.print("Humidity          ");
    lcd.setCursor(0, 2); lcd.print("A: ");
    lcd.print(isnan(hA) ? (blinkState ? "ERR  " : "     ") : String(hA, 1) + " %");

    lcd.setCursor(10, 2); lcd.print("B: ");
    lcd.print(isnan(hB) ? (blinkState ? "ERR  " : "     ") : String(hB, 1) + " %");

    lcd.setCursor(0, 3); lcd.print("C: ");
    lcd.print(isnan(hC) ? (blinkState ? "ERR  " : "     ") : String(hC, 1) + " %");

    lcd.setCursor(10, 3); lcd.print("D: ");
    lcd.print(isnan(hD) ? (blinkState ? "ERR  " : "     ") : String(hD, 1) + " %");
  }

  // --- NTP sync every 24 hours ---
  if (now - lastNtpCheck > ntpInterval) {
    requestNtpTime();
    lastNtpCheck = now;
  }

  // --- CSV write every 30 seconds ---
  if (now - lastCsvWrite >= csvWriteInterval) {
    appendCsvData();
    lastCsvWrite = now;
  }
}
