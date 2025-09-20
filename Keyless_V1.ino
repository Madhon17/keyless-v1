#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <time.h>

// ================== KONFIGURASI PIN ==================
#define RELAY_PIN    33   // Relay aktif LOW
#define LED_PIN      2
#define BUZZER_PIN   15  // Buzzer aktif HIGH

// ================== KONFIGURASI BLE ==================
#define SCAN_TIME_SEC 5       // durasi scan tiap start (detik)

// ================== KONFIGURASI RELAY ==================
#define RELAY_HYSTERESIS_MS 30000UL  // relay tetap ON 30 detik meski device hilang

// ================== KONFIGURASI MULTI DEVICE & NVS ==================
Preferences preferences;
#define MAX_DEVICES 10
String devices[MAX_DEVICES];
unsigned long lastSeenDevices[MAX_DEVICES];
int deviceCount = 0;

// ================== VARIABEL GLOBAL ==================
BLEScan* pBLEScan;
bool relayState = false;               
bool buzzerBeeped[MAX_DEVICES];        
bool deviceDetected = false;           // device resmi terdeteksi untuk relay ON
int lastBeepSecond = -1;               // detik terakhir beep countdown
bool timeSynced = false;
const char* NTP_SERVER = "pool.ntp.org";
const int WIFI_CONNECT_TIMEOUT_MS = 8000;

// ================== FUNGSI SIMPAN / LOAD DEVICE ==================
void loadDevices() {
  preferences.begin("ble-devices", true);
  deviceCount = preferences.getInt("count", 0);
  if (deviceCount > MAX_DEVICES) deviceCount = MAX_DEVICES;
  for (int i = 0; i < deviceCount; i++) {
    String key = "dev" + String(i);
    devices[i] = preferences.getString(key.c_str(), "");
    lastSeenDevices[i] = 0;  // pastikan reset
    buzzerBeeped[i] = false;
  }
  preferences.end();
  deviceDetected = false; // pastikan relay OFF saat boot
}

void saveDevices() {
  preferences.begin("ble-devices", false);
  preferences.putInt("count", deviceCount);
  for (int i = 0; i < deviceCount; i++) {
    String key = "dev" + String(i);
    preferences.putString(key.c_str(), devices[i]);
  }
  preferences.end();
}

// ================== FUNGSI RELAY ==================
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, state ? LOW : HIGH);
  digitalWrite(LED_PIN, state ? HIGH : LOW);
}

// ================== FUNGSI TIMESTAMP ==================
String getTimeStamp() {
  time_t now = time(nullptr);
  if (timeSynced && now > 1600000000) {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[32];
    sprintf(buf, "[%02d:%02d:%02d WIB]", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buf);
  } else {
    unsigned long s = millis() / 1000;
    unsigned long hh = s / 3600;
    unsigned long mm = (s % 3600) / 60;
    unsigned long ss = s % 60;
    char buf[32];
    sprintf(buf, "[UP %02lu:%02lu:%02lu]", hh, mm, ss);
    return String(buf);
  }
}

// ================== HELPERS WIFI & NTP ==================
void saveWifiCredentials(const String &ssid, const String &pass) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();
}

void clearWifiCredentials() {
  preferences.begin("wifi", false);
  preferences.remove("ssid");
  preferences.remove("pass");
  preferences.end();
}

bool connectWiFiFromPrefs() {
  preferences.begin("wifi", true);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  preferences.end();
  if (ssid.length() == 0) return false;
  Serial.println(String("Menghubungkan ke WiFi: ") + ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("✅ WiFi connected, IP: " + WiFi.localIP().toString());
      return true;
    }
    delay(200);
  }
  Serial.println("❌ Gagal konek WiFi (timeout)");
  return false;
}

bool connectWiFi(const String &ssid, const String &pass) {
  Serial.println(String("Menghubungkan ke WiFi: ") + ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("✅ WiFi connected, IP: " + WiFi.localIP().toString());
      return true;
    }
    delay(200);
  }
  Serial.println("❌ Gagal konek WiFi (timeout)");
  return false;
}

void startNTP() {
  configTime(7 * 3600, 0, NTP_SERVER);
  unsigned long start = millis();
  while (millis() - start < 5000) {
    time_t now = time(nullptr);
    if (now > 1600000000) {
      timeSynced = true;
      Serial.println("✅ Waktu berhasil disinkronkan via NTP: " + getTimeStamp());
      return;
    }
    delay(200);
  }
  Serial.println("⚠️ NTP sinkronisasi belum berhasil (akan coba lagi nanti)");
  timeSynced = false;
}

// ================== CALLBACK SCAN ==================
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    String addr = advertisedDevice.getAddress().toString().c_str();
    for (int i = 0; i < deviceCount; i++) {
      if (addr.equalsIgnoreCase(devices[i])) {
        lastSeenDevices[i] = millis(); 
        break;
      }
    }
  }
};

