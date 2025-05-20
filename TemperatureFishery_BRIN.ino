#include <Wire.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <EEPROM.h>
#include <esp_now.h>

#define ONE_WIRE_BUS 18
#define RELAY_WATER_HEATER 21
#define RELAY_WATER_COOLER 22

const char* ssid = "Zoie";
const char* password = "ZOIEPAKEI";

WiFiUDP udp;
NTPClient timeClient(udp, "pool.ntp.org", 25200, 60000); 

RTC_DS3231 rtc;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensorSuhu(&oneWire);

float suhuMin = 26.0;
float suhuMax = 30.0;
float suhuMinHysteresis = 0.3;
float suhuMaxHysteresis = 0.3;

bool waterHeaterOn = false;
bool waterCoolerOn = false;
bool ntpTimeSet = false;

uint8_t peerAddress[] = {0x20, 0x43, 0xA8, 0x65, 0x71, 0x78};

#define MSG_A 1
#define MSG_B 2
#define MSG_SP 3

void setup() {
  Serial.begin(115200);
  Wire.begin(25, 26);

  pinMode(RELAY_WATER_HEATER, OUTPUT);
  pinMode(RELAY_WATER_COOLER, OUTPUT);
  digitalWrite(RELAY_WATER_HEATER, LOW);
  digitalWrite(RELAY_WATER_COOLER, LOW);

  sensorSuhu.begin();
  rtc.begin();

  EEPROM.begin(512);
  EEPROM.get(0, suhuMin);
  EEPROM.get(10, suhuMax);
  EEPROM.get(20, suhuMinHysteresis);
  EEPROM.get(30, suhuMaxHysteresis);

  if (isnan(suhuMin) || suhuMin == 0) suhuMin = 26.0;
  if (isnan(suhuMax) || suhuMax == 0) suhuMax = 30.0;
  if (isnan(suhuMinHysteresis) || suhuMinHysteresis <= 0) suhuMinHysteresis = 0.3;
  if (isnan(suhuMaxHysteresis) || suhuMaxHysteresis <= 0) suhuMaxHysteresis = 0.3;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 5000) {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.begin();
    if (timeClient.update()) {
      setTime(timeClient.getEpochTime());
      rtc.adjust(DateTime(year(), month(), day(), hour(), minute(), second()));
      ntpTimeSet = true;
    }
  }

  if (!ntpTimeSet) {
    Serial.println("Gagal mendapatkan waktu dari NTP. Menggunakan RTC.");
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

void kontrolSuhu(float suhu) {
  bool aktifHeater = suhu < (suhuMin - suhuMinHysteresis);
  bool aktifCooler = suhu > (suhuMax + suhuMaxHysteresis);

  waterHeaterOn = aktifHeater;
  waterCoolerOn = aktifCooler;

  digitalWrite(RELAY_WATER_HEATER, waterHeaterOn ? HIGH : LOW);
  digitalWrite(RELAY_WATER_COOLER, waterCoolerOn ? HIGH : LOW);
}

void PerintahSuhuSerial() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.startsWith("SETMIN")) {
      int spaceIndex = input.indexOf(' ');
      if (spaceIndex != -1) {
        float nilai = input.substring(spaceIndex + 1).toFloat();
        if (nilai > 0 && nilai < suhuMax) {
          suhuMin = nilai;
          EEPROM.put(0, suhuMin);
          EEPROM.commit();
        }
      }
    } else if (input.startsWith("SETMAX")) {
      int spaceIndex = input.indexOf(' ');
      if (spaceIndex != -1) {
        float nilai = input.substring(spaceIndex + 1).toFloat();
        if (nilai > suhuMin && nilai < 100) {
          suhuMax = nilai;
          EEPROM.put(10, suhuMax);
          EEPROM.commit();
        }
      }
    } else if (input.startsWith("SETHYMIN")) {
      int spaceIndex = input.indexOf(' ');
      if (spaceIndex != -1) {
        float nilai = input.substring(spaceIndex + 1).toFloat();
        if (nilai > 0 && nilai <= 5) {
          suhuMinHysteresis = nilai;
          EEPROM.put(20, suhuMinHysteresis);
          EEPROM.commit();
        }
      }
    } else if (input.startsWith("SETHYMAX")) {
      int spaceIndex = input.indexOf(' ');
      if (spaceIndex != -1) {
        float nilai = input.substring(spaceIndex + 1).toFloat();
        if (nilai > 0 && nilai <= 5) {
          suhuMaxHysteresis = nilai;
          EEPROM.put(30, suhuMaxHysteresis);
          EEPROM.commit();
        }
      }
    }
  }
}

