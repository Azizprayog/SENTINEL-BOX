#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Fingerprint.h>
#include <SPI.h>
#include <MFRC522.h>
#include <HTTPClient.h>

// ========== WIFI & MQTT ==========
const char* ssid        = "Danyep";
const char* password    = "HiSayang";
const char* mqtt_server = "294542ce25054f91be349c2b99c45ebc.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "sentinel";
const char* mqtt_pass   = "Tes12345";

// ========== IP ESP32-CAM ==========
const char* camIP   = "172.26.245.51"; // <-- ganti IP ESP32-CAM kamu
const int   camPort = 80;

// ========== LCD I2C ==========
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ========== FINGERPRINT (UART2: RX=16, TX=17) ==========
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger(&mySerial);

// ========== RFID RC522 (SPI) ==========
#define RST_PIN  4
#define SS_PIN   5
MFRC522 mfrc522(SS_PIN, RST_PIN);

// ========== RELAY ==========
#define RELAY_PIN 13

// ========== WEBSERVER ==========
WebServer serverHTTP(80);

// ========== MQTT CLIENT ==========
WiFiClientSecure espClient;
PubSubClient client(espClient);

// ========== VARIABEL AUTH ==========
bool brankasTerbuka = false;
bool wajahOK        = false;
bool sidikOK        = false;
bool rfidOK         = false;

int  gagalCount     = 0;
const int MAX_GAGAL = 5;

unsigned long authStartTime;
const unsigned long AUTH_TIMEOUT = 30000;

// ========== MODE ENROLL ==========
bool enrollMode = false;
int  enrollID   = -1;

// ========== DATA TERDAFTAR ==========
const uint32_t authorizedRFID = 0x5909D006;

// ========== DEKLARASI FUNGSI ==========
void updateLCD();
void publishStatus(const char* status);
void connectMQTT();
void checkAllAuth();
void resetAuth();
void checkFingerprint();
void checkRFID();
void tampilLockout();
void doEnroll(int id);
void triggerCamScan();

// ===================================================
// LCD
// ===================================================
void updateLCD() {
  lcd.clear();
  if (enrollMode) {
    lcd.setCursor(0, 0);
    lcd.print("Mode Enroll ID:");
    lcd.setCursor(0, 1);
    lcd.print(enrollID);
    return;
  }
  if (brankasTerbuka) {
    lcd.setCursor(0, 0);
    lcd.print("Berangkas Terbuka");
    lcd.setCursor(0, 1);
    lcd.print("Selamat Datang! ");
  } else {
    lcd.setCursor(0, 0);
    if (!rfidOK) {
      lcd.print("Tempelkan Kartu ");
      lcd.setCursor(0, 1);
      lcd.print("                ");
    } else if (!sidikOK) {
      lcd.print("Tempel Sidik");
      lcd.setCursor(0, 1);
      lcd.print("Jari Anda       ");
    } else if (!wajahOK) {
      lcd.print("Scan Wajah");
      lcd.setCursor(0, 1);
      lcd.print("Lihat ke Kamera ");
    }
  }
}

// ===================================================
// LOCKOUT
// ===================================================
void tampilLockout() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AKSES DIKUNCI!");
  lcd.setCursor(0, 1);
  lcd.print("Hubungi Admin   ");
  publishStatus("lockout");
  Serial.println("[AUTH] LOCKOUT! 5x gagal");
  delay(30000);
  gagalCount = 0;
  resetAuth();
  updateLCD();
}

// ===================================================
// TRIGGER ESP32-CAM
// ===================================================
void triggerCamScan() {
  Serial.println("[CAM] Trigger scan wajah...");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Menghubungi");
  lcd.setCursor(0, 1);
  lcd.print("Kamera...       ");

  HTTPClient http;
  String url = "http://" + String(camIP) + ":" + String(camPort) + "/trigger_scan";
  http.begin(url);
  http.setTimeout(5000);

  int code = http.GET();
  http.end();

  if (code == 200) {
    Serial.println("[CAM] Trigger OK, tunggu hasil...");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Scan Wajah");
    lcd.setCursor(0, 1);
    lcd.print("Lihat ke Kamera ");
  } else {
    Serial.println("[CAM] Gagal! Code: " + String(code));
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Kamera Error!");
    lcd.setCursor(0, 1);
    lcd.print("Cek koneksi     ");
    delay(2000);
    gagalCount++;
    if (gagalCount >= MAX_GAGAL) tampilLockout();
    else updateLCD();
  }
}

// ===================================================
// HANDLER HTTP DARI ESP32-CAM
// ===================================================
void handleOpen() {
  String name = serverHTTP.arg("name");
  Serial.println("[HTTP] /open diterima, nama: " + name);

  if (!rfidOK || !sidikOK) {
    Serial.println("[HTTP] /open ditolak - auth belum lengkap");
    serverHTTP.send(403, "text/plain", "AUTH_INCOMPLETE");
    return;
  }

  wajahOK = true;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Halo,");
  lcd.setCursor(0, 1);
  lcd.print(name.substring(0, 10) + " \x7E");
  delay(2000);

  serverHTTP.send(200, "text/plain", "OK");
  checkAllAuth();
}