// ================== SERIAL COMMANDS ==================
void handleSerialCommand() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd.startsWith("add ")) {
    String mac = cmd.substring(4);
    mac.trim();
    bool exists = false;
    for (int i = 0; i < deviceCount; i++) if (devices[i].equalsIgnoreCase(mac)) exists = true;
    if (!exists && deviceCount < MAX_DEVICES) {
      devices[deviceCount] = mac;
      lastSeenDevices[deviceCount] = 0;
      buzzerBeeped[deviceCount] = false;
      deviceCount++;
      saveDevices();
      Serial.println("✅ Device ditambahkan: " + mac);
    } else Serial.println("⚠️ Device sudah ada atau slot penuh");
  }
  else if (cmd.startsWith("remove ")) {
    String mac = cmd.substring(7);
    mac.trim();
    bool found = false;
    for (int i = 0; i < deviceCount; i++) {
      if (devices[i].equalsIgnoreCase(mac)) {
        for (int j = i; j < deviceCount - 1; j++) {
          devices[j] = devices[j + 1];
          lastSeenDevices[j] = lastSeenDevices[j + 1];
          buzzerBeeped[j] = buzzerBeeped[j + 1];
        }
        deviceCount--;
        saveDevices();
        found = true;
        Serial.println("🗑️ Device dihapus: " + mac);
        break;
      }
    }
    if (!found) Serial.println("⚠️ Device tidak ditemukan");
  }
  else if (cmd.equalsIgnoreCase("list")) {
    Serial.println("📋 Daftar Device Terdaftar:");
    if (deviceCount == 0) Serial.println("   (kosong)");
    else for (int i = 0; i < deviceCount; i++)
      Serial.printf("   %d. %s\n", i + 1, devices[i].c_str());
  }
  else if (cmd.startsWith("wifi ")) {
    int sp = cmd.indexOf(' ');
    String rest = cmd.substring(sp + 1);
    int sp2 = rest.indexOf(' ');
    if (sp2 <= 0) { Serial.println("⚠️ Format wifi: wifi <SSID> <PASSWORD>"); return; }
    String ssid = rest.substring(0, sp2);
    String pass = rest.substring(sp2 + 1);
    ssid.trim(); pass.trim();
    if (ssid.length() == 0) { Serial.println("⚠️ SSID kosong"); return;}
    saveWifiCredentials(ssid, pass);
    Serial.println("✅ Kredensial WiFi disimpan. Mencoba koneksi...");
    if (connectWiFi(ssid, pass)) startNTP();
    else Serial.println("⚠️ Koneksi WiFi gagal. Periksa SSID/password.");
  }
  else if (cmd.equalsIgnoreCase("clearwifi")) {
    clearWifiCredentials();
    Serial.println("✅ Kredensial WiFi dihapus dari NVS");
  }
  else Serial.println("⚠️ Perintah tidak dikenali. Gunakan: add/remove/list/wifi/clearwifi");
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  relayState = false;
  lastBeepSecond = -1;
  deviceDetected = false;

  loadDevices();

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);

  Serial.println("⚡ ESP32 siap. Mode standby: menunggu device BLE terdaftar...");

  if (connectWiFiFromPrefs()) startNTP();
  else {
    Serial.println("ℹ️ Tidak ada kredensial WiFi di NVS. Gunakan perintah:");
    Serial.println("   wifi <SSID> <PASSWORD>");
  }

  Serial.println("\n=== MODE SERIAL ===");
  Serial.println("Gunakan perintah:");
  Serial.println("  add <mac_address>");
  Serial.println("  remove <mac_address>");
  Serial.println("  list");
  Serial.println("  wifi <SSID> <PASSWORD>");
  Serial.println("  clearwifi");
  Serial.println("===================");
}

// ================== LOOP ==================
void loop() {
  handleSerialCommand();

  pBLEScan->start(SCAN_TIME_SEC, false);
  pBLEScan->clearResults();

  unsigned long now = millis();
  bool anyDeviceActive = false;

  for (int i = 0; i < deviceCount; i++) {
    if (lastSeenDevices[i] > 0 && now - lastSeenDevices[i] < RELAY_HYSTERESIS_MS) {
      anyDeviceActive = true;
      if (!relayState) {
        Serial.println(getTimeStamp() + " 📡 Device resmi terdeteksi → Relay ON");
        setRelay(true);
        deviceDetected = true;

        if (!buzzerBeeped[i]) {
          digitalWrite(BUZZER_PIN, HIGH);
          delay(200);
          digitalWrite(BUZZER_PIN, LOW);
          buzzerBeeped[i] = true;
        }
      }
      break;
    }
  }

  // Countdown / relay OFF
  if (!anyDeviceActive && relayState && deviceDetected) {
    unsigned long mostRecentSeen = 0;
    for (int i = 0; i < deviceCount; i++)
      if (lastSeenDevices[i] > mostRecentSeen) mostRecentSeen = lastSeenDevices[i];

    if (mostRecentSeen > 0) {
      unsigned long lostDuration = now - mostRecentSeen;
      unsigned long lostSec = lostDuration / 1000UL;

      if (lostSec >= 25 && lostSec < 30 && (int)lostSec != lastBeepSecond) {
        lastBeepSecond = (int)lostSec;
        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println(getTimeStamp() + " ⏰ Warning: countdown sebelum Relay OFF (detik " + String(lostSec) + ")");
      }

      if (lostSec >= 30) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(500);
        digitalWrite(BUZZER_PIN, LOW);

        Serial.println(getTimeStamp() + " ⏹ Semua device hilang → Relay OFF");
        setRelay(false);
        deviceDetected = false;
        for (int i = 0; i < deviceCount; i++) buzzerBeeped[i] = false;
        lastBeepSecond = -1;
      }
    } else {
      setRelay(false);
    }
  }

  static unsigned long lastNtpTry = 0;
  if (!timeSynced && WiFi.status() == WL_CONNECTED && (millis() - lastNtpTry > 60000)) {
    lastNtpTry = millis();
    startNTP();
  }

  delay(50);
}
