// ============================================================
//   SENTINEL BOX - ESP32 UTAMA (Otak)
//   Sensor  : RFID RC522 + AS608 Fingerprint + Relay
//   Network : WiFi + MQTT (PubSubClient)
//   Comms   : UART ke ESP32-CAM (GPIO14 RX / GPIO12 TX)
//   LCD     : I2C 16x2 (0x27)
// ============================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>

// ============================================================
// KONFIGURASI WiFi & MQTT
// ============================================================
const char* ssid        = "SIJABAIK";          // ganti sesuai WiFi kamu
const char* password    = "SENTINEL";           // ganti sesuai WiFi kamu
const char* mqtt_server = "10.42.0.32";         // IP broker MQTT
const int   mqtt_port   = 1883;
const char* mqtt_user   = "sentinel";
const char* mqtt_pass   = "Tes12345";

// ============================================================
// PIN MAPPING
// ============================================================
// RFID RC522 (SPI)
#define RST_PIN    4
#define SS_PIN     5
// SCK=18, MOSI=23, MISO=19 (otomatis SPI default ESP32)

// Fingerprint AS608 (UART2)
// RX2=16, TX2=17

// Relay Solenoid
#define RELAY_PIN  13

// Komunikasi ke ESP32-CAM (UART1)
#define CAM_RX_PIN 14   // terima dari TX ESP32-CAM
#define CAM_TX_PIN 12   // kirim ke RX ESP32-CAM

// ============================================================
// MQTT TOPICS
// ============================================================
// SUBSCRIBE (terima perintah dari server/python)
#define TOPIC_KUNCI      "brankas/kunci"       // UNLOCK / LOCK / SCAN
#define TOPIC_CAM_RESULT "brankas/wajah/result"// WAJAH_OK / WAJAH_FAIL (diteruskan dari python)

// PUBLISH (kirim status ke server/python)
#define TOPIC_RFID       "brankas/rfid"        // UID kartu yang di-tap
#define TOPIC_FINGER     "brankas/sidikjari"   // MATCH / FAIL / ID:<n>
#define TOPIC_RELAY      "brankas/relay"       // OPEN / CLOSED
#define TOPIC_STATUS     "brankas/status"      // status autentikasi keseluruhan
#define TOPIC_LOG        "brankas/log"         // log teks untuk debugging

// ============================================================
// DATA AUTENTIKASI (GANTI DENGAN DATA ASLI)
// ============================================================
const uint32_t AUTHORIZED_RFID_UID = 0x5909D006; // baca UID dulu via serial monitor
const int      AUTHORIZED_FINGER_ID = 1;          // ID sidik jari yang di-enrolwwl

// ============================================================
// OBJEK
// ============================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);

HardwareSerial fingerSerial(2);   // UART2: RX=16, TX=17
Adafruit_Fingerprint finger(&fingerSerial);

MFRC522 mfrc522(SS_PIN, RST_PIN);

HardwareSerial camSerial(1);      // UART1: RX=14, TX=12

WiFiClient   espClient;
PubSubClient client(espClient);

// ============================================================
// VARIABEL STATE AUTENTIKASI
// ============================================================
bool authWajah  = false;
bool authSidik  = false;
bool authRFID   = false;

unsigned long authStartTime = 0;
const unsigned long AUTH_TIMEOUT_MS = 60000; // 60 detik untuk selesaikan 3 auth

// ============================================================
// HELPER: LCD + LOG
// ============================================================
void lcdPrint(const char* baris1, const char* baris2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(baris1);
  lcd.setCursor(0, 1);
  lcd.print(baris2);
}

void mqttLog(const char* msg) {
  Serial.println(msg);
  if (client.connected()) {
    client.publish(TOPIC_LOG, msg);
  }
}

void standbyLCD() {
  char baris2[17] = "";
  snprintf(baris2, sizeof(baris2), "W:%d F:%d R:%d",
           authWajah ? 1 : 0,
           authSidik ? 1 : 0,
           authRFID  ? 1 : 0);
  lcdPrint("== SENTINEL ==", baris2);
}

// ============================================================
// RELAY CONTROL
// ============================================================
void relayBuka() {
  digitalWrite(RELAY_PIN, HIGH);
  client.publish(TOPIC_RELAY, "OPEN");
  mqttLog("[RELAY] BUKA - solenoid aktif");
}

void relayKunci() {
  digitalWrite(RELAY_PIN, LOW);
  client.publish(TOPIC_RELAY, "CLOSED");
  mqttLog("[RELAY] KUNCI - solenoid nonaktif");
}