void handleDeny() {
  Serial.println("[HTTP] /deny diterima");

  gagalCount++;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Wajah Ditolak!");
  lcd.setCursor(0, 1);
  lcd.print("Gagal: " + String(gagalCount) + "/" + String(MAX_GAGAL));
  delay(2000);

  serverHTTP.send(200, "text/plain", "DENIED");

  if (gagalCount >= MAX_GAGAL) tampilLockout();
  else updateLCD();
}

// ===================================================
// MQTT
// ===================================================
void publishStatus(const char* status) {
  client.publish("brankas/status", status);
  Serial.print("[MQTT] Status: ");
  Serial.println(status);
}

void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("[MQTT] [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  if (String(topic) == "brankas/kunci") {
    if (message == "UNLOCK") {
      brankasTerbuka = true;
      digitalWrite(RELAY_PIN, HIGH);
      updateLCD();
      publishStatus("terbuka");
    } else if (message == "LOCK") {
      brankasTerbuka = false;
      digitalWrite(RELAY_PIN, LOW);
      gagalCount = 0;
      resetAuth();
      updateLCD();
      publishStatus("terkunci");
    } else {
      publishStatus("error");
    }
  }

  if (String(topic) == "brankas/sidik/enroll") {
    int id = message.toInt();
    if (id >= 1 && id <= 127) {
      enrollMode = true;
      enrollID   = id;
      Serial.print("[ENROLL] Mulai enroll ID: ");
      Serial.println(id);
      updateLCD();
      doEnroll(id);
    } else {
      client.publish("brankas/sidik/result", "ERROR:ID_INVALID");
    }
  }

  if (String(topic) == "brankas/sidik/hapus") {
    int id = message.toInt();
    if (id >= 1 && id <= 127) {
      uint8_t p = finger.deleteModel(id);
      if (p == FINGERPRINT_OK)
        client.publish("brankas/sidik/result", ("DELETED:" + String(id)).c_str());
      else
        client.publish("brankas/sidik/result", ("DELETE_FAIL:" + String(id)).c_str());
    }
  }
}

void connectMQTT() {
  while (!client.connected()) {
    Serial.print("[MQTT] Connecting...");
    String clientId = "ESP32-" + WiFi.macAddress();
    clientId.replace(":", "");
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("Connected!");
      client.subscribe("brankas/kunci");
      client.subscribe("brankas/sidik/enroll");
      client.subscribe("brankas/sidik/hapus");
      publishStatus("online");
    } else {
      Serial.print("Gagal rc=");
      Serial.print(client.state());
      Serial.println(" | Retry 5s...");
      delay(5000);
    }
  }
}

// ===================================================
// ENROLL
// ===================================================
void doEnroll(int id) {
  int p = -1;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Tempel Jari #1");
  lcd.setCursor(0, 1);
  lcd.print("ID: " + String(id));
  client.publish("brankas/sidik/result", ("ENROLL_STEP1:" + String(id)).c_str());

  unsigned long t = millis();
  while (p != FINGERPRINT_OK) {
    if (millis() - t > 15000) {
      client.publish("brankas/sidik/result", "ERROR:TIMEOUT");
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Enroll Timeout!");
      delay(2000); enrollMode = false; updateLCD(); return;
    }
    p = finger.getImage();
    client.loop();
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    client.publish("brankas/sidik/result", "ERROR:IMAGE1_FAIL");
    enrollMode = false; updateLCD(); return;
  }

  lcd.clear(); lcd.setCursor(0,0); lcd.print("Angkat Jari...");
  delay(2000);
  while (finger.getImage() != FINGERPRINT_NOFINGER) { client.loop(); }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Tempel Lagi");
  lcd.setCursor(0, 1); lcd.print("Jari yang Sama");
  client.publish("brankas/sidik/result", ("ENROLL_STEP2:" + String(id)).c_str());

  p = -1; t = millis();
  while (p != FINGERPRINT_OK) {
    if (millis() - t > 15000) {
      client.publish("brankas/sidik/result", "ERROR:TIMEOUT");
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Enroll Timeout!");
      delay(2000); enrollMode = false; updateLCD(); return;
    }
    p = finger.getImage();
    client.loop();
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    client.publish("brankas/sidik/result", "ERROR:IMAGE2_FAIL");
    enrollMode = false; updateLCD(); return;
  }

  p = finger.createModel();
  if (p == FINGERPRINT_ENROLLMISMATCH) {
    lcd.clear(); lcd.setCursor(0,0); lcd.print("Jari Tidak");
    lcd.setCursor(0,1); lcd.print("Cocok! Ulangi");
    client.publish("brankas/sidik/result", "ERROR:MISMATCH");
    delay(2000); enrollMode = false; updateLCD(); return;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Enroll Berhasil!");
    lcd.setCursor(0,1); lcd.print("ID: " + String(id) + " \x7E");
    client.publish("brankas/sidik/result", ("SUCCESS:" + String(id)).c_str());
    delay(3000);
  } else {
    client.publish("brankas/sidik/result", "ERROR:STORE_FAIL");
  }

  enrollMode = false;
  updateLCD();
}

