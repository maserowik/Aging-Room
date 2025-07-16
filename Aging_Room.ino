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

unsigned long currentEpoch = 0;
unsigned long lastNtpCheck = 0;
const unsigned long ntpInterval = 86400000;

// --- DHT22 and LCD setup ---
#define DHTTYPE DHT22
DHT dhtA(2, DHTTYPE), dhtB(3, DHTTYPE), dhtC(5, DHTTYPE), dhtD(6, DHTTYPE);

LiquidCrystal_I2C lcd(0x3F, 20, 4);
#define RED_LED_PIN 8
#define GREEN_LED_PIN 7
#define BUTTON_PIN 13

float tempThreshold = 22.0;
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
const int chipSelect = 4;
const unsigned long csvWriteInterval = 300000;  //how often the CSV is upated in milliseconds
unsigned long lastCsvWrite = 0;

// --- Ethernet Server ---
EthernetServer server(80);

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
  if (isLeapYear(year)) daysInMonth[1] = 29;

  month = 0;
  while (days >= daysInMonth[month]) {
    days -= daysInMonth[month];
    month++;
  }
  day = days + 1;

  unsigned long daysSince1970 = epoch / 86400UL;
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

void requestNtpTime() {
  IPAddress ntpIP(129, 6, 15, 28);
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
      unsigned long epoch = (highWord << 16) | lowWord;

      unsigned long deviceEpoch = currentEpoch; // existing device time before update
      long offset = (long)epoch - (long)deviceEpoch;

      Serial.print("NTP Offset (seconds): ");
      Serial.println(offset);

      epoch -= 2208988800UL;

      int year, month, day, hour, minute, second, weekday;
      epochToDateTime(epoch, year, month, day, hour, minute, second, weekday);
      bool dstActive = isDST(year, month, day, weekday);
      int timeZoneOffset = dstActive ? -4 : -5;
      currentEpoch = epoch + (timeZoneOffset * 3600UL);

      Serial.print("DST Active: ");
      Serial.println(dstActive ? "Yes (EDT)" : "No (EST)");
      Serial.print("Local Date & Time: ");
      Serial.print(month + 1); Serial.print("-");
      Serial.print(day); Serial.print("-");
      Serial.print(year); Serial.print(" ");
      Serial.print(hour); Serial.print(":");
      Serial.print(minute); Serial.print(":");
      Serial.println(second);
      return;
    }
  }
  Serial.println("NTP response timeout.");
}

String getDateString() {
  int year, month, day, hour, minute, second, weekday;
  epochToDateTime(currentEpoch, year, month, day, hour, minute, second, weekday);
  char buffer[11];
  snprintf(buffer, sizeof(buffer), "%02d-%02d-%04d", month + 1, day, year);
  return String(buffer);
}

// *** New function added to get HH:MM:SS time string ***
String getTimeString() {
  int year, month, day, hour, minute, second, weekday;
  epochToDateTime(currentEpoch, year, month, day, hour, minute, second, weekday);
  char buffer[9];
  snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hour, minute, second);
  return String(buffer);
}

void createCsvHeaderIfNeeded() {
  if (!SD.exists("temp.csv")) {
    File f = SD.open("temp.csv", FILE_WRITE);
    if (f) {
      f.println("Date,Time,Sensor A,Sensor B,Sensor C,Sensor D");
      f.close();
    }
  }
  if (!SD.exists("humid.csv")) {
    File f = SD.open("humid.csv", FILE_WRITE);
    if (f) {
      f.println("Date,Time,Sensor A,Sensor B,Sensor C,Sensor D");
      f.close();
    }
  }
}