// ============================================================
// CEK SEMUA AUTENTIKASI
// ============================================================
void resetAuth() {
  authWajah  = false;
  authSidik  = false;
  authRFID   = false;
  authStartTime = millis();
  client.publish(TOPIC_STATUS, "RESET");
  standbyLCD();
}

void cekSemuaAuth() {
  // Publish status progress
  char statusMsg[64];
  snprintf(statusMsg, sizeof(statusMsg),
           "{\"wajah\":%d,\"sidik\":%d,\"rfid\":%d}",
           authWajah ? 1 : 0,
           authSidik ? 1 : 0,
           authRFID  ? 1 : 0);
  client.publish(TOPIC_STATUS, statusMsg);

  if (authWajah && authSidik && authRFID) {
    // ====== SEMUA AUTH LULUS ======
    lcdPrint("AKSES DITERIMA", "Membuka...");
    client.publish(TOPIC_STATUS, "GRANTED");
    mqttLog("[AUTH] SEMUA LULUS - membuka brankas");

    relayBuka();
    delay(5000);  // solenoid aktif 5 detik
    relayKunci();

    lcdPrint("Brankas ditutup", "Auth direset");
    delay(2000);
    resetAuth();

  } else {
    // Tampilkan progress
    standbyLCD();
  }
}

// ============================================================
// FINGERPRINT
// ============================================================
void checkFingerprint() {
  if (authSidik) return;  // sudah OK, skip

  int result = finger.getImage();
  if (result == FINGERPRINT_NOFINGER) return;
  if (result != FINGERPRINT_OK) return;

  result = finger.image2Tz();
  if (result != FINGERPRINT_OK) return;

  result = finger.fingerFastSearch();
  if (result != FINGERPRINT_OK) {
    // Tidak dikenali
    lcdPrint("Sidik Salah!", "Coba lagi");
    client.publish(TOPIC_FINGER, "FAIL");
    mqttLog("[FINGER] Sidik jari tidak dikenali");
    delay(1500);
    standbyLCD();
    return;
  }

  // Dikenali - cek ID
  char fingerMsg[32];
  snprintf(fingerMsg, sizeof(fingerMsg), "MATCH:ID%d", finger.fingerID);
  client.publish(TOPIC_FINGER, fingerMsg);

  if (finger.fingerID == AUTHORIZED_FINGER_ID) {
    authSidik = true;
    lcdPrint("Sidik Cocok!", "");
    mqttLog("[FINGER] Sidik jari OK");
    delay(1200);
    cekSemuaAuth();
  } else {
    lcdPrint("Sidik Tdk", "Terdaftar");
    mqttLog("[FINGER] Sidik jari ID tidak diotorisasi");
    delay(1500);
    standbyLCD();
  }
}

// ============================================================
// RFID
// ============================================================
void checkRFID() {
  if (authRFID) return;  // sudah OK, skip

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial())  return;

  // Susun UID jadi uint32
  uint32_t uid = 0;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    uid = (uid << 8) | mfrc522.uid.uidByte[i];
  }

  // Publish UID ke MQTT
  char uidStr[32];
  snprintf(uidStr, sizeof(uidStr), "0x%08X", uid);
  client.publish(TOPIC_RFID, uidStr);

  Serial.print("[RFID] UID: ");
  Serial.println(uidStr);

  if (uid == AUTHORIZED_RFID_UID) {
    authRFID = true;
    lcdPrint("Kartu Cocok!", "");
    mqttLog("[RFID] Kartu RFID OK");
    delay(1200);
    cekSemuaAuth();
  } else {
    lcdPrint("Kartu Salah!", "Ditolak");
    mqttLog("[RFID] Kartu tidak diotorisasi");
    delay(1500);
    standbyLCD();
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// ============================================================
// BACA PESAN DARI ESP32-CAM (via UART)
// ============================================================
void checkCamSerial() {
  while (camSerial.available()) {
    String msg = camSerial.readStringUntil('\n');
    msg.trim();
    if (msg.length() == 0) continue;

    Serial.print("[CAM-UART] ");
    Serial.println(msg);

    if (msg == "WAJAH_OK" && !authWajah) {
      authWajah = true;
      lcdPrint("Wajah Cocok!", "");
      // Forward ke MQTT agar python/dashboard tahu
      client.publish("brankas/wajah/result", "WAJAH_OK");
      mqttLog("[CAM] Wajah terverifikasi OK");
      delay(1200);
      cekSemuaAuth();

    } else if (msg == "WAJAH_FAIL") {
      lcdPrint("Wajah Gagal!", "Coba lagi");
      client.publish("brankas/wajah/result", "WAJAH_FAIL");
      mqttLog("[CAM] Wajah gagal dikenali");
      delay(1500);
      standbyLCD();
    }
  }
}

// ============================================================
// MQTT CALLBACK (terima perintah dari Python/Dashboard)
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("[MQTT IN] ");
  Serial.print(topic);
  Serial.print(" -> ");
  Serial.println(msg);

  // ---- Topic: brankas/kunci ----
  if (String(topic) == TOPIC_KUNCI) {

    if (msg == "UNLOCK") {
      // Override manual dari dashboard
      lcdPrint("UNLOCK MANUAL", "Dari Dashboard");
      relayBuka();
      delay(5000);
      relayKunci();
      resetAuth();

    } else if (msg == "LOCK") {
      relayKunci();
      lcdPrint("TERKUNCI", "Manual LOCK");
      delay(2000);
      resetAuth();

    } else if (msg == "SCAN") {
      // Minta ESP32-CAM untuk scan
      camSerial.println("SCAN");
      lcdPrint("Scan Wajah...", "Hadap kamera");
      mqttLog("[KUNCI] Perintah SCAN dikirim ke CAM");

    } else if (msg == "RESET") {
      resetAuth();
      mqttLog("[KUNCI] Auth direset oleh server");
    }
  }

  // ---- Topic: brankas/wajah/result (dari Python face recognition) ----
  if (String(topic) == TOPIC_CAM_RESULT) {
    if (msg == "WAJAH_OK" && !authWajah) {
      authWajah = true;
      lcdPrint("Wajah OK!", "(via MQTT)");
      mqttLog("[AUTH] Wajah OK via MQTT");
      delay(1200);
      cekSemuaAuth();
    } else if (msg == "WAJAH_FAIL") {
      lcdPrint("Wajah Gagal!", "(via MQTT)");
      delay(1500);
      standbyLCD();
    }
  }
}

