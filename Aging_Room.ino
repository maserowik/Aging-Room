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
const unsigned long ntpInterval = 86400000;  // 24 hrs

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
const unsigned long csvWriteInterval = 30000;
unsigned long lastCsvWrite = 0;

// --- NTP Helpers ---
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

  weekday = (epoch / 86400 + 4) % 7;
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
    int wNov1 = (weekday - (day - 1)) % 7;
    if (wNov1 < 0) wNov1 += 7;
    int firstSunday = 1 + ((7 - wNov1) % 7);
    return day < firstSunday;
  }
  return false;
}

void printDateTime(unsigned long epoch) {
  int y, m, d, h, min, s, w;
  epochToDateTime(epoch, y, m, d, h, min, s, w);

  if (m + 1 < 10) Serial.print("0");
  Serial.print(m + 1); Serial.print("-");
  if (d < 10) Serial.print("0");
  Serial.print(d); Serial.print("-");
  Serial.print(y); Serial.print(" ");

  if (h < 10) Serial.print("0");
  Serial.print(h); Serial.print(":");
  if (min < 10) Serial.print("0");
  Serial.print(min); Serial.print(":");
  if (s < 10) Serial.print("0");
  Serial.print(s); Serial.print(" | ");

  int h12 = h % 12; if (h12 == 0) h12 = 12;
  Serial.print(h12); Serial.print(":");
  if (min < 10) Serial.print("0");
  Serial.print(min); Serial.print(":");
  if (s < 10) Serial.print("0");
  Serial.print(s); Serial.println(h < 12 ? " AM" : " PM");
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
      unsigned long secsSince1900 = (highWord << 16) | lowWord;
      unsigned long epoch = secsSince1900 - 2208988800UL;

      int y, m, d, h, min, s, w;
      epochToDateTime(epoch, y, m, d, h, min, s, w);
      bool dst = isDST(y, m, d, w);
      currentEpoch = epoch + (dst ? -4 : -5) * 3600UL;

      Serial.print("DST Active: "); Serial.println(dst ? "Yes" : "No");
      Serial.print("Adjusted Epoch: "); Serial.println(currentEpoch);
      Serial.print("DateTime: "); printDateTime(currentEpoch);
      return;
    }
  }
  Serial.println("NTP response timeout.");
}

String getDateString() {
  int y, m, d, h, min, s, w;
  epochToDateTime(currentEpoch, y, m, d, h, min, s, w);
  char buf[11];
  snprintf(buf, sizeof(buf), "%02d-%02d-%04d", m + 1, d, y);
  return String(buf);
}

void createCsvHeaderIfNeeded() {
  if (!SD.exists("temp.csv")) {
    File file = SD.open("temp.csv", FILE_WRITE);
    if (file) {
      file.println("Date,Sensor A,Sensor B,Sensor C,Sensor D");
      file.close();
    }
  }
  if (!SD.exists("humid.csv")) {
    File file = SD.open("humid.csv", FILE_WRITE);
    if (file) {
      file.println("Date,Sensor A,Sensor B,Sensor C,Sensor D");
      file.close();
    }
  }
}

void appendCsvData() {
  String dateStr = getDateString();

  File tf = SD.open("temp.csv", FILE_WRITE);
  if (tf) {
    String line = dateStr + ",";
    line += isnan(tA) ? "ERR" : String(tA, 1); line += ",";
    line += isnan(tB) ? "ERR" : String(tB, 1); line += ",";
    line += isnan(tC) ? "ERR" : String(tC, 1); line += ",";
    line += isnan(tD) ? "ERR" : String(tD, 1);
    tf.println(line);
    tf.close();
    Serial.print("✅ temp.csv: "); Serial.println(line);
  } else {
    Serial.println("❌ Failed to write temp.csv");
  }

  File hf = SD.open("humid.csv", FILE_WRITE);
  if (hf) {
    String line = dateStr + ",";
    line += isnan(hA) ? "ERR" : String(hA, 1); line += ",";
    line += isnan(hB) ? "ERR" : String(hB, 1); line += ",";
    line += isnan(hC) ? "ERR" : String(hC, 1); line += ",";
    line += isnan(hD) ? "ERR" : String(hD, 1);
    hf.println(line);
    hf.close();
    Serial.print("✅ humid.csv: "); Serial.println(line);
  } else {
    Serial.println("❌ Failed to write humid.csv");
  }
}


// --- main setup and loop ---
void setup() {
  Serial.begin(9600);
  lcd.init(); lcd.backlight();
  pinMode(RED_LED_PIN, OUTPUT); pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  lcd.clear(); lcd.setCursor(0, 0); lcd.print("System Booting...");
  delay(3000);

  dhtA.begin(); dhtB.begin(); dhtC.begin(); dhtD.begin();

  EEPROM.get(0, tempThreshold);
  if (tempThreshold < 20.0 || tempThreshold > 50.0) tempThreshold = 22.0;

  Ethernet.init(10);
  if (Ethernet.begin(mac) == 0) while (true);
  Udp.begin(localPort);

  requestNtpTime();
  lastNtpCheck = millis();

  if (SD.begin(chipSelect)) {
    createCsvHeaderIfNeeded();
  }

  lastDisplaySwitch = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastSensorRead >= sensorReadInterval) {
    tA = dhtA.readTemperature(); hA = dhtA.readHumidity();
    tB = dhtB.readTemperature(); hB = dhtB.readHumidity();
    tC = dhtC.readTemperature(); hC = dhtC.readHumidity();
    tD = dhtD.readTemperature(); hD = dhtD.readHumidity();
    lastSensorRead = now;
  }

  if (now - lastCsvWrite >= csvWriteInterval) {
    appendCsvData();
    lastCsvWrite = now;
  }

  if (now - lastNtpCheck >= ntpInterval) {
    requestNtpTime();
    lastNtpCheck = now;
  }

  // --- LCD DISPLAY (locked, unchanged) ---
  if (now - lastDisplaySwitch >= 10000) {
    displayMode = !displayMode;
    lcd.clear();
    lastDisplaySwitch = now;
  }

  lcd.setCursor(0, 0); lcd.print("Seegrid Aging Room");

  if (displayMode == 0) {
    lcd.setCursor(0, 1); lcd.print("Temperature       ");
    lcd.setCursor(0, 2); lcd.print("A: ");
    lcd.print(isnan(tA) ? "ERR" : String(tA, 1) + " C");
    lcd.setCursor(10, 2); lcd.print("B: ");
    lcd.print(isnan(tB) ? "ERR" : String(tB, 1) + " C");
    lcd.setCursor(0, 3); lcd.print("C: ");
    lcd.print(isnan(tC) ? "ERR" : String(tC, 1) + " C");
    lcd.setCursor(10, 3); lcd.print("D: ");
    lcd.print(isnan(tD) ? "ERR" : String(tD, 1) + " C");
  } else {
    lcd.setCursor(0, 1); lcd.print("Humidity          ");
    lcd.setCursor(0, 2); lcd.print("A: ");
    lcd.print(isnan(hA) ? "ERR" : String(hA, 1) + " %");
    lcd.setCursor(10, 2); lcd.print("B: ");
    lcd.print(isnan(hB) ? "ERR" : String(hB, 1) + " %");
    lcd.setCursor(0, 3); lcd.print("C: ");
    lcd.print(isnan(hC) ? "ERR" : String(hC, 1) + " %");
    lcd.setCursor(10, 3); lcd.print("D: ");
    lcd.print(isnan(hD) ? "ERR" : String(hD, 1) + " %");
  }
}