void appendCsvData() {
  String dateStr = getDateString();
  String timeStr = getTimeString();

  File tf = SD.open("temp.csv", FILE_WRITE);
  if (tf) {
    tf.print(dateStr + "," + timeStr + ",");
    tf.print(isnan(tA) ? "ERR" : String(tA, 1) + " C"); tf.print(",");
    tf.print(isnan(tB) ? "ERR" : String(tB, 1) + " C"); tf.print(",");
    tf.print(isnan(tC) ? "ERR" : String(tC, 1) + " C"); tf.print(",");
    tf.println(isnan(tD) ? "ERR" : String(tD, 1) + " C");
    tf.close();

    Serial.print("Temperature data written to temp.csv: ");
    Serial.print(dateStr);
    Serial.print(" ");
    Serial.print(timeStr);
    Serial.print(", ");
    Serial.print(isnan(tA) ? "ERR" : String(tA, 1) + " °C");
    Serial.print(", ");
    Serial.print(isnan(tB) ? "ERR" : String(tB, 1) + " °C");
    Serial.print(", ");
    Serial.print(isnan(tC) ? "ERR" : String(tC, 1) + " °C");
    Serial.print(", ");
    Serial.println(isnan(tD) ? "ERR" : String(tD, 1) + " °C");
  } else {
    Serial.println("Failed to open temp.csv for writing.");
  }

  File hf = SD.open("humid.csv", FILE_WRITE);
  if (hf) {
    hf.print(dateStr + "," + timeStr + ",");
    hf.print(isnan(hA) ? "ERR" : String(hA, 1) + " %"); hf.print(",");
    hf.print(isnan(hB) ? "ERR" : String(hB, 1) + " %"); hf.print(",");
    hf.print(isnan(hC) ? "ERR" : String(hC, 1) + " %"); hf.print(",");
    hf.println(isnan(hD) ? "ERR" : String(hD, 1) + " %");
    hf.close();

    Serial.print("Humidity data written to humid.csv: ");
    Serial.print(dateStr);
    Serial.print(" ");
    Serial.print(timeStr);
    Serial.print(", ");
    Serial.print(isnan(hA) ? "ERR" : String(hA, 1) + " %");
    Serial.print(", ");
    Serial.print(isnan(hB) ? "ERR" : String(hB, 1) + " %");
    Serial.print(", ");
    Serial.print(isnan(hC) ? "ERR" : String(hC, 1) + " %");
    Serial.print(", ");
    Serial.println(isnan(hD) ? "ERR" : String(hD, 1) + " %");
  } else {
    Serial.println("Failed to open humid.csv for writing.");
  }
}

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
    digitalWrite(RED_LED_PIN, LOW); delay(250);
  }
  for (int i = 0; i < 2; i++) {
    digitalWrite(GREEN_LED_PIN, HIGH); delay(250);
    digitalWrite(GREEN_LED_PIN, LOW); delay(250);
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
    tempThreshold = 22.0;
  }

  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);
  Ethernet.init(10);

  Serial.println("Starting Ethernet with DHCP...");
  if (Ethernet.begin(mac) == 0) {
    Serial.println("DHCP failed");
    while (true);
  }

  delay(1000);
  Serial.print("Ethernet IP: ");
  Serial.println(Ethernet.localIP());

  Udp.begin(localPort);
  requestNtpTime();
  lastNtpCheck = millis();

  if (!SD.begin(chipSelect)) {
    Serial.println("SD card initialization failed!");
  } else {
    Serial.println("SD card initialized.");
    createCsvHeaderIfNeeded();
  }

  lastDisplaySwitch = millis();

  server.begin();  // Start Ethernet server for web requests
}

void serveFile(EthernetClient &client, const char* filename, const char* contentType) {
  if (SD.exists(filename)) {
    File file = SD.open(filename, FILE_READ);
    client.println("HTTP/1.1 200 OK");
    client.print("Content-Type: ");
    client.println(contentType);
    client.println("Connection: close");
    client.println();

    while (file.available()) {
      client.write(file.read());
    }
    file.close();
  } else {
    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("File not found");
  }
}

