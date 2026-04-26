#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

const char* ssid = "Nell";
const char* password = "Dimasu(kin)";

const char* mqtt_server = "294542ce25054f91be349c2b99c45ebc.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;

const char* mqtt_user = "Sentinel-box";
const char* mqtt_pass = "Sentinelbox123";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// --- Publish status balik ke broker ---
void publishStatus(const char* status) {
  client.publish("brankas/status", status);
  Serial.print("[MQTT] Status dikirim: ");
  Serial.println(status);
}

void callback(char* topic, byte* payload, unsigned int length) {
  // Rakit payload jadi string
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("[MQTT] Pesan masuk di topic [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  // Handle perintah kunci
  if (String(topic) == "brankas/kunci") {
    if (message == "UNLOCK") {
      Serial.println("[ACTION] Membuka brankas...");
      // TODO: tambah logika relay/servo di sini
      publishStatus("terbuka");

    } else if (message == "LOCK") {
      Serial.println("[ACTION] Mengunci brankas...");
      // TODO: tambah logika relay/servo di sini
      publishStatus("terkunci");

    } else {
      Serial.println("[ACTION] Perintah tidak dikenal");
      publishStatus("error");
    }
  }
}

void connectMQTT() {
  while (!client.connected()) {
    Serial.print("[MQTT] Connecting...");
    if (client.connect("ESP32Client", mqtt_user, mqtt_pass)) {
      Serial.println("Connected!");
      client.subscribe("brankas/kunci");
      publishStatus("online"); // kasih tau Python ESP32 udah nyala
    } else {
      Serial.print("Gagal, rc=");
      Serial.println(client.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());

  espClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();
}