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

const char* ssid        = "SIJABAIK";
const char* password    = "SENTINEL";
const char* mqtt_server = "10.42.0.32";
const int   mqtt_port   = 1883;
const char* mqtt_user   = "sentinel";
const char* mqtt_pass   = "Tes12345";

#define RST_PIN    4
#define SS_PIN     5
#define RELAY_PIN  13
#define CAM_RX_PIN 14
#define CAM_TX_PIN 12

#define TOPIC_KUNCI      "brankas/kunci"
#define TOPIC_CAM_RESULT "brankas/wajah/result"
#define TOPIC_RFID       "brankas/rfid"
#define TOPIC_FINGER     "brankas/sidikjari"
#define TOPIC_ENROLL     "brankas/sidik/enroll"
#define TOPIC_RELAY      "brankas/relay"
#define TOPIC_STATUS     "brankas/status"

const uint32_t AUTHORIZED_RFID_UID = 0x5909D006;
const int AUTHORIZED_FINGER_ID = 1;

LiquidCrystal_I2C lcd(0x27, 16, 2);
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger(&fingerSerial);
MFRC522 mfrc522(SS_PIN, RST_PIN);
HardwareSerial camSerial(1);

WiFiClient espClient;
PubSubClient client(espClient);

// ================= FLOW STATE =================
enum StepAuth {
  WAIT_RFID,
  WAIT_FINGER,
  WAIT_FACE
};

StepAuth currentStep = WAIT_RFID;

int failCount = 0;
const int MAX_FAIL = 5;

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
  if (currentStep != WAIT_RFID) return;

  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  uint32_t uid=0;
  for(byte i=0;i<mfrc522.uid.size;i++)
    uid=(uid<<8)|mfrc522.uid.uidByte[i];

  client.publish(TOPIC_RFID, String(uid).c_str()); // 🔥 TAMBAH

  if(uid==AUTHORIZED_RFID_UID){
    client.publish(TOPIC_RFID, "VALID"); // 🔥 TAMBAH

    lcdPrint("Kartu Berhasil","");
    delay(2000);

    currentStep = WAIT_FINGER;
    lcdPrint("Tempel Jari","");
  } else {
    client.publish(TOPIC_RFID, "INVALID"); // 🔥 TAMBAH
    handleFail("Kartu Salah");
  }

  mfrc522.PICC_HaltA();
}

// ================= FINGER =================
void checkFingerprint() {
  if (currentStep != WAIT_FINGER) return;

  if (finger.getImage()!=FINGERPRINT_OK) return;
  if (finger.image2Tz()!=FINGERPRINT_OK) return;

  if (finger.fingerFastSearch()!=FINGERPRINT_OK) {
    client.publish(TOPIC_FINGER, "FAIL"); // 🔥 TAMBAH
    handleFail("Sidik Salah");
    return;
  }

  if (finger.fingerID == AUTHORIZED_FINGER_ID) {
    client.publish(TOPIC_FINGER, "MATCH"); // 🔥 TAMBAH

    lcdPrint("Sidik Berhasil","");
    delay(2000);

    currentStep = WAIT_FACE;
    lcdPrint("Hadap Kamera","");
  } else {
    client.publish(TOPIC_FINGER, "UNKNOWN"); // 🔥 TAMBAH
    handleFail("Tidak Terdaftar");
  }
}

// ================= CAM =================
void checkCamUART() {

  if (currentStep != WAIT_FACE) return;

#if USE_UART_CAM
  while (camSerial.available()) {
    String msg = camSerial.readStringUntil('\n');
    msg.trim();

    if (msg == "WAJAH_OK") {
      lcdPrint("Wajah Berhasil","");
      delay(4000);

      lcdPrint("AKSES DITERIMA","");
      relayBuka();
      delay(5000);
      relayKunci();

      resetSystem();
    }

    else if (msg == "WAJAH_FAIL") {
      handleFail("Wajah Gagal");
    }
  }
#endif
}

// ================= MQTT =================
void callback(char* topic, byte* payload, unsigned int length) {
  String msg="";
  for(int i=0;i<length;i++) msg+=(char)payload[i];

  Serial.print("[MQTT] ");
  Serial.print(topic);
  Serial.print(" => ");
  Serial.println(msg);

  // 🔥 CONTROL DARI DASHBOARD
  if(String(topic)==TOPIC_KUNCI){
    if(msg=="UNLOCK"){
      relayBuka();
    } else if(msg=="LOCK"){
      relayKunci();
    }
  }

  // 🔥 HASIL WAJAH
  if(String(topic)==TOPIC_CAM_RESULT){
    if(currentStep != WAIT_FACE) return;

    if(msg=="WAJAH_OK"){
      lcdPrint("Wajah Berhasil","");
      delay(2000);

      lcdPrint("AKSES DITERIMA","");
      relayBuka();
      delay(5000);
      relayKunci();

      resetSystem();
    } else {
      handleFail("Wajah Gagal");
    }
  }
}

void reconnect(){
  while(!client.connected()){
    Serial.println("[MQTT] Connecting...");
    
    if(client.connect("SENTINEL",mqtt_user,mqtt_pass)){
      Serial.println("[MQTT] Connected");

      // 🔥 SUBSCRIBE SEMUA
      client.subscribe(TOPIC_CAM_RESULT);
      client.subscribe(TOPIC_KUNCI);

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

  camSerial.begin(115200,SERIAL_8N1,CAM_RX_PIN,CAM_TX_PIN);

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
  checkCamUART();
}