void serveRootPage(EthernetClient &client) {
  String lastUpdate = getDateString() + " " + getTimeString();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><title>CSV Download</title></head><body>");
  client.println("<h1>Seegrid Aging Room Data</h1>");
  client.print("<p>Last update: ");
  client.print(lastUpdate);
  client.println("</p>");
  client.println("<ul>");
  client.println("<li><a href=\"/temp.csv\">Download Temperature CSV</a></li>");
  client.println("<li><a href=\"/humid.csv\">Download Humidity CSV</a></li>");
  client.println("<li><a href=\"/delete_temp\">Delete Temperature CSV</a></li>");
  client.println("<li><a href=\"/delete_humid\">Delete Humidity CSV</a></li>");
  client.println("</ul>");
  client.println("</body></html>");
}

void loop() {
  unsigned long now = millis();

  // --- Threshold Menu Button Hold ---
  if (digitalRead(BUTTON_PIN) == LOW) {
    unsigned long holdStart = millis();
    while (digitalRead(BUTTON_PIN) == LOW) {
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
        if (millis() - lastBlinkToggle >= 500) {
          blinkState = !blinkState;
          lastBlinkToggle = millis();
        }

        digitalWrite(GREEN_LED_PIN, HIGH);
        digitalWrite(RED_LED_PIN, LOW);

        if (millis() - lastIncTime >= 2000) {
          tempThreshold += 1.0;
          if (tempThreshold > 50.0) tempThreshold = 20.0;
          lastIncTime = millis();
        }

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

      EEPROM.put(0, tempThreshold);

      // Save confirmation — red blinks
      for (int i = 0; i < 10; i++) {
        digitalWrite(RED_LED_PIN, HIGH);
        digitalWrite(GREEN_LED_PIN, LOW);
        delay(250);
        digitalWrite(RED_LED_PIN, LOW);
        delay(250);
      }

      // Show old/new threshold — both blink
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

  // --- Sensor Readings ---
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
// --- LED Logic ---
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

if (tempError) {
  digitalWrite(RED_LED_PIN, blinkState ? HIGH : LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
} else if (tempOutOfRange) {
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, LOW);
} else {
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, LOW);
}

 

  // --- LCD Display ---
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

  // --- NTP Time Check every 24h ---
  if (millis() - lastNtpCheck >= ntpInterval) {
    requestNtpTime();
    lastNtpCheck = millis();
  }

  // --- CSV File Write every 5 min ---
  if (millis() - lastCsvWrite >= csvWriteInterval) {
    appendCsvData();
    lastCsvWrite = millis();
  }

    // --- Web Server Code Injection ---
  EthernetClient client = server.available();
  if (client) {
    bool currentLineIsBlank = true;
    String httpRequest = "";

    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        httpRequest += c;

        if (c == '\n' && currentLineIsBlank) {
          if (httpRequest.startsWith("GET /temp.csv")) {
            serveFile(client, "temp.csv", "text/csv");
            break;
          } else if (httpRequest.startsWith("GET /humid.csv")) {
            serveFile(client, "humid.csv", "text/csv");
            break;
          } else if (httpRequest.startsWith("GET /delete_temp")) {
            SD.remove("temp.csv");
            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: text/plain");
            client.println("Connection: close");
            client.println();
            client.println("Temperature CSV deleted.");
            break;
          } else if (httpRequest.startsWith("GET /delete_humid")) {
            SD.remove("humid.csv");
            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: text/plain");
            client.println("Connection: close");
            client.println();
            client.println("Humidity CSV deleted.");
            break;
          } else if (httpRequest.startsWith("GET /")) {
            serveRootPage(client);
            break;
          } else {
            client.println("HTTP/1.1 404 Not Found");
            client.println("Content-Type: text/plain");
            client.println("Connection: close");
            client.println();
            client.println("404 Not Found");
            break;
          }
        }

        if (c == '\n') {
          currentLineIsBlank = true;
        } else if (c != '\r') {
          currentLineIsBlank = false;
        }
      }
    }
    delay(1);
    client.stop();
  }
}