// ===================================================
// AUTH
// ===================================================
void resetAuth() {
  wajahOK = sidikOK = rfidOK = false;
  authStartTime = millis();
}

void checkAllAuth() {
  if (rfidOK && sidikOK && wajahOK) {
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("R:\x7E S:\x7E W:\x7E");
    lcd.setCursor(0,1); lcd.print("Verifikasi OK!");
    delay(3000);

    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Akses Diterima!");
    lcd.setCursor(0,1); lcd.print("Membuka...      ");
    delay(1000);

    brankasTerbuka = true;
    digitalWrite(RELAY_PIN, HIGH);
    publishStatus("terbuka");

    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Pintu Terbuka");
    lcd.setCursor(0,1); lcd.print("Tunggu 5 detik..");
    delay(5000);

    digitalWrite(RELAY_PIN, LOW);
    brankasTerbuka = false;
    gagalCount = 0;
    publishStatus("terkunci");
    resetAuth();
    updateLCD();
  } else {
    updateLCD();
  }
}

// ===================================================
// RFID
// ===================================================
void checkRFID() {
  if (rfidOK) return;
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  uint32_t uid = 0;
  for (byte i = 0; i < mfrc522.uid.size; i++)
    uid = (uid << 8) | mfrc522.uid.uidByte[i];

  Serial.print("[RFID] UID: 0x");
  Serial.println(uid, HEX);

  if (uid == authorizedRFID) {
    rfidOK = true;
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Kartu RFID");
    lcd.setCursor(0,1); lcd.print("Berhasil \x7E");
    delay(3000);
    checkAllAuth();
  } else {
    gagalCount++;
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Kartu Ditolak!");
    lcd.setCursor(0,1); lcd.print("Gagal: " + String(gagalCount) + "/" + String(MAX_GAGAL));
    delay(2000);
    if (gagalCount >= MAX_GAGAL) tampilLockout();
    else updateLCD();
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// ===================================================
// FINGERPRINT
// ===================================================
void checkFingerprint() {
  if (sidikOK || !rfidOK) return;

  int result = finger.getImage();
  if (result != FINGERPRINT_OK) return;

  result = finger.image2Tz();
  if (result != FINGERPRINT_OK) return;

  result = finger.fingerFastSearch();
  if (result != FINGERPRINT_OK) {
    gagalCount++;
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Sidik Ditolak!");
    lcd.setCursor(0,1); lcd.print("Gagal: " + String(gagalCount) + "/" + String(MAX_GAGAL));
    delay(2000);
    if (gagalCount >= MAX_GAGAL) tampilLockout();
    else updateLCD();
    return;
  }

  sidikOK = true;
  Serial.println("[AUTH] Sidik jari OK, ID: " + String(finger.fingerID));
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Sidik Jari");
  lcd.setCursor(0,1); lcd.print("Berhasil \x7E");
  delay(2000);

  // Otomatis trigger scan wajah ke ESP32-CAM
  triggerCamScan();
}

// ===================================================
// SETUP
// ===================================================
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0); lcd.print("Sentinel Box");
  lcd.setCursor(0,1); lcd.print("Booting...");
  delay(1000);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  WiFi.begin(ssid, password);
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    lcd.setCursor(0,1); lcd.print("Please wait...  ");
  }
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("WiFi Connected!");
  lcd.setCursor(0,1); lcd.print(WiFi.localIP().toString());
  Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
  delay(1500);

  espClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  mySerial.begin(57600, SERIAL_8N1, 16, 17);
  finger.begin(57600);
  lcd.clear();
  if (finger.verifyPassword()) {
    lcd.setCursor(0,0); lcd.print("Finger Sensor");
    lcd.setCursor(0,1); lcd.print("OK!");
  } else {
    lcd.setCursor(0,0); lcd.print("Finger ERROR!");
    lcd.setCursor(0,1); lcd.print("Cek kabel");
  }
  delay(1500);

  SPI.begin();
  mfrc522.PCD_Init();
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("RFID Siap");
  delay(1000);

  // HTTP server untuk terima hasil dari ESP32-CAM
  serverHTTP.on("/open", handleOpen);
  serverHTTP.on("/deny", handleDeny);
  serverHTTP.begin();
  Serial.println("[HTTP] Server jalan di port 80");

  gagalCount = 0;
  resetAuth();
  updateLCD();
  Serial.println("[SYSTEM] Sistem siap!");
}

// ===================================================
// LOOP
// ===================================================
void loop() {
  if (!client.connected()) connectMQTT();
  client.loop();

  serverHTTP.handleClient(); // terima HTTP dari ESP32-CAM

  if (enrollMode) return;

  if (!rfidOK)             checkRFID();
  if (rfidOK && !sidikOK) checkFingerprint();

  // Timeout auth
  if (millis() - authStartTime > AUTH_TIMEOUT && (rfidOK || sidikOK)) {
    Serial.println("[AUTH] Timeout! Reset.");
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Timeout!");
    lcd.setCursor(0,1); lcd.print("Ulangi proses   ");
    delay(1500);
    resetAuth();
    updateLCD();
  }
}