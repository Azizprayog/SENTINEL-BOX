// ============================================================
//   SENTINEL BOX - ESP32 UTAMA (FLOW BARU)
// ============================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>

#define USE_UART_CAM 0

const char* ssid        = "R-403";
const char* password    = "*ruang403";
const char* mqtt_server = "10.4.3.101";
const int   mqtt_port   = 1883;
const char* mqtt_user   = "sentinel";
const char* mqtt_pass   = "Tes12345";

#define RST_PIN    4
#define SS_PIN     5
#define RELAY_PIN  13
#define CAM_RX_PIN 14
#define CAM_TX_PIN 12

#define TOPIC_STATUS     "brankas/status"
#define TOPIC_KUNCI      "brankas/kunci"
#define TOPIC_RFID       "brankas/rfid"
#define TOPIC_FINGER     "brankas/sidikjari"
#define TOPIC_ENROLL     "brankas/sidik/enroll"
#define TOPIC_DELETE     "brankas/sidik/hapus"
#define TOPIC_RELAY      "brankas/relay"

const uint32_t AUTHORIZED_RFID_UID = 0x5909D006;

LiquidCrystal_I2C lcd(0x27, 16, 2);
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger(&fingerSerial);
MFRC522 mfrc522(SS_PIN, RST_PIN);

WiFiClient espClient;
PubSubClient client(espClient);

// ================= FLOW STATE =================
enum StepAuth {
  WAIT_RFID,
  WAIT_FINGER,
};

StepAuth currentStep = WAIT_RFID;
bool isEnrolling = false; 
int enrollStep = 0;
int enrollID = 1;

int failRFID = 0;
int failFinger = 0;

const int MAX_FAIL = 5;
bool sentProcess = false;

// ================= LCD =================
void lcdPrint(const char* a, const char* b="") {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(a);
  lcd.setCursor(0,1); lcd.print(b);
}

// ================= RELAY =================
void relayBuka() {
  digitalWrite(RELAY_PIN, HIGH);
  client.publish(TOPIC_RELAY, "OPEN");
}

void relayKunci() {
  digitalWrite(RELAY_PIN, LOW);
  client.publish(TOPIC_RELAY, "CLOSED");
}

// ================= RESET =================
void resetSystem() {
  failRFID = 0;
  failFinger = 0;
  sentProcess = false;
  currentStep = WAIT_RFID;
  lcdPrint("Tempel Kartu","");
}

// ================= RFID =================
void checkRFID() {
  if (isEnrolling) return;
  if (currentStep != WAIT_RFID) return;

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  uint32_t uid = 0;
  for (byte i = 0; i < mfrc522.uid.size; i++)
    uid = (uid << 8) | mfrc522.uid.uidByte[i];

  // ================= VALIDASI =================
  if (uid == AUTHORIZED_RFID_UID) {

    // 🔥 RESET FAIL RFID kalau berhasil
    failRFID = 0;

    client.publish(TOPIC_RFID, String(uid).c_str());
    lcdPrint("Kartu Berhasil", "");
    delay(2000);

    currentStep = WAIT_FINGER;
    failFinger = 0;
    lcdPrint("Verifikasi", "Sidik Jari");
  } else {
    client.publish(TOPIC_RFID, "UNKNOWN");

    failRFID++;

    lcdPrint("Kartu Salah", ("Try: " + String(failRFID)).c_str());
    delay(2000);

    if (failRFID >= MAX_FAIL) {
      lcdPrint("Akses Ditolak","");
      delay(3000);
      resetSystem();   // balik ke awal
    } else {
      lcdPrint("Coba Lagi","");
      // tetap di WAIT_RFID (ga perlu set ulang)
    }
  }

  mfrc522.PICC_HaltA();
}

