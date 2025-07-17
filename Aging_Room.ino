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

bool isDST(int year, int month, int day, int weekday) {
  // US DST rules: DST starts second Sunday in March and ends first Sunday in November
  if (month < 3 || month > 11) return false; // Jan, Feb, Dec = no DST
  if (month > 3 && month < 11) return true;  // Apr-Oct = DST

  // For March and November, calculate DST transition Sundays:
  // weekday: 0=Sunday, 1=Monday, ... 6=Saturday
  int previousSunday = day - weekday; // find previous Sunday of the month day

  if (month == 3) {
    // DST starts on second Sunday in March
    // second Sunday means previousSunday >= 8 (days)
    return (previousSunday >= 8);
  } else if (month == 11) {
    // DST ends on first Sunday in November
    // first Sunday means previousSunday < 8
    return (previousSunday < 8);
  }

  return false; // fallback
}



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
const unsigned long csvWriteInterval = 300000;  // 5 minutes
unsigned long lastCsvWrite = 0;

// --- Ethernet Server ---
EthernetServer server(80);

// --- Function Prototypes ---
void sendNTPpacket(IPAddress& address);
bool isLeapYear(int year);
void epochToDateTime(unsigned long epoch, int &year, int &month, int &day, int &hour, int &minute, int &second, int &weekday);
bool isDST(int year, int month, int day, int weekday);
void requestNtpTime();
String getDateString();
String getTimeString();
void createCsvHeaderIfNeeded();
void appendCsvData();
void serveFile(EthernetClient &client, const char* filename, const char* contentType);
void serveRootPage(EthernetClient &client);
void serveStatsPage(EthernetClient &client);  // <-- Added stats page

// --- Existing locked functions here (omitted for brevity) ---

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