void loop() {
  PerintahSuhuSerial();

  sensorSuhu.requestTemperatures();
  float suhu = sensorSuhu.getTempCByIndex(0);

  if (!isnan(suhu) && suhu != -127.0) {
    kontrolSuhu(suhu);
    DateTime now = rtc.now();
    char waktu[25];
    sprintf(waktu, "%d/%d/%d %02d:%02d:%02d",
            now.day(), now.month(), now.year(),
            now.hour(), now.minute(), now.second());

    kirimPesan(MSG_A, suhu, waktu);
    delay(500);
    kirimPesan(MSG_B, suhu, waktu);
    delay(500);
    kirimPesan(MSG_SP, suhu, waktu);
    delay(9600);
  }
}

void kirimPesan(int tipe, float suhu, const char* waktu) {
  const int farmID = 66;
  int valve = 1;
  int upLevel = 1;
  int feederActive = 0;

  float airTemp = 29.12;
  float airHum = 76.33;
  float PHValue = 6.78;
  float TDSValue = 1010.56;
  float O2Value = 7.89;

  int thPHUp = 8, thPHDown = 6;
  int thTDSUp = 1200, thTDSDown = 800;
  int thHumUp = 85, thHumDown = 60;

  if (tipe == MSG_A) {
    Serial.print("A;");
    Serial.print(farmID); Serial.print(";");
    Serial.print(waktu); Serial.print(";");
    Serial.print(suhu, 2); Serial.print(";");
    Serial.print(airTemp); Serial.print(";");
    Serial.print(airHum); Serial.print(";");
    Serial.print(PHValue); Serial.print(";");
    Serial.print(TDSValue); Serial.print(";");
    Serial.print(O2Value); Serial.print(";");
    Serial.println(upLevel);

    kirimPesanESPNow(MSG_A, farmID, waktu, suhu, 0, 0, 0, false, false);
  }

  if (tipe == MSG_B) {
    Serial.print("B;");
    Serial.print(farmID); Serial.print(";");
    Serial.print(waktu); Serial.print(";");
    Serial.print(valve); Serial.print(";");
    Serial.print(upLevel); Serial.print(";");
    Serial.print(feederActive); Serial.print(";");
    Serial.print(waterCoolerOn ? "1" : "0"); Serial.print(";");
    Serial.println(waterHeaterOn ? "1" : "0");

    kirimPesanESPNow(MSG_B, farmID, waktu, suhu, valve, upLevel, feederActive, waterCoolerOn, waterHeaterOn);
  }

  if (tipe == MSG_SP) {
    Serial.print("SP;");
    Serial.print(farmID); Serial.print(";");
    Serial.print(waktu); Serial.print(";");
    Serial.print(thPHUp); Serial.print(";");
    Serial.print(thPHDown); Serial.print(";");
    Serial.print(thTDSUp); Serial.print(";");
    Serial.print(thTDSDown); Serial.print(";");
    Serial.print(suhuMax); Serial.print(";");
    Serial.print(suhuMin); Serial.print(";");
    Serial.print(thHumUp); Serial.print(";");
    Serial.println(thHumDown);

    kirimPesanESPNow(MSG_SP, farmID, waktu, suhu, 0, 0, 0, false, false);
  }
}

void kirimPesanESPNow(int tipe, int farmID, const char* waktu, float suhu, int valve, int upLevel, int feeder, bool cooler, bool heater) {
  char payload[150];
  if (tipe == MSG_A) {
    sprintf(payload, "%d;A;%s;%.2f;29.12;76.33;6.78;1010.56;7.89;%d",
            farmID, waktu, suhu, upLevel);
  } else if (tipe == MSG_B) {
    sprintf(payload, "%d;B;%s;%d;%d;%d;%d;%d",
            farmID, waktu, valve, upLevel, feeder,
            cooler ? 1 : 0,
            heater ? 1 : 0);
  } else if (tipe == MSG_SP) {
    int thPHUp = 8, thPHDown = 6;
    int thTDSUp = 1200, thTDSDown = 800;
    int thHumUp = 85, thHumDown = 60;
    sprintf(payload, "%d;SP;%s;%d;%d;%d;%d;%.2f;%.2f;%d;%d",
            farmID, waktu, thPHUp, thPHDown, thTDSUp, thTDSDown, suhuMax, suhuMin, thHumUp, thHumDown);
  }
  esp_now_send(peerAddress, (uint8_t *)payload, strlen(payload));
}
