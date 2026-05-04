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

const char* ssid        = "Nell";
const char* password    = "Dimasu(kin)";
const char* mqtt_server = "192.168.1.10";
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
const int AUTHORIZED_FINGER_ID = 1;

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

int failCount = 0;
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
  failCount = 0;
  sentProcess = false;
  currentStep = WAIT_RFID;
  lcdPrint("Tempel Kartu","");
}

// ================= FAIL =================
void handleFail(const char* msg){
  failCount++;

  lcdPrint(msg,"Gagal");
  delay(4000);

  if(failCount >= MAX_FAIL){
    lcdPrint("Akses Ditolak","");
    delay(4000);
    resetSystem();
    return;
  }

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
    client.publish(TOPIC_RFID, String(uid).c_str());  // ✔ kirim UID saja
    lcdPrint("Kartu Berhasil", "");
    delay(2000);
    currentStep = WAIT_FINGER;
    lcdPrint("Tempel Jari", "");
  } else {
    client.publish(TOPIC_RFID, "UNKNOWN");  // ✔ gagal
    handleFail("Kartu Salah");
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

      enrollStep = 1;
      return;
    }

    // STEP 2
    if (enrollStep == 1) {
      lcdPrint("Tempel Lagi", "Langkah 2");

      if (finger.getImage()!=FINGERPRINT_OK) return;
      if (finger.image2Tz(2)!=FINGERPRINT_OK) return;

      // buat model
      if (finger.createModel()!=FINGERPRINT_OK) {
        client.publish(TOPIC_FINGER, "ERROR");
        isEnrolling = false;
        enrollStep = 0;
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
      lcdPrint("Scan Sidik...","");

    if (finger.image2Tz()!=FINGERPRINT_OK) return;
    
    if (finger.fingerFastSearch()!=FINGERPRINT_OK) {
      client.publish(TOPIC_FINGER, "FAIL");
      sentProcess = false;
        handleFail("Sidik Salah");
        return;
    }

    if (finger.fingerID == AUTHORIZED_FINGER_ID) {
      client.publish(TOPIC_FINGER, "MATCH");
      sentProcess = false;

      lcdPrint("Sidik OK","");
      delay(2000);

      lcdPrint("AKSES DITERIMA","");
      relayBuka();
      delay(2000);
      relayKunci();

      resetSystem();
  } else {
      client.publish(TOPIC_FINGER, "UNKNOWN");
      sentProcess = false;
      handleFail("Tidak Terdaftar");
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

    lcdPrint("MODE ENROLL", ("ID: " + String(enrollID)).c_str());
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

  lcdPrint("IP:", WiFi.localIP().toString().c_str());
  delay(2000);

  client.setServer(mqtt_server,mqtt_port);
  client.setCallback(callback);

  fingerSerial.begin(57600,SERIAL_8N1,16,17);
  finger.begin(57600);

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