// ================= FINGER =================
void checkFingerprint() {
  if (isEnrolling) {

    // STEP 1
    if (enrollStep == 0) {
        lcdPrint("Tempel Jari", "Langkah 1");
      if (finger.getImage()!=FINGERPRINT_OK) return;
      if (finger.image2Tz(1)!=FINGERPRINT_OK) return;
        client.publish(TOPIC_FINGER, "ENROLL_STEP1");
        lcdPrint("Angkat Jari", "");
      delay(2000);
      while (finger.getImage() != FINGERPRINT_NOFINGER) {
        delay(10);
      }
      enrollStep = 1;
      return;
    }

    // STEP 2
    if (enrollStep == 1) {
      lcdPrint("Tempel Lagi", "Langkah 2");

      // tunggu sampai jari ditempel lagi
      while (finger.getImage() != FINGERPRINT_OK) {
        delay(10);
      }

      // convert image ke buffer 2
      if (finger.image2Tz(2) != FINGERPRINT_OK) {
        client.publish(TOPIC_FINGER, "ERROR");

        isEnrolling = false;
        enrollStep = 0;

        lcdPrint("Gagal Convert", "");
        delay(2000);

        resetSystem();
        return;
      }

      // cek apakah ID sudah ada
      uint8_t check = finger.loadModel(enrollID);
      if (check == FINGERPRINT_OK) {
          lcdPrint("ID Sudah Ada", "");
          client.publish(TOPIC_FINGER, "ID_EXISTS");

          isEnrolling = false;
          enrollStep = 0;
          delay(2000);
          resetSystem();
          return;
      }

      // gabungkan buffer 1 & 2 jadi model fingerprint
      if (finger.createModel() != FINGERPRINT_OK) {
          client.publish(TOPIC_FINGER, "ERROR");

          isEnrolling = false;
          enrollStep = 0;

          lcdPrint("Model Gagal", "");
          delay(2000);

          resetSystem();
          return;
      }

      // simpan ke slot
      if (finger.storeModel(enrollID)!=FINGERPRINT_OK) {
        client.publish(TOPIC_FINGER, "ERROR");
        isEnrolling = false;
        enrollStep = 0;
        return;
      }

      client.publish(TOPIC_FINGER, "SUCCESS");

      lcdPrint("Berhasil", ("ID: " + String(enrollID)).c_str());
      delay(3000);

      isEnrolling = false;
      enrollStep = 0;
      resetSystem();
      return;
    }
  }

  // ================= NORMAL MODE =================
    if (currentStep != WAIT_FINGER) return;
      
    if (!sentProcess) {
      client.publish(TOPIC_FINGER, "PROCESS");
      sentProcess = true;
    }

    if (finger.getImage()!=FINGERPRINT_OK) return;

      lcdPrint("Scan","Sidik jari");

    if (finger.image2Tz()!=FINGERPRINT_OK) return;
    
    if (finger.fingerFastSearch()!=FINGERPRINT_OK) {
      client.publish(TOPIC_FINGER, "FAIL");
      failFinger++;

      // tampilkan jumlah percobaan
      lcdPrint("Sidik Salah", ("Try: " + String(failFinger)).c_str());
      delay(2000);

      // kalau belum 5x → tetap di fingerprint
      if (failFinger < MAX_FAIL) {
        lcdPrint("Coba Lagi Jari","");
        delay(1000);
        return;
      }

      // kalau sudah 5x → reset ke RFID
      lcdPrint("Akses Ditolak","");
      delay(3000);

      resetSystem();
      return;
    }

    // ================= SIDIK VALID =================
    if (finger.fingerID >= 1 && finger.confidence > 50) {

      Serial.print("Finger ID: ");
      Serial.println(finger.fingerID);

      Serial.print("Confidence: ");
      Serial.println(finger.confidence);

      String payload = "MATCH:" + String(finger.fingerID);
      client.publish(TOPIC_FINGER, payload.c_str());
      sentProcess = false;

      // reset counter
      failFinger = 0;
      failRFID = 0;

      lcdPrint("Sidik OK", ("ID: " + String(finger.fingerID)).c_str());
      delay(2000);

      lcdPrint("AKSES DITERIMA", "");
      relayBuka();
      delay(2000);
      relayKunci();

      resetSystem();
    } else {
      client.publish(TOPIC_FINGER, "UNKNOWN");
      sentProcess = false;

      failFinger++;

      lcdPrint("Sidik Tidak Cocok", ("Try: " + String(failFinger)).c_str());
      delay(2000);

      if (failFinger < MAX_FAIL) {
        lcdPrint("Coba Lagi","");
        return;
      }

      lcdPrint("Akses Ditolak","");
      delay(3000);
      resetSystem();
    }
}

// ================= MQTT =================
void callback(char* topic, byte* payload, unsigned int length) {
  String msg="";
  for(int i=0;i<length;i++) msg+=(char)payload[i];

  Serial.print("[MQTT] ");
  Serial.print(topic);
  Serial.print(" => ");
  Serial.println(msg);

  // ================= KUNCI =================
  if(String(topic)==TOPIC_KUNCI){
    if(msg=="UNLOCK"){
      relayBuka();
      delay(2000);  
      lcdPrint("MODE MANUAL","");
    }
    else if(msg=="LOCK"){
      relayKunci();
      resetSystem();
    }
  }

  // ================= ENROLL =================
  if(String(topic)==TOPIC_ENROLL){
    isEnrolling = true;
    enrollStep = 0;
    enrollID = msg.toInt();

    lcdPrint("Pendaftaran jari", ("ID: " + String(enrollID)).c_str());
    delay(1500);
    client.publish(TOPIC_FINGER, "PROCESS");
  }

  // ================= DELETE SIDIK JARI =================
  if(String(topic) == TOPIC_DELETE){
      int id = msg.toInt();
      if (finger.deleteModel(id) == FINGERPRINT_OK) {
        Serial.println("Sidik jari dihapus");
      } else {
        Serial.println("Gagal hapus sidik jari");
      }
    }
  }


void reconnect(){
  while(!client.connected()){
    Serial.println("[MQTT] Connecting...");
    
    if(client.connect("SENTINEL",mqtt_user,mqtt_pass)){
      Serial.println("[MQTT] Connected");

      // 🔥 SUBSCRIBE SEMUA
      client.subscribe(TOPIC_KUNCI);
      client.subscribe(TOPIC_ENROLL); 
      client.subscribe(TOPIC_DELETE);

      // 🔥 STATUS ONLINE
      client.publish(TOPIC_STATUS, "online");

    } else {
      Serial.print("Failed rc=");
      Serial.println(client.state());
      delay(2000);
    }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN,OUTPUT);
  relayKunci();

  lcd.init(); lcd.backlight();

  lcdPrint("Connecting WiFi","");
  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
  }

  lcdPrint("WiFi OK","");
  delay(1000);

  lcdPrint("WiFi Terhubung", WiFi.localIP().toString().c_str());
  delay(2000);

  client.setServer(mqtt_server,mqtt_port);
  client.setCallback(callback);

  fingerSerial.begin(57600,SERIAL_8N1,16,17);
  finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor OK");
  } else {
    Serial.println("Fingerprint sensor ERROR");

    lcdPrint("Sidik Jari", "Bermasalah");
    
    while (1) {
      delay(1);
    }
  }

  SPI.begin();
  mfrc522.PCD_Init();


  resetSystem();

  // 🔥 STATUS ONLINE
  client.publish(TOPIC_STATUS, "booting");
}

// ================= LOOP =================
void loop() {
  if(!client.connected()) reconnect();
  client.loop();

  checkRFID();
  checkFingerprint();
}