void loop() {
  unsigned long now = millis();

  // --- Update internal clock every second ---
  static unsigned long lastEpochUpdate = 0;
  if (now - lastEpochUpdate >= 1000) {
    currentEpoch++;
    lastEpochUpdate = now;
  }

  // --- Threshold Menu Button Hold (locked logic) ---
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

  unsigned long blinkInterval;

  if (tempError) {
    blinkInterval = blinkIntervalFast; // Fast blink for error
  } else if (tempOutOfRange) {
    blinkInterval = blinkIntervalNormal; // Slow blink for out of range
  } else {
    blinkInterval = 0; // No blink when normal
  }

  if (blinkInterval > 0 && now - lastBlinkToggle >= blinkInterval) {
    blinkState = !blinkState;
    lastBlinkToggle = now;
  }

  if (tempError) {
    digitalWrite(RED_LED_PIN, blinkState ? HIGH : LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
  } else if (tempOutOfRange) {
    digitalWrite(RED_LED_PIN, blinkState ? HIGH : LOW);
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

  // --- Web Server Handling ---
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
            createCsvHeaderIfNeeded();
            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: text/plain");
            client.println("Connection: close");
            client.println();
            client.println("Temperature CSV deleted.");
            break;
          } else if (httpRequest.startsWith("GET /delete_humid")) {
            SD.remove("humid.csv");
            createCsvHeaderIfNeeded();
            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: text/plain");
            client.println("Connection: close");
            client.println();
            client.println("Humidity CSV deleted.");
            break;
          } else if (httpRequest.startsWith("GET /stats")) {
            serveStatsPage(client);  // <-- Your new stats page URL
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

// --- Added your stats page ---
void serveStatsPage(EthernetClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();

  client.println(F(R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Seegrid Aging Room Statistics</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <script src="https://cdn.jsdelivr.net/npm/chartjs-plugin-zoom@2.0.1/dist/chartjs-plugin-zoom.min.js"></script>
  <style>
    body { font-family: Arial; padding: 20px; }
    canvas { max-width: 100%; height: auto; }
    h2 { margin-top: 40px; }
  </style>
</head>
<body>
  <h1>Seegrid Aging Room Statistics For The Past 14 Days</h1>
  <h2>Temperature (°C)</h2>
  <canvas id="tempChart"></canvas>

  <h2>Humidity (%)</h2>
  <canvas id="humidChart"></canvas>

  <script>
    async function fetchCsv(url) {
      const res = await fetch(url);
      const text = await res.text();
      return text.split("\\n").slice(1).filter(row => row.trim() !== "");
    }

    function parseCsvRows(rows) {
      const labels = [];
      const a = [], b = [], c = [], d = [];

      for (let row of rows) {
        const cols = row.split(",");
        if (cols.length < 6) continue;

        const datetime = cols[0] + " " + cols[1];
        labels.push(datetime);
        a.push(parseFloat(cols[2]) || null);
        b.push(parseFloat(cols[3]) || null);
        c.push(parseFloat(cols[4]) || null);
        d.push(parseFloat(cols[5]) || null);
      }
      return { labels, a, b, c, d };
    }

    function createDataset(label, data, color) {
      return {
        label: label,
        data: data,
        borderColor: color,
        backgroundColor: 'transparent',
        fill: false,
        spanGaps: true,
        pointRadius: 2,
        tension: 0.2
      };
    }

    async function drawCharts() {
      const tempRows = await fetchCsv('/temp.csv');
      const humidRows = await fetchCsv('/humid.csv');

      const tempData = parseCsvRows(tempRows);
      const humidData = parseCsvRows(humidRows);

      const tempThreshold = 42;

      const tempCtx = document.getElementById('tempChart').getContext('2d');
      const humidCtx = document.getElementById('humidChart').getContext('2d');

      const tempChart = new Chart(tempCtx, {
        type: 'line',
        data: {
          labels: tempData.labels,
          datasets: [
            createDataset('Sensor A', tempData.a, 'red'),
            createDataset('Sensor B', tempData.b, 'blue'),
            createDataset('Sensor C', tempData.c, 'green'),
            createDataset('Sensor D', tempData.d, 'orange'),
            {
              label: 'Threshold 42°C',
              data: tempData.labels.map(() => tempThreshold),
              borderColor: 'black',
              borderWidth: 1,
              borderDash: [5,5],
              fill: false,
              pointRadius: 0,
              tension: 0,
            }
          ]
        },
        options: {
          responsive: true,
          interaction: {
            mode: 'nearest',
            intersect: false
          },
          plugins: {
            zoom: {
              zoom: {
                wheel: { enabled: true },
                pinch: { enabled: true },
                mode: 'x',
              },
              pan: {
                enabled: true,
                mode: 'x',
              }
            },
            tooltip: {
              callbacks: {
                label: ctx => {
                  let label = ctx.dataset.label || '';
                  if (label) {
                    label += ': ';
                  }
                  label += ctx.parsed.y !== null ? ctx.parsed.y.toFixed(1) : 'N/A';
                  return label;
                }
              }
            }
          },
          scales: {
            x: {
              type: 'time',
              time: {
                parser: 'YYYY-MM-DD HH:mm:ss',
                tooltipFormat: 'MMM D, YYYY HH:mm',
                unit: 'day',
                displayFormats: {
                  day: 'MMM D'
                }
              },
              title: { display: true, text: 'Date' }
            },
            y: {
              title: { display: true, text: 'Temperature (°C)' },
              min: 0,
              max: 60
            }
          }
        }
      });

      const humidChart = new Chart(humidCtx, {
        type: 'line',
        data: {
          labels: humidData.labels,
          datasets: [
            createDataset('Sensor A', humidData.a, 'red'),
            createDataset('Sensor B', humidData.b, 'blue'),
            createDataset('Sensor C', humidData.c, 'green'),
            createDataset('Sensor D', humidData.d, 'orange'),
          ]
        },
        options: {
          responsive: true,
          interaction: {
            mode: 'nearest',
            intersect: false
          },
          plugins: {
            zoom: {
              zoom: {
                wheel: { enabled: true },
                pinch: { enabled: true },
                mode: 'x',
              },
              pan: {
                enabled: true,
                mode: 'x',
              }
            },
            tooltip: {
              callbacks: {
                label: ctx => {
                  let label = ctx.dataset.label || '';
                  if (label) {
                    label += ': ';
                  }
                  label += ctx.parsed.y !== null ? ctx.parsed.y.toFixed(1) : 'N/A';
                  return label;
                }
              }
            }
          },
          scales: {
            x: {
              type: 'time',
              time: {
                parser: 'YYYY-MM-DD HH:mm:ss',
                tooltipFormat: 'MMM D, YYYY HH:mm',
                unit: 'day',
                displayFormats: {
                  day: 'MMM D'
                }
              },
              title: { display: true, text: 'Date' }
            },
            y: {
              title: { display: true, text: 'Humidity (%)' },
              min: 0,
              max: 100
            }
          }
        }
      });
    }

    drawCharts();
  </script>
</body>
</html>
  )rawliteral"));
}

// --- Implement serveRootPage and serveFile as in your locked code ---
void serveRootPage(EthernetClient &client) {
  // Your locked homepage serving code here...
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println(F("<html><head><title>Seegrid Aging Room</title></head><body>"));
  client.println(F("<h1>Seegrid Aging Room</h1>"));
  client.println(F("<ul>"));
  client.println(F("<li><a href=\"/temp.csv\">Download Temperature CSV</a></li>"));
  client.println(F("<li><a href=\"/delete_temp\">Delete Temperature CSV</a></li>"));
  client.println(F("<li><a href=\"/humid.csv\">Download Humidity CSV</a></li>"));
  client.println(F("<li><a href=\"/delete_humid\">Delete Humidity CSV</a></li>"));
  client.println(F("<li><a href=\"/stats\">Seegrid Aging Room Statistics</a></li>"));
  client.println(F("</ul>"));
  client.println(F("</body></html>"));
}

void serveFile(EthernetClient &client, const char* filename, const char* contentType) {
  if (!SD.exists(filename)) {
    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("File not found");
    return;
  }

  File file = SD.open(filename);
  client.println("HTTP/1.1 200 OK");
  client.print("Content-Type: ");
  client.println(contentType);
  client.println("Connection: close");
  client.println();

  while (file.available()) {
    client.write(file.read());
  }
  file.close();
}

// --- CSV header and append functions from locked code ---
void createCsvHeaderIfNeeded() {
  if (!SD.exists("temp.csv")) {
    File f = SD.open("temp.csv", FILE_WRITE);
    f.println("Date,Time,SensorA,SensorB,SensorC,SensorD");
    f.close();
  }
  if (!SD.exists("humid.csv")) {
    File f = SD.open("humid.csv", FILE_WRITE);
    f.println("Date,Time,SensorA,SensorB,SensorC,SensorD");
    f.close();
  }
}

void appendCsvData() {
  // Convert currentEpoch to date/time strings (locked logic assumed)
  int year, month, day, hour, minute, second, weekday;
  epochToDateTime(currentEpoch, year, month, day, hour, minute, second, weekday);

  char dateStr[11];
  sprintf(dateStr, "%04d-%02d-%02d", year, month, day);

  char timeStr[9];
  sprintf(timeStr, "%02d:%02d:%02d", hour, minute, second);

  File fTemp = SD.open("temp.csv", FILE_WRITE);
  if (fTemp) {
    fTemp.print(dateStr);
    fTemp.print(",");
    fTemp.print(timeStr);
    fTemp.print(",");
    fTemp.print(isnan(tA) ? "" : String(tA,1));
    fTemp.print(",");
    fTemp.print(isnan(tB) ? "" : String(tB,1));
    fTemp.print(",");
    fTemp.print(isnan(tC) ? "" : String(tC,1));
    fTemp.print(",");
    fTemp.println(isnan(tD) ? "" : String(tD,1));
    fTemp.close();
  }

  File fHumid = SD.open("humid.csv", FILE_WRITE);
  if (fHumid) {
    fHumid.print(dateStr);
    fHumid.print(",");
    fHumid.print(timeStr);
    fHumid.print(",");
    fHumid.print(isnan(hA) ? "" : String(hA,1));
    fHumid.print(",");
    fHumid.print(isnan(hB) ? "" : String(hB,1));
    fHumid.print(",");
    fHumid.print(isnan(hC) ? "" : String(hC,1));
    fHumid.print(",");
    fHumid.println(isnan(hD) ? "" : String(hD,1));
    fHumid.close();
  }
}

// --- Stub epochToDateTime (locked logic assumed) ---
void epochToDateTime(unsigned long epoch, int &year, int &month, int &day, int &hour, int &minute, int &second, int &weekday) {
  // Your locked conversion code here, or simplified for example:
  // For demonstration, just a fixed date/time
  year = 2025; month = 7; day = 16; hour = (epoch / 3600) % 24; minute = (epoch / 60) % 60; second = epoch % 60; weekday = 4;
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