// ============================================================
// CONNECT MQTT
// ============================================================
void connectMQTT() {
  while (!client.connected()) {
    Serial.print("[MQTT] Connecting...");
    String clientId = "SENTINEL-" + WiFi.macAddress();
    clientId.replace(":", "");

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("CONNECTED");
      client.subscribe(TOPIC_KUNCI);
      client.subscribe(TOPIC_CAM_RESULT);
      client.publish(TOPIC_LOG, "ESP32 Utama online");
    } else {
      Serial.print("FAILED rc=");
      Serial.println(client.state());
      delay(3000);
    }
  }
}

// ============================================================
// TIMEOUT AUTH
// ============================================================
void cekTimeout() {
  if ((authWajah || authSidik || authRFID) &&
      (millis() - authStartTime > AUTH_TIMEOUT_MS)) {
    lcdPrint("TIMEOUT!", "Auth direset");
    client.publish(TOPIC_STATUS, "TIMEOUT");
    mqttLog("[AUTH] Timeout - auth direset");
    delay(2000);
    resetAuth();
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  // Relay
  pinMode(RELAY_PIN, OUTPUT);
  relayKunci();

  // LCD
  lcd.init();
  lcd.backlight();
  lcdPrint("SENTINEL BOX", "Booting...");

  // WiFi
  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
    lcdPrint("WiFi OK", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] GAGAL - lanjut tanpa WiFi");
    lcdPrint("WiFi GAGAL", "Mode Offline");
  }
  delay(1000);

  // MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  // Fingerprint AS608
  fingerSerial.begin(57600, SERIAL_8N1, 16, 17);  // RX=16, TX=17
  finger.begin(57600);
  delay(500);
  if (finger.verifyPassword()) {
    Serial.println("[FINGER] Sensor OK");
    lcdPrint("Finger OK", "");
  } else {
    Serial.println("[FINGER] Sensor ERROR!");
    lcdPrint("Finger ERROR!", "Cek kabel");
  }
  delay(1000);

  // RFID RC522
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("[RFID] RC522 siap");
  lcdPrint("RFID OK", "");
  delay(500);

  // UART ke ESP32-CAM
  camSerial.begin(115200, SERIAL_8N1, CAM_RX_PIN, CAM_TX_PIN);
  Serial.println("[CAM] UART ke ESP32-CAM siap");

  authStartTime = millis();
  standbyLCD();
  Serial.println("[SENTINEL] Sistem siap!");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  // Reconnect MQTT jika putus
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();

  // Baca pesan dari ESP32-CAM via UART
  checkCamSerial();

  // Cek sensor RFID
  checkRFID();

  // Cek sensor sidik jari
  checkFingerprint();

  // Cek timeout autentikasi
  cekTimeout();
}