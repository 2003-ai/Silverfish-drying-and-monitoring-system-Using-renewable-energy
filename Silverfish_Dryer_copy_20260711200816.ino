void setu/*
 * ════════════════════════════════════════════════════════════════════
 *  SILVER FISH DRYER MONITORING SYSTEM — ESP32-S3
 *  Version: 3.2  (Auto-growing Table + Multi-WiFi + Dark Purple/Blue Theme)
 * ════════════════════════════════════════════════════════════════════
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <EEPROM.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>

// ════════════════════════════════════════════════════════════════════
//  PIN MAP (ESP32-S3)
// ════════════════════════════════════════════════════════════════════
#define PIN_DHT 5
#define PIN_SDA 8
#define PIN_SCL 9
#define PIN_HX711_DOUT 10
#define PIN_HX711_SCK 11
#define PIN_FAN_PWM 12
#define PIN_HEATER_PWM 13
#define PIN_GSM_TX 16
#define PIN_GSM_RX 17
#define PIN_DOOR_SENSOR 18

// ════════════════════════════════════════════════════════════════════
//  USER CONFIGURATION
// ════════════════════════════════════════════════════════════════════
#define SMS_NUMBER "+256700000000"  // Add your phone number here
#define WEB_USER "admin"
#define WEB_PASS "dryer2025"
#define LOADCELL_DIVIDER 420.0f
#define NTP_OFFSET 10800

// ─── Drying thresholds ────────────────────────────────────────────
#define WEIGHT_LOSS_PCT 80.0f
#define HUMIDITY_DRY_THRESH 25.0f
#define TEMP_TARGET 60.0f
#define TEMP_HYSTERESIS 2.0f
#define TEMP_MAX_SAFE 70.0f
#define FAN_DUTY_DRYING 220
#define FAN_DUTY_IDLE 80

// ─── Timing ───────────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS 5000UL
#define LCD_INTERVAL_MS 3000UL
#define EEPROM_INTERVAL_MS 60000UL
#define GSM_RETRY_MS 30000UL
#define GSM_BOOT_MS 3000UL
#define TABLE_UPDATE_MS 1500UL  // Table updates every 1.5 seconds

// ─── EEPROM ───────────────────────────────────────────────────────
#define EE_MAGIC_ADDR 0
#define EE_INIT_WEIGHT_ADDR 1
#define EE_INIT_HUM_ADDR 5
#define EE_RUNNING_ADDR 9
#define EE_MAGIC_VAL 0xA7

// ════════════════════════════════════════════════════════════════════
//  WI-FI NETWORKS (5 networks to scan)
// ════════════════════════════════════════════════════════════════════
struct WifiCred {
  const char *ssid;
  const char *password;
};

static const WifiCred WIFI_NETWORKS[] = {
  { "Kamya2026", "12345678" },
  { "Mikie2026", "12345678" },
  { "Chemowo2026", "12345678" },
  { "KIGULA", "iambrina123" },
  { "Jane2026", "12345678" },
};
static const uint8_t WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

// ════════════════════════════════════════════════════════════════════
//  OBJECTS
// ════════════════════════════════════════════════════════════════════
DHT dht(PIN_DHT, DHT11);
LiquidCrystal_I2C lcd(0x27, 16, 2);
HX711 scale;
WebServer server(80);
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org", NTP_OFFSET, 60000);
HardwareSerial gsmSerial(1);

// ════════════════════════════════════════════════════════════════════
//  GLOBAL STATE
// ════════════════════════════════════════════════════════════════════
float g_tempC = 0;
float g_humidity = 0;
float g_weightKg = 0;
bool g_dhtOK = false;
bool g_scaleOK = false;
bool g_lcdOK = false;
bool g_doorOpen = false;

float g_initWeightKg = 0;
float g_initHumidity = 0;

bool g_dryingActive = false;
bool g_dryingDone = false;
float g_weightLossPct = 0;
float g_humDropPct = 0;

uint8_t g_fanDuty = 0;
uint8_t g_heaterDuty = 0;

unsigned long g_lastSensorMs = 0;
unsigned long g_lastLcdMs = 0;
unsigned long g_lastEepromMs = 0;
unsigned long g_lastGsmRetryMs = 0;
unsigned long g_lastTableUpdateMs = 0;
unsigned long g_dryStartMs = 0;

uint8_t g_lcdPage = 0;

bool g_smsPendingStart = false;
String g_pendingSmsText = "";

bool g_loggedIn = false;
bool g_guestMode = false;
bool g_wifiReady = false;
String g_connectedSSID = "";
String g_localIP = "";

static bool g_scaleTared = false;
static bool g_selfCheckDone = false;

char g_ntpTime[10] = "--:--:--";
char g_ntpDate[14] = "---";

// ─── Data History for Table ────────────────────────────────────────
#define MAX_TABLE_RECORDS 200
struct TableRecord {
  String timestamp;
  float temp;
  float hum;
  float weight;
  float loss;
  float drop;
  int fan;
  int heat;
  String door;
  String status;
  String elapsed;
};
TableRecord tableRecords[MAX_TABLE_RECORDS];
int tableRecordCount = 0;

// ════════════════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════════
void readSensors();
void controlActuators();
void updateLCD();
void checkDryingComplete();
void startDrying();
void stopDrying();
void saveToEEPROM();
void loadFromEEPROM();
void gsmSendSMS(const String &msg);
void gsmProcess();
void handleWiFiConnect();
void runDeferredInit();
void updateNTP();
bool detectI2C(uint8_t addr);
void handleRoot();
void handleLogin();
void handleGuestLogin();
void handleLogout();
void handleData();
void handleControl();
void handleManualControl();
void sendDashboard();
void sendTablePage();
void handleTableHTML();
void addTableRecord();
String getTableHTML();

// ════════════════════════════════════════════════════════════════════
//  EEPROM FUNCTIONS
// ════════════════════════════════════════════════════════════════════
void eepromWriteFloat(int addr, float val) {
  union {
    float f;
    uint8_t b[4];
  } u;
  u.f = val;
  for (int i = 0; i < 4; i++) EEPROM.write(addr + i, u.b[i]);
}
float eepromReadFloat(int addr) {
  union {
    float f;
    uint8_t b[4];
  } u;
  for (int i = 0; i < 4; i++) u.b[i] = EEPROM.read(addr + i);
  return u.f;
}
void saveToEEPROM() {
  EEPROM.write(EE_MAGIC_ADDR, EE_MAGIC_VAL);
  eepromWriteFloat(EE_INIT_WEIGHT_ADDR, g_initWeightKg);
  eepromWriteFloat(EE_INIT_HUM_ADDR, g_initHumidity);
  EEPROM.write(EE_RUNNING_ADDR, g_dryingActive ? 1 : 0);
  EEPROM.commit();
}
void loadFromEEPROM() {
  if (EEPROM.read(EE_MAGIC_ADDR) != EE_MAGIC_VAL) return;
  g_initWeightKg = eepromReadFloat(EE_INIT_WEIGHT_ADDR);
  g_initHumidity = eepromReadFloat(EE_INIT_HUM_ADDR);
  g_dryingActive = (EEPROM.read(EE_RUNNING_ADDR) == 1);
}

// ════════════════════════════════════════════════════════════════════
//  NTP TIME
// ════════════════════════════════════════════════════════════════════
void updateNTP() {
  if (!g_wifiReady) return;
  ntpClient.update();
  int h = ntpClient.getHours();
  int m = ntpClient.getMinutes();
  int s = ntpClient.getSeconds();
  snprintf(g_ntpTime, sizeof(g_ntpTime), "%02d:%02d:%02d", h, m, s);

  unsigned long ep = ntpClient.getEpochTime();
  unsigned long days = ep / 86400UL;
  int y = 1970;
  while (true) {
    bool ly = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    uint32_t dy = ly ? 366 : 365;
    if (days < dy) break;
    days -= dy;
    y++;
  }
  const uint8_t md[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  bool ly2 = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  int mo = 0;
  uint32_t rem = days;
  for (mo = 0; mo < 12; mo++) {
    uint8_t d2 = md[mo];
    if (mo == 1 && ly2) d2 = 29;
    if (rem < d2) break;
    rem -= d2;
  }
  const char *mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  snprintf(g_ntpDate, sizeof(g_ntpDate), "%02d %s %04d", (int)rem + 1, mn[mo], y);
}

// ════════════════════════════════════════════════════════════════════
//  SENSORS
// ════════════════════════════════════════════════════════════════════
void readSensors() {
  float t = dht.readTemperature(), h = dht.readHumidity();
  g_dhtOK = (!isnan(t) && !isnan(h));
  if (g_dhtOK) {
    g_tempC = t;
    g_humidity = h;
  }

  if (scale.is_ready()) {
    float raw = scale.get_units(5);
    g_weightKg = max(0.0f, raw / 1000.0f);
    g_scaleOK = true;
  }

  g_doorOpen = (digitalRead(PIN_DOOR_SENSOR) == HIGH);

  if (g_dryingActive && g_initWeightKg > 0.1f) {
    g_weightLossPct = max(0.0f, min(100.0f,
                                    (g_initWeightKg - g_weightKg) / g_initWeightKg * 100.0f));
    if (g_initHumidity > 0)
      g_humDropPct = max(0.0f, min(100.0f,
                                   (g_initHumidity - g_humidity) / g_initHumidity * 100.0f));
  }
}

// ════════════════════════════════════════════════════════════════════
//  ACTUATORS
// ════════════════════════════════════════════════════════════════════
void controlActuators() {
  if (!g_dryingActive || g_dryingDone || g_doorOpen) {
    g_fanDuty = FAN_DUTY_IDLE;
    g_heaterDuty = 0;
  } else {
    g_fanDuty = FAN_DUTY_DRYING;
    if (!g_dhtOK || g_tempC >= TEMP_MAX_SAFE)
      g_heaterDuty = 0;
    else if (g_tempC < (TEMP_TARGET - TEMP_HYSTERESIS))
      g_heaterDuty = 255;
    else if (g_tempC > (TEMP_TARGET + TEMP_HYSTERESIS))
      g_heaterDuty = 0;
  }
  analogWrite(PIN_FAN_PWM, g_fanDuty);
  analogWrite(PIN_HEATER_PWM, g_heaterDuty);
}

// ════════════════════════════════════════════════════════════════════
//  DRYING LOGIC
// ════════════════════════════════════════════════════════════════════
void checkDryingComplete() {
  if (!g_dryingActive || g_dryingDone) return;
  if (g_weightLossPct >= WEIGHT_LOSS_PCT && g_humidity <= HUMIDITY_DRY_THRESH) {
    g_dryingDone = true;
    stopDrying();
    gsmSendSMS("DRYER COMPLETE. Loss=" + String(g_weightLossPct, 1) + "% RH=" + String(g_humidity, 1) + "%");
  }
}
void startDrying() {
  if (!g_dhtOK || !g_scaleOK) {
    gsmSendSMS("DRYER ERR: Cannot start. DHT:" + String(g_dhtOK ? "OK" : "FAIL") + " Scale:" + String(g_scaleOK ? "OK" : "FAIL"));
    return;
  }
  g_initWeightKg = g_weightKg;
  g_initHumidity = g_humidity;
  g_dryingActive = true;
  g_dryingDone = false;
  g_dryStartMs = millis();
  saveToEEPROM();
  gsmSendSMS("DRYER STARTED. Wt=" + String(g_initWeightKg, 3) + "kg RH=" + String(g_initHumidity, 1) + "%");
  Serial.println(F("[DRYER] Started."));
}
void stopDrying() {
  g_dryingActive = false;
  saveToEEPROM();
  if (!g_dryingDone)
    gsmSendSMS("DRYER STOPPED. Loss=" + String(g_weightLossPct, 1) + "%");
  Serial.println(F("[DRYER] Stopped."));
}

// ════════════════════════════════════════════════════════════════════
//  LCD
// ════════════════════════════════════════════════════════════════════
void updateLCD() {
  if (!g_lcdOK) return;
  lcd.clear();
  char l1[17], l2[17];
  switch (g_lcdPage) {
    case 0:
      snprintf(l1, 17, "T:%5.1fC H:%3.0f%%", g_tempC, g_humidity);
      snprintf(l2, 17, "Wt:%.2fkg Dr:%s", g_weightKg, g_doorOpen ? "OPEN" : "CLSD");
      break;
    case 1:
      snprintf(l1, 17, "Loss:%5.1f%% Drop:%3.0f%%", g_weightLossPct, g_humDropPct);
      snprintf(l2, 17, "%s", g_dryingActive ? g_dryingDone ? "DONE" : "DRYING" : "IDLE");
      break;
    case 2:
      snprintf(l1, 17, "Fan:%3d%% Heat:%3d%%", (g_fanDuty * 100) / 255, (g_heaterDuty * 100) / 255);
      snprintf(l2, 17, "IP:%s", g_localIP.substring(0, 14).c_str());
      break;
  }
  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
  g_lcdPage = (g_lcdPage + 1) % 3;
}

// ════════════════════════════════════════════════════════════════════
//  GSM
// ════════════════════════════════════════════════════════════════════
void gsmSendSMS(const String &msg) {
  g_pendingSmsText = msg;
  g_smsPendingStart = true;
  Serial.print(F("[GSM] Queued: "));
  Serial.println(msg);
}
void gsmProcess() {
  if (!g_smsPendingStart) return;
  unsigned long now = millis();
  if (g_lastGsmRetryMs && now - g_lastGsmRetryMs < GSM_RETRY_MS) return;
  g_lastGsmRetryMs = now;
  gsmSerial.println(F("AT"));
  delay(300);
  if (!gsmSerial.find("OK")) {
    Serial.println(F("[GSM] No resp"));
    return;
  }
  gsmSerial.println(F("AT+CMGF=1"));
  delay(300);
  gsmSerial.print(F("AT+CMGS=\""));
  gsmSerial.print(SMS_NUMBER);
  gsmSerial.println(F("\""));
  delay(500);
  gsmSerial.print(g_pendingSmsText);
  gsmSerial.write(0x1A);
  delay(3000);
  if (gsmSerial.find("+CMGS:")) {
    Serial.println(F("[GSM] OK"));
    g_smsPendingStart = false;
    g_pendingSmsText = "";
  } else Serial.println(F("[GSM] Failed — retry"));
}

// ════════════════════════════════════════════════════════════════════
//  DEFERRED SELF-CHECK
// ════════════════════════════════════════════════════════════════════
void runDeferredInit() {
  if (g_selfCheckDone) return;
  unsigned long now = millis();
  static bool lcdChk = false, dhtChk = false, gsmChk = false;
  if (!lcdChk && now >= 500) {
    lcdChk = true;
    g_lcdOK = detectI2C(0x27) || detectI2C(0x3F);
    Serial.print(F("[CHK] LCD: "));
    Serial.println(g_lcdOK ? F("OK") : F("FAIL"));
  }
  if (!dhtChk && now >= 2000) {
    dhtChk = true;
    float t = dht.readTemperature(), h = dht.readHumidity();
    g_dhtOK = (!isnan(t) && !isnan(h));
    Serial.print(F("[CHK] DHT11: "));
    Serial.println(g_dhtOK ? F("OK") : F("FAIL"));
  }
  if (!g_scaleTared && scale.is_ready()) {
    g_scaleTared = true;
    g_scaleOK = true;
    scale.tare();
    Serial.println(F("[CHK] HX711: OK — tared"));
  }
  if (!gsmChk && dhtChk && now >= GSM_BOOT_MS) {
    gsmChk = true;
    String err = "";
    if (!g_dhtOK) err += "DHT11 FAIL; ";
    if (!g_lcdOK) err += "LCD FAIL; ";
    if (!g_scaleOK && !scale.is_ready()) err += "HX711 FAIL; ";
    if (err.length()) gsmSendSMS("DRYER BOOT ERR: " + err);
    else Serial.println(F("[CHK] All OK"));
    g_selfCheckDone = true;
  }
}
bool detectI2C(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// ════════════════════════════════════════════════════════════════════
//  WI-FI CONNECTION (Scans 5 networks, shows connected SSID and IP)
// ════════════════════════════════════════════════════════════════════
void handleWiFiConnect() {
  if (g_wifiReady) return;

  Serial.println(F("\n[WIFI] Scanning for networks..."));

  for (uint8_t i = 0; i < WIFI_NETWORK_COUNT; i++) {
    Serial.printf("[WIFI] Trying SSID: %s\n", WIFI_NETWORKS[i].ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      g_connectedSSID = WIFI_NETWORKS[i].ssid;
      g_localIP = WiFi.localIP().toString();
      g_wifiReady = true;

      Serial.println(F("\n╔══════════════════════════════════════════════════════════╗"));
      Serial.println(F("║                    WI-FI CONNECTED                       ║"));
      Serial.println(F("╠══════════════════════════════════════════════════════════╣"));
      Serial.printf("║  SSID: %-40s ║\n", g_connectedSSID.c_str());
      Serial.printf("║  IP Address: %-37s ║\n", g_localIP.c_str());
      Serial.printf("║  Gateway: %-39s ║\n", WiFi.gatewayIP().toString().c_str());
      Serial.printf("║  RSSI: %d dBm %-38s ║\n", WiFi.RSSI(), "");
      Serial.println(F("╚══════════════════════════════════════════════════════════╝\n"));

      // Update LCD with connection info
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(g_connectedSSID.substring(0, 16));
      lcd.setCursor(0, 1);
      lcd.print(g_localIP.substring(0, 16));
      delay(3000);

      ntpClient.begin();
      server.begin();
      Serial.println(F("[WEB] Server started."));
      return;
    }
  }

  Serial.println(F("[WIFI] Failed to connect to any network - running offline"));
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi FAILED!");
  lcd.setCursor(0, 1);
  lcd.print("Offline Mode");
  delay(3000);
}

// ════════════════════════════════════════════════════════════════════
//  TABLE MANAGEMENT (Auto-growing every 1.5 seconds)
// ════════════════════════════════════════════════════════════════════
void addTableRecord() {
  TableRecord record;
  record.timestamp = String(g_ntpTime);
  record.temp = g_tempC;
  record.hum = g_humidity;
  record.weight = g_weightKg;
  record.loss = g_weightLossPct;
  record.drop = g_humDropPct;
  record.fan = (g_fanDuty * 100) / 255;
  record.heat = (g_heaterDuty * 100) / 255;
  record.door = g_doorOpen ? "OPEN" : "CLSD";

  if (g_dryingDone) record.status = "DONE";
  else if (g_dryingActive) record.status = "DRYING";
  else record.status = "IDLE";

  unsigned long elapsed = g_dryingActive ? (millis() - g_dryStartMs) / 1000UL : 0;
  int hours = elapsed / 3600;
  int mins = (elapsed % 3600) / 60;
  record.elapsed = String(hours) + "h " + String(mins) + "m";

  // Shift records down and add new one at the top
  for (int i = MAX_TABLE_RECORDS - 1; i > 0; i--) {
    tableRecords[i] = tableRecords[i - 1];
  }
  tableRecords[0] = record;
  if (tableRecordCount < MAX_TABLE_RECORDS) tableRecordCount++;
}

String getTableHTML() {
  String html = "<div style='overflow-x:auto; max-height:400px; overflow-y:auto;'>"
                "<table style='width:100%; border-collapse:collapse; font-size:0.75rem;'>"
                "<thead style='position:sticky; top:0; background:#4a148c;'>"
                "<tr style='color:#ffd966;'>"
                "<th>TIME</th><th>TEMP</th><th>HUM</th><th>WEIGHT</th><th>LOSS</th><th>DROP</th><th>FAN</th><th>HEAT</th><th>DOOR</th><th>STATUS</th><th>ELAPSED</th>"
                "</tr></thead><tbody>";

  for (int i = 0; i < tableRecordCount; i++) {
    String statusClass = "";
    if (tableRecords[i].status == "DONE") statusClass = "style='color:#4caf50'";
    else if (tableRecords[i].status == "DRYING") statusClass = "style='color:#ff9800'";
    else statusClass = "style='color:#78909c'";

    html += "<tr>";
    html += "<td>" + tableRecords[i].timestamp + "</td>";
    html += "<td>" + String(tableRecords[i].temp, 1) + "°C</td>";
    html += "<td>" + String(tableRecords[i].hum, 0) + "%</td>";
    html += "<td>" + String(tableRecords[i].weight, 3) + "kg</td>";
    html += "<td>" + String(tableRecords[i].loss, 1) + "%</td>";
    html += "<td>" + String(tableRecords[i].drop, 1) + "%</td>";
    html += "<td>" + String(tableRecords[i].fan) + "%</td>";
    html += "<td>" + String(tableRecords[i].heat) + "%</td>";
    html += "<td>" + tableRecords[i].door + "</td>";
    html += "<td " + statusClass + ">" + tableRecords[i].status + "</td>";
    html += "<td>" + tableRecords[i].elapsed + "</td>";
    html += "</tr>";
  }
  html += "</tbody></table></div>";
  return html;
}

// ════════════════════════════════════════════════════════════════════
//  WEB HANDLERS
// ════════════════════════════════════════════════════════════════════
void handleRoot() {
  if (!g_loggedIn && !g_guestMode) {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>Silver Fish Dryer</title><style>"
                  "*{margin:0;padding:0;box-sizing:border-box}"
                  "body{background:linear-gradient(135deg,#4a148c 0%,#1a237e 100%);"
                  "color:#e8f5e9;font-family:'Segoe UI',sans-serif;min-height:100vh;display:flex;"
                  "align-items:center;justify-content:center}"
                  ".box{background:linear-gradient(145deg,#311b92,#1a237e);border-radius:20px;"
                  "padding:44px 40px;min-width:320px;text-align:center;box-shadow:0 20px 40px rgba(0,0,0,0.5)}"
                  "h2{color:#ffd966;letter-spacing:4px;margin-bottom:4px}"
                  ".sub{color:#90caf9;font-size:.75rem;margin-bottom:28px}"
                  "input{width:100%;padding:12px;margin:5px 0 14px;background:#1a237e;"
                  "border:2px solid #7c4dff;border-radius:8px;color:#e8f5e9}"
                  "input:focus{outline:none;border-color:#ffd966}"
                  "button{width:100%;padding:13px;background:linear-gradient(135deg,#7c4dff,#4a148c);"
                  "color:#fff;border:none;border-radius:8px;font-weight:700;cursor:pointer;margin-top:10px}"
                  ".guest-btn{background:linear-gradient(135deg,#42a5f5,#1565c0)}"
                  "</style></head><body><div class='box'>"
                  "<h2>🐟 SILVER FISH DRYER</h2>"
                  "<div class='sub'>MONITORING & CONTROL SYSTEM</div>"
                  "<form method='POST' action='/login'>"
                  "<input type='text' name='u' placeholder='ADMIN USERNAME' required>"
                  "<input type='password' name='p' placeholder='ADMIN PASSWORD' required>"
                  "<button type='submit'>🔓 ADMIN ACCESS</button>"
                  "</form>"
                  "<form method='POST' action='/guest' style='margin-top:15px'>"
                  "<button type='submit' class='guest-btn'>👁️ GUEST ACCESS (VIEW ONLY)</button>"
                  "</form></div></body></html>";
    server.send(200, "text/html", html);
  } else {
    sendDashboard();
  }
}

void handleLogin() {
  String u = server.arg("u");
  String p = server.arg("p");
  if (u == WEB_USER && p == WEB_PASS) {
    g_loggedIn = true;
    g_guestMode = false;
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  } else {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<title>Login Failed</title><style>body{background:#4a148c;color:#e8f5e9;"
                  "font-family:sans-serif;display:flex;align-items:center;justify-content:center;height:100vh}"
                  "</style></head><body><div style='text-align:center'><h2 style='color:#ff6b6b'>✗ LOGIN FAILED</h2>"
                  "<p>Invalid credentials.</p><a href='/' style='color:#ffd966'>← BACK</a>"
                  "</div></body></html>";
    server.send(401, "text/html", html);
  }
}

void handleGuestLogin() {
  g_guestMode = true;
  g_loggedIn = false;
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleLogout() {
  g_loggedIn = false;
  g_guestMode = false;
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleData() {
  if (!g_loggedIn && !g_guestMode) {
    server.send(403, "text/plain", "Forbidden");
    return;
  }
  unsigned long elapsed = g_dryingActive ? (millis() - g_dryStartMs) / 1000UL : 0;
  char buf[600];
  snprintf(buf, sizeof(buf),
           "{\"temp\":%.2f,\"hum\":%.2f,\"wt\":%.3f,\"loss\":%.1f,"
           "\"hdrop\":%.1f,\"fan\":%d,\"heat\":%d,"
           "\"active\":%s,\"done\":%s,\"dhtOK\":%s,\"scaleOK\":%s,"
           "\"door\":%s,\"elapsed\":%lu,\"initW\":%.3f,\"initH\":%.1f,"
           "\"time\":\"%s\",\"date\":\"%s\",\"progress\":%.1f,\"guest\":%s,"
           "\"ssid\":\"%s\",\"ip\":\"%s\"}",
           g_tempC, g_humidity, g_weightKg, g_weightLossPct,
           g_humDropPct,
           (g_fanDuty * 100) / 255, (g_heaterDuty * 100) / 255,
           g_dryingActive ? "true" : "false",
           g_dryingDone ? "true" : "false",
           g_dhtOK ? "true" : "false",
           g_scaleOK ? "true" : "false",
           g_doorOpen ? "OPEN" : "CLOSED",
           elapsed, g_initWeightKg, g_initHumidity,
           g_ntpTime, g_ntpDate,
           g_weightLossPct,
           g_guestMode ? "true" : "false",
           g_connectedSSID.c_str(), g_localIP.c_str());
  server.send(200, "application/json", buf);
}

void handleControl() {
  if (!g_loggedIn || g_guestMode) {
    server.send(403, "text/plain", "Forbidden");
    return;
  }
  if (server.hasArg("cmd")) {
    String cmd = server.arg("cmd");
    if (cmd == "start") startDrying();
    else if (cmd == "stop") stopDrying();
  }
  server.send(200, "text/plain", "OK");
}

void handleManualControl() {
  if (!g_loggedIn || g_guestMode) {
    server.send(403, "text/plain", "Forbidden");
    return;
  }
  if (server.hasArg("device") && server.hasArg("state")) {
    String device = server.arg("device");
    int state = server.arg("state").toInt();

    if (device == "fan") {
      if (state == 1) {
        g_fanDuty = FAN_DUTY_DRYING;
        analogWrite(PIN_FAN_PWM, g_fanDuty);
      } else {
        g_fanDuty = 0;
        analogWrite(PIN_FAN_PWM, 0);
      }
    } else if (device == "heater") {
      if (state == 1) {
        g_heaterDuty = 255;
        analogWrite(PIN_HEATER_PWM, g_heaterDuty);
      } else {
        g_heaterDuty = 0;
        analogWrite(PIN_HEATER_PWM, 0);
      }
    }
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void sendTablePage() {
  if (!g_loggedIn && !g_guestMode) {
    server.send(403, "text/plain", "Forbidden");
    return;
  }

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Silver Fish Dryer - Data Log</title>"
                "<style>"
                "*{margin:0;padding:0;box-sizing:border-box}"
                "body{background:linear-gradient(135deg,#4a148c 0%,#1a237e 100%);"
                "font-family:'Segoe UI',sans-serif;color:#e8f5e9}"
                ".header{background:linear-gradient(145deg,#311b92,#1a237e);padding:15px 25px;"
                "display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;border-bottom:3px solid #ffd966}"
                ".logo h1{color:#ffd966;font-size:1.3rem}"
                ".logo p{color:#90caf9;font-size:0.7rem}"
                ".nav-btn{padding:8px 20px;background:linear-gradient(135deg,#7c4dff,#4a148c);"
                "color:white;text-decoration:none;border-radius:20px}"
                ".logout-btn{background:transparent;border:1px solid #ff6b6b;color:#ff6b6b;padding:8px 20px;border-radius:20px;cursor:pointer}"
                ".container{max-width:1400px;margin:0 auto;padding:20px}"
                ".table-wrapper{background:linear-gradient(145deg,#311b92,#1a237e);border-radius:20px;padding:20px;overflow-x:auto}"
                "th{background:#4a148c;color:#ffd966;padding:12px;position:sticky;top:0}"
                "td{padding:10px 12px;border-bottom:1px solid #7c4dff}"
                "tr:hover{background:#4a148c}"
                ".footer{text-align:center;padding:20px;color:#90caf9;font-size:0.7rem}"
                "</style></head><body>"
                "<div class='header'><div class='logo'><h1>🐟 SILVER FISH DRYER</h1><p>DATA LOGGER</p></div>"
                "<div><a href='/' class='nav-btn'>📊 DASHBOARD</a> "
                "<button class='logout-btn' onclick='logout()'>🚪 LOGOUT</button></div></div>"
                "<div class='container'><div class='table-wrapper' id='tableContainer'>Loading...</div>"
                "<div class='footer'><p>📋 Auto-refreshes every 1.5 seconds | Last "
                + String(MAX_TABLE_RECORDS) + " records</p></div></div>"
                                              "<script>function logout(){window.location.href='/logout'}"
                                              "setInterval(async()=>{try{const r=await fetch('/tablehtml');const h=await r.text();"
                                              "document.getElementById('tableContainer').innerHTML=h}catch(e){}},1500);"
                                              "</script></body></html>";
  server.send(200, "text/html", html);
}

void handleTableHTML() {
  if (!g_loggedIn && !g_guestMode) {
    server.send(403, "text/plain", "Forbidden");
    return;
  }
  server.send(200, "text/html", getTableHTML());
}

void sendDashboard() {
  String guestWarning = g_guestMode ? "<div style='background:#ff9800; color:#000; padding:10px; text-align:center; border-radius:10px; margin-bottom:15px'>"
                                      "👁️ GUEST MODE - View Only. Controls are disabled.</div>"
                                    : "";

  String controlButtons = g_guestMode ? "<div class='button-group'><button class='glow-btn btn-start' disabled style='opacity:0.5'>▶ AUTO START</button>"
                                        "<button class='glow-btn btn-stop' disabled style='opacity:0.5'>■ AUTO STOP</button>"
                                        "<button class='glow-btn btn-fan off' disabled style='opacity:0.5'>🌀 FAN OFF</button>"
                                        "<button class='glow-btn btn-heater off' disabled style='opacity:0.5'>🔥 HEATER OFF</button></div>"
                                      : "<div class='button-group'>"
                                        "<button class='glow-btn btn-start' id='autoStartBtn' onclick='autoStart()'>▶ AUTO START</button>"
                                        "<button class='glow-btn btn-stop' onclick='autoStop()'>■ AUTO STOP</button>"
                                        "<button class='glow-btn btn-fan' id='fanBtn' onclick='toggleFan()'>🌀 FAN OFF</button>"
                                        "<button class='glow-btn btn-heater' id='heaterBtn' onclick='toggleHeater()'>🔥 HEATER OFF</button>"
                                        "</div>";

  String guestFlag = g_guestMode ? "true" : "false";

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Silver Fish Dryer Monitoring System</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      background: linear-gradient(135deg, #4a148c 0%, #1a237e 100%);
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      color: #e8f5e9;
    }
    .header {
      background: linear-gradient(145deg, #311b92, #1a237e);
      padding: 15px 25px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      border-bottom: 3px solid #ffd966;
      box-shadow: 0 5px 20px rgba(0,0,0,0.3);
    }
    .logo h1 { color: #ffd966; font-size: 1.3rem; letter-spacing: 3px; text-shadow: 0 0 15px rgba(255,217,102,0.3); }
    .logo p { color: #90caf9; font-size: 0.7rem; letter-spacing: 2px; }
    .status-bar { display: flex; gap: 20px; align-items: center; flex-wrap: wrap; }
    .live-badge { background: rgba(76,175,80,0.2); padding: 5px 12px; border-radius: 20px; border: 1px solid #4caf50; animation: pulse 1.5s ease-in-out infinite; }
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
    .time-date { background: #1a237e; padding: 5px 12px; border-radius: 10px; font-family: monospace; color: #ffd966; }
    .wifi-info { background: #1a237e; padding: 5px 12px; border-radius: 10px; font-size: 0.7rem; }
    .nav-buttons { display: flex; gap: 10px; }
    .nav-btn { padding: 6px 15px; background: linear-gradient(135deg, #7c4dff, #4a148c); color: white; text-decoration: none; border-radius: 20px; font-size: 0.8rem; }
    .logout-btn { background: transparent; border: 1px solid #ff6b6b; color: #ff6b6b; padding: 6px 15px; border-radius: 20px; cursor: pointer; }
    .container { max-width: 1400px; margin: 0 auto; padding: 20px; }
    .gauge-row { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 20px; margin-bottom: 20px; }
    .gauge-card { background: linear-gradient(145deg, #311b92, #1a237e); border-radius: 20px; padding: 15px; text-align: center; border: 1px solid #7c4dff; transition: transform 0.3s; }
    .gauge-card:hover { transform: translateY(-5px); border-color: #ffd966; }
    .gauge-title { font-size: 0.7rem; color: #90caf9; letter-spacing: 2px; margin-bottom: 10px; }
    canvas { margin: 10px auto; }
    .gauge-value { font-size: 1.5rem; font-weight: bold; color: #ffd966; margin-top: 10px; }
    .stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 15px; margin-bottom: 20px; }
    .stat-card { background: linear-gradient(145deg, #311b92, #1a237e); border-radius: 15px; padding: 15px; text-align: center; border-left: 4px solid #7c4dff; }
    .stat-label { font-size: 0.65rem; color: #90caf9; letter-spacing: 1px; }
    .stat-value { font-size: 1.3rem; font-weight: bold; color: #ffd966; }
    .control-panel { background: linear-gradient(145deg, #311b92, #1a237e); border-radius: 20px; padding: 20px; margin-bottom: 20px; }
    .control-title { font-size: 0.9rem; color: #ffd966; letter-spacing: 2px; margin-bottom: 15px; border-bottom: 1px solid #7c4dff; padding-bottom: 10px; }
    .button-group { display: flex; gap: 15px; flex-wrap: wrap; justify-content: center; }
    .glow-btn { padding: 10px 25px; font-size: 0.9rem; font-weight: bold; border: none; border-radius: 50px; cursor: pointer; transition: all 0.3s; text-transform: uppercase; letter-spacing: 1px; }
    .btn-start { background: linear-gradient(135deg, #4caf50, #2e7d32); color: white; box-shadow: 0 0 15px rgba(76,175,80,0.5); }
    .btn-start:hover { transform: translateY(-3px); box-shadow: 0 0 30px rgba(76,175,80,0.8); }
    .btn-stop { background: linear-gradient(135deg, #ff6b6b, #c0392b); color: white; box-shadow: 0 0 15px rgba(255,107,107,0.5); }
    .btn-stop:hover { transform: translateY(-3px); box-shadow: 0 0 30px rgba(255,107,107,0.8); }
    .btn-fan { background: linear-gradient(135deg, #2196f3, #0d47a1); color: white; }
    .btn-heater { background: linear-gradient(135deg, #ff9800, #e65100); color: white; }
    .btn-fan.off, .btn-heater.off { background: linear-gradient(135deg, #555, #333); box-shadow: none; }
    .glow-btn:disabled { opacity: 0.5; cursor: not-allowed; transform: none; }
    .door-closed { color: #4caf50; }
    .door-open { color: #ff6b6b; animation: blink 1s step-end infinite; }
    @keyframes blink { 50% { opacity: 0.3; } }
    .charts-section { display: grid; grid-template-columns: repeat(auto-fit, minmax(350px, 1fr)); gap: 20px; margin-top: 20px; }
    .chart-card { background: linear-gradient(145deg, #311b92, #1a237e); border-radius: 20px; padding: 15px; }
    .chart-card h3 { color: #ffd966; font-size: 0.8rem; margin-bottom: 10px; letter-spacing: 1px; }
    .progress-container { margin-top: 10px; }
    .progress-label { display: flex; justify-content: space-between; font-size: 0.6rem; margin-bottom: 3px; }
    .progress-bar { height: 6px; background: #1a237e; border-radius: 10px; overflow: hidden; }
    .progress-fill { height: 100%; background: linear-gradient(90deg, #4caf50, #ffd966); border-radius: 10px; transition: width 0.5s ease; }
    @keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
    @keyframes heat-wave { 0% { opacity: 0.3; } 50% { opacity: 1; } 100% { opacity: 0.3; } }
    @media (max-width: 768px) { .header { flex-direction: column; gap: 10px; text-align: center; } .charts-section { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <div class="header">
    <div class="logo">
      <h1>🐟 SILVER FISH DRYER</h1>
      <p>MONITORING & CONTROL SYSTEM | ESP32-S3</p>
    </div>
    <div class="status-bar">
      <div class="live-badge">● LIVE</div>
      <div class="time-date"><span id="clock">--:--:--</span></div>
      <div class="wifi-info" id="wifiInfo">📡 Connecting...</div>
      <div class="nav-buttons">
        <a href="/table" class="nav-btn">📋 DATA LOG</a>
        <button class="logout-btn" onclick="logout()">🚪 LOGOUT</button>
      </div>
    </div>
  </div>
  
  <div class="container">
    )rawliteral" + guestWarning
                + R"rawliteral(
    
    <div class="gauge-row">
      <div class="gauge-card">
        <div class="gauge-title">🌡️ TEMPERATURE</div>
        <canvas id="tempGauge" width="150" height="150"></canvas>
        <div class="gauge-value"><span id="tempVal">--.-</span><span style="font-size:0.8rem">°C</span></div>
        <div class="progress-container"><div class="progress-label"><span>0°C</span><span>Target 60°C</span><span>70°C</span></div>
        <div class="progress-bar"><div class="progress-fill" id="tempFill" style="width:0%"></div></div></div>
      </div>
      <div class="gauge-card">
        <div class="gauge-title">💧 HUMIDITY</div>
        <canvas id="humGauge" width="150" height="150"></canvas>
        <div class="gauge-value"><span id="humVal">--.-</span><span style="font-size:0.8rem">%</span></div>
        <div class="progress-container"><div class="progress-label"><span>0%</span><span>Target 25%</span><span>100%</span></div>
        <div class="progress-bar"><div class="progress-fill" id="humFill" style="width:0%"></div></div></div>
      </div>
      <div class="gauge-card">
        <div class="gauge-title">⚖️ DRYING PROGRESS</div>
        <canvas id="progressGauge" width="150" height="150"></canvas>
        <div class="gauge-value"><span id="progressVal">0</span><span style="font-size:0.8rem">%</span></div>
        <div class="progress-container"><div class="progress-label"><span>0%</span><span>Target 80%</span><span>100%</span></div>
        <div class="progress-bar"><div class="progress-fill" id="progressFill" style="width:0%"></div></div></div>
      </div>
      <div class="gauge-card">
        <div class="gauge-title">🌀 FAN SPEED</div>
        <div style="position:relative; width:150px; height:150px; margin:0 auto;">
          <svg width="150" height="150" viewBox="0 0 150 150">
            <circle cx="75" cy="75" r="70" fill="none" stroke="#311b92" stroke-width="8"/>
            <g id="fanBlade" style="transform-origin:75px 75px">
              <ellipse cx="75" cy="25" rx="15" ry="35" fill="#2196f3" opacity="0.8"/>
              <ellipse cx="75" cy="125" rx="15" ry="35" fill="#2196f3" opacity="0.8" transform="rotate(180 75 75)"/>
              <ellipse cx="25" cy="75" rx="35" ry="15" fill="#2196f3" opacity="0.8" transform="rotate(90 75 75)"/>
              <ellipse cx="125" cy="75" rx="35" ry="15" fill="#2196f3" opacity="0.8" transform="rotate(270 75 75)"/>
            </g>
            <circle cx="75" cy="75" r="15" fill="#ffd966"/>
          </svg>
        </div>
        <div class="gauge-value"><span id="fanVal">0</span><span style="font-size:0.8rem">%</span></div>
      </div>
      <div class="gauge-card">
        <div class="gauge-title">🔥 HEATER POWER</div>
        <div style="position:relative; width:150px; height:150px; margin:0 auto;">
          <svg width="150" height="150" viewBox="0 0 150 150">
            <rect x="60" y="20" width="30" height="110" rx="15" fill="#311b92" stroke="#ff9800" stroke-width="3"/>
            <g id="heaterWave">
              <rect x="65" y="25" width="20" height="20" rx="5" fill="#ff9800" opacity="0.8"/>
              <rect x="65" y="55" width="20" height="20" rx="5" fill="#ff9800" opacity="0.6"/>
              <rect x="65" y="85" width="20" height="20" rx="5" fill="#ff9800" opacity="0.4"/>
            </g>
          </svg>
        </div>
        <div class="gauge-value"><span id="heaterVal">0</span><span style="font-size:0.8rem">%</span></div>
      </div>
    </div>
    
    <div class="stats-grid">
      <div class="stat-card"><div class="stat-label">📊 WEIGHT LOSS</div><div class="stat-value"><span id="lossVal">0</span>%</div></div>
      <div class="stat-card"><div class="stat-label">💧 HUMIDITY DROP</div><div class="stat-value"><span id="dropVal">0</span>%</div></div>
      <div class="stat-card"><div class="stat-label">🚪 DOOR STATUS</div><div class="stat-value"><span id="doorVal" class="door-closed">CLOSED</span></div></div>
      <div class="stat-card"><div class="stat-label">⏱️ ELAPSED TIME</div><div class="stat-value"><span id="elapsedTime">0h 0m</span></div></div>
      <div class="stat-card"><div class="stat-label">⚖️ INITIAL WEIGHT</div><div class="stat-value"><span id="initWt">0</span> kg</div></div>
      <div class="stat-card"><div class="stat-label">🎯 TARGET LOSS</div><div class="stat-value">80%</div></div>
    </div>
    
    <div class="control-panel">
      <div class="control-title">🎮 MANUAL CONTROLS</div>
      )rawliteral"
                + controlButtons + R"rawliteral(
    </div>
    
    <div class="charts-section">
      <div class="chart-card"><h3>🌡️ TEMPERATURE TREND</h3><canvas id="tempChart"></canvas></div>
      <div class="chart-card"><h3>💧 HUMIDITY TREND</h3><canvas id="humChart"></canvas></div>
      <div class="chart-card"><h3>⚖️ WEIGHT TREND</h3><canvas id="weightChart"></canvas></div>
      <div class="chart-card"><h3>📉 LOSS PERCENTAGE TREND</h3><canvas id="lossChart"></canvas></div>
      <div class="chart-card"><h3>🌀 FAN vs HEATER</h3><canvas id="fanHeaterChart"></canvas></div>
      <div class="chart-card"><h3>📊 HUMIDITY DROP TREND</h3><canvas id="dropChart"></canvas></div>
    </div>
  </div>
  
  <script>
    const isGuest = )rawliteral"
                + guestFlag + R"rawliteral(;
    let tempGauge, humGauge, progressGauge;
    let tempChart, humChart, weightChart, lossChart, fanHeaterChart, dropChart;
    let historyData = { time: [], temp: [], hum: [], weight: [], loss: [], fan: [], heat: [], drop: [] };
    
    function initGauges() {
      tempGauge = new Chart(document.getElementById('tempGauge'), { type: 'doughnut', data: { datasets: [{ data: [0, 100], backgroundColor: ['#ffd966', '#311b92'], borderWidth: 0 }] }, options: { cutout: '70%', responsive: true, plugins: { tooltip: { enabled: false } } } });
      humGauge = new Chart(document.getElementById('humGauge'), { type: 'doughnut', data: { datasets: [{ data: [0, 100], backgroundColor: ['#4caf50', '#311b92'], borderWidth: 0 }] }, options: { cutout: '70%', responsive: true, plugins: { tooltip: { enabled: false } } } });
      progressGauge = new Chart(document.getElementById('progressGauge'), { type: 'doughnut', data: { datasets: [{ data: [0, 100], backgroundColor: ['#ffd966', '#311b92'], borderWidth: 0 }] }, options: { cutout: '70%', responsive: true, plugins: { tooltip: { enabled: false } } } });
    }
    
    function initCharts() {
      tempChart = new Chart(document.getElementById('tempChart'), { type: 'line', data: { labels: [], datasets: [{ label: 'Temperature (°C)', data: [], borderColor: '#ffd966', backgroundColor: 'rgba(255,217,102,0.1)', tension: 0.3, fill: true }] }, options: { responsive: true, plugins: { legend: { labels: { color: '#e8f5e9' } } } } });
      humChart = new Chart(document.getElementById('humChart'), { type: 'line', data: { labels: [], datasets: [{ label: 'Humidity (%)', data: [], borderColor: '#4caf50', backgroundColor: 'rgba(76,175,80,0.1)', tension: 0.3, fill: true }] }, options: { responsive: true, plugins: { legend: { labels: { color: '#e8f5e9' } } } } });
      weightChart = new Chart(document.getElementById('weightChart'), { type: 'line', data: { labels: [], datasets: [{ label: 'Weight (kg)', data: [], borderColor: '#2196f3', backgroundColor: 'rgba(33,150,243,0.1)', tension: 0.3, fill: true }] }, options: { responsive: true, plugins: { legend: { labels: { color: '#e8f5e9' } } } } });
      lossChart = new Chart(document.getElementById('lossChart'), { type: 'line', data: { labels: [], datasets: [{ label: 'Loss (%)', data: [], borderColor: '#ff9800', backgroundColor: 'rgba(255,152,0,0.1)', tension: 0.3, fill: true }] }, options: { responsive: true, plugins: { legend: { labels: { color: '#e8f5e9' } } } } });
      fanHeaterChart = new Chart(document.getElementById('fanHeaterChart'), { type: 'line', data: { labels: [], datasets: [{ label: 'Fan (%)', data: [], borderColor: '#2196f3', tension: 0.3 }, { label: 'Heater (%)', data: [], borderColor: '#ff9800', tension: 0.3 }] }, options: { responsive: true, plugins: { legend: { labels: { color: '#e8f5e9' } } } } });
      dropChart = new Chart(document.getElementById('dropChart'), { type: 'line', data: { labels: [], datasets: [{ label: 'Humidity Drop (%)', data: [], borderColor: '#90caf9', backgroundColor: 'rgba(144,202,249,0.1)', tension: 0.3, fill: true }] }, options: { responsive: true, plugins: { legend: { labels: { color: '#e8f5e9' } } } } });
    }
    
    function updateGauges(temp, hum, progress) {
      if (tempGauge) { tempGauge.data.datasets[0].data = [Math.min(100, temp / 70 * 100), 100 - Math.min(100, temp / 70 * 100)]; tempGauge.update(); }
      if (humGauge) { humGauge.data.datasets[0].data = [Math.min(100, hum), 100 - Math.min(100, hum)]; humGauge.update(); }
      if (progressGauge) { progressGauge.data.datasets[0].data = [Math.min(100, progress), 100 - Math.min(100, progress)]; progressGauge.update(); }
    }
    
    function updateUI(data) {
      document.getElementById('tempVal').innerHTML = data.temp.toFixed(1);
      document.getElementById('humVal').innerHTML = data.hum.toFixed(1);
      document.getElementById('lossVal').innerHTML = data.loss.toFixed(1);
      document.getElementById('dropVal').innerHTML = data.hdrop.toFixed(1);
      document.getElementById('progressVal').innerHTML = data.loss.toFixed(1);
      document.getElementById('fanVal').innerHTML = data.fan;
      document.getElementById('heaterVal').innerHTML = data.heat;
      document.getElementById('clock').innerHTML = data.time;
      document.getElementById('wifiInfo').innerHTML = '📡 ' + data.ssid + ' | ' + data.ip;
      document.getElementById('initWt').innerHTML = data.initW.toFixed(3);
      
      const doorEl = document.getElementById('doorVal');
      doorEl.innerHTML = data.door;
      doorEl.className = data.door === 'OPEN' ? 'door-open' : 'door-closed';
      
      const hours = Math.floor(data.elapsed / 3600);
      const mins = Math.floor((data.elapsed % 3600) / 60);
      document.getElementById('elapsedTime').innerHTML = hours + 'h ' + mins + 'm';
      
      document.getElementById('tempFill').style.width = Math.min(100, data.temp / 70 * 100) + '%';
      document.getElementById('humFill').style.width = Math.min(100, data.hum) + '%';
      document.getElementById('progressFill').style.width = Math.min(100, data.loss) + '%';
      
      updateGauges(data.temp, data.hum, data.loss);
      
      const fanBlade = document.getElementById('fanBlade');
      const heaterWave = document.getElementById('heaterWave');
      if (fanBlade) fanBlade.style.animation = data.fan > 0 ? 'spin 1s linear infinite' : 'none';
      if (heaterWave) heaterWave.style.animation = data.heat > 0 ? 'heat-wave 0.8s ease-in-out infinite' : 'none';
      
      if (!isGuest) {
        const fanBtn = document.getElementById('fanBtn');
        const heaterBtn = document.getElementById('heaterBtn');
        if (fanBtn) { fanBtn.innerHTML = data.fan > 0 ? '🌀 FAN ON' : '🌀 FAN OFF'; fanBtn.classList.toggle('off', data.fan === 0); }
        if (heaterBtn) { heaterBtn.innerHTML = data.heat > 0 ? '🔥 HEATER ON' : '🔥 HEATER OFF'; heaterBtn.classList.toggle('off', data.heat === 0); }
        const autoStartBtn = document.getElementById('autoStartBtn');
        if (autoStartBtn && data.active && !data.done) { autoStartBtn.innerHTML = '⏸ DRYING...'; autoStartBtn.disabled = true; autoStartBtn.style.opacity = '0.6'; }
        else if (autoStartBtn) { autoStartBtn.innerHTML = '▶ AUTO START'; autoStartBtn.disabled = false; autoStartBtn.style.opacity = '1'; }
      }
      
      historyData.time.push(data.time);
      historyData.temp.push(data.temp);
      historyData.hum.push(data.hum);
      historyData.weight.push(data.wt);
      historyData.loss.push(data.loss);
      historyData.fan.push(data.fan);
      historyData.heat.push(data.heat);
      historyData.drop.push(data.hdrop);
      if (historyData.time.length > 30) { for (let k in historyData) historyData[k].shift(); }
      
      if (tempChart) { tempChart.data.labels = historyData.time; tempChart.data.datasets[0].data = historyData.temp; tempChart.update(); }
      if (humChart) { humChart.data.labels = historyData.time; humChart.data.datasets[0].data = historyData.hum; humChart.update(); }
      if (weightChart) { weightChart.data.labels = historyData.time; weightChart.data.datasets[0].data = historyData.weight; weightChart.update(); }
      if (lossChart) { lossChart.data.labels = historyData.time; lossChart.data.datasets[0].data = historyData.loss; lossChart.update(); }
      if (fanHeaterChart) { fanHeaterChart.data.labels = historyData.time; fanHeaterChart.data.datasets[0].data = historyData.fan; fanHeaterChart.data.datasets[1].data = historyData.heat; fanHeaterChart.update(); }
      if (dropChart) { dropChart.data.labels = historyData.time; dropChart.data.datasets[0].data = historyData.drop; dropChart.update(); }
    }
    
    async function fetchData() {
      try { const res = await fetch('/data'); const data = await res.json(); updateUI(data); }
      catch(e) { console.error('Fetch error:', e); }
    }
    
    async function autoStart() { if (!isGuest) { await fetch('/ctrl?cmd=start'); setTimeout(fetchData, 500); } }
    async function autoStop() { if (!isGuest) { await fetch('/ctrl?cmd=stop'); setTimeout(fetchData, 500); } }
    async function toggleFan() { if (!isGuest) { const btn = document.getElementById('fanBtn'); const isOn = btn.innerHTML.includes('ON'); await fetch('/manual?device=fan&state=' + (isOn ? 0 : 1)); setTimeout(fetchData, 500); } }
    async function toggleHeater() { if (!isGuest) { const btn = document.getElementById('heaterBtn'); const isOn = btn.innerHTML.includes('ON'); await fetch('/manual?device=heater&state=' + (isOn ? 0 : 1)); setTimeout(fetchData, 500); } }
    function logout() { window.location.href = '/logout'; }
    
    initGauges(); initCharts(); fetchData(); setInterval(fetchData, 3000);
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n[BOOT] Silver Fish Dryer on ESP32-S3..."));

  EEPROM.begin(16);
  loadFromEEPROM();

  pinMode(PIN_FAN_PWM, OUTPUT);
  analogWrite(PIN_FAN_PWM, 0);
  pinMode(PIN_HEATER_PWM, OUTPUT);
  analogWrite(PIN_HEATER_PWM, 0);
  pinMode(PIN_DOOR_SENSOR, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SILVER FISH");
  lcd.setCursor(0, 1);
  lcd.print("DRYER SYSTEM");

  dht.begin();
  scale.begin(PIN_HX711_DOUT, PIN_HX711_SCK);
  scale.set_scale(LOADCELL_DIVIDER);

  gsmSerial.begin(9600, SERIAL_8N1, PIN_GSM_RX, PIN_GSM_TX);

  WiFi.mode(WIFI_STA);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/guest", HTTP_POST, handleGuestLogin);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/data", HTTP_GET, handleData);
  server.on("/ctrl", HTTP_GET, handleControl);
  server.on("/manual", HTTP_GET, handleManualControl);
  server.on("/table", HTTP_GET, sendTablePage);
  server.on("/tablehtml", HTTP_GET, handleTableHTML);
  server.begin();

  if (g_dryingActive) {
    g_dryStartMs = millis();
    Serial.println(F("[BOOT] Resuming from EEPROM."));
  }
  Serial.println(F("[BOOT] Ready."));
}

// ════════════════════════════════════════════════════════════════════
//  MAIN LOOP
// ════════════════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  handleWiFiConnect();
  runDeferredInit();

  if (now - g_lastSensorMs >= SENSOR_INTERVAL_MS) {
    g_lastSensorMs = now;
    updateNTP();
    readSensors();
    controlActuators();
    checkDryingComplete();

    if (g_dryingActive && (!g_dhtOK || !g_scaleOK)) {
      static unsigned long lastErrMs = 0;
      if (now - lastErrMs > 300000UL) {
        lastErrMs = now;
        gsmSendSMS("DRYER ALERT: Sensor fault! DHT:" + String(g_dhtOK ? "OK" : "FAIL") + " Scale:" + String(g_scaleOK ? "OK" : "FAIL"));
      }
    }
  }

  if (now - g_lastTableUpdateMs >= TABLE_UPDATE_MS) {
    g_lastTableUpdateMs = now;
    addTableRecord();
  }

  if (now - g_lastLcdMs >= LCD_INTERVAL_MS) {
    g_lastLcdMs = now;
    updateLCD();
  }

  if (now - g_lastEepromMs >= EEPROM_INTERVAL_MS) {
    g_lastEepromMs = now;
    if (g_dryingActive) saveToEEPROM();
  }

  gsmProcess();
  server.handleClient();
  yield();
}p() {
  // put your setup code here, to run once:

}

void loop() {
  // put your main code here, to run repeatedly:

}
