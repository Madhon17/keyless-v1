/*
  KODE FINAL V3 (dimodifikasi: tambahkan opsi akses IP + domain local mDNS)
  - Basis: KODE FINAL V3 (AUTO/MANUAL, responsive UI, buzzer, WiFi/IP, log, login sederhana)
  - Tambahan:
    * Login lebih aman: password di-hash (SHA-256) dan disimpan di NVS; bisa diganti via Web UI
    * Session token (cookie) in-memory untuk autentikasi
    * PWA support: /manifest.json & /sw.js (service worker)
    * mDNS: akses domain local "keyless.local" ketika WiFi STA berhasil terhubung
    * Web UI: add/remove/list devices, wifi set/clear
    * add scan MAC BLE Automatis WEBUI " dekatkan perangkat ke esp32 selama 5 detik, nanti akan ada notif mac address" 
  - Komentar dalam Bahasa Indonesia
*/

#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include "mbedtls/sha256.h"
#include <ESPmDNS.h>   // <-- tambahan untuk mDNS

// ================== KONFIGURASI PIN ==================
#define RELAY_PIN    33   // Relay aktif LOW
#define LED_PIN      2
#define BUZZER_PIN   15  // Buzzer aktif HIGH

// ================== KONFIGURASI BLE ==================
#define SCAN_TIME_SEC 5       // durasi scan tiap start (detik)

// ================== KONFIGURASI RELAY ==================
#define RELAY_HYSTERESIS_MS 30000UL  // relay tetap ON 30 detik meski device hilang

// ================== NVS / DEVICE ==================
Preferences preferences;
#define MAX_DEVICES 10
String devices[MAX_DEVICES];
unsigned long lastSeenDevices[MAX_DEVICES];
int deviceCount = 0;

// ================== VAR GLOBAL ==================
BLEScan* pBLEScan;
bool relayState = false;
bool buzzerBeeped[MAX_DEVICES];
bool deviceDetected = false;
int lastBeepSecond = -1;
bool timeSynced = false;
const char* NTP_SERVER = "pool.ntp.org";
const int WIFI_CONNECT_TIMEOUT_MS = 8000;

String controlMode = "AUTO"; // default AUTO
WebServer server(80);

// ========== Authentication ==========
const char* DEFAULT_USER = "admin";
const char* DEFAULT_PASS = "1234"; // hanya dipakai saat inisialisasi; disimpan sebagai hash
String storedPassHash = ""; // hex SHA-256
String currentSessionToken = ""; // token session in-memory
bool isAuthenticated = false;

// ========== Log sederhana ==========
#define LOG_CAPACITY 10
String logs[LOG_CAPACITY];
int logCount = 0;
int logIndex = 0;

// ========== mDNS settings ==========
const char* MDNS_NAME = "keyless"; // akan menjadi keyless.local

// ========= Helpers: SHA256 hex ==========
String toHex(const unsigned char *buf, size_t len) {
  String s;
  s.reserve(len * 2);
  const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = buf[i];
    s += hex[(c >> 4) & 0xF];
    s += hex[c & 0xF];
  }
  return s;
}

String sha256Hex(const String &input) {
  unsigned char output[32];
  mbedtls_sha256_ret((const unsigned char*)input.c_str(), input.length(), output, 0);
  return toHex(output, 32);
}

// ================== FUNGSI SIMPAN / LOAD DEVICE ==================
void loadDevices() {
  preferences.begin("ble-devices", true);
  deviceCount = preferences.getInt("count", 0);
  if (deviceCount > MAX_DEVICES) deviceCount = MAX_DEVICES;
  for (int i = 0; i < deviceCount; i++) {
    String key = "dev" + String(i);
    devices[i] = preferences.getString(key.c_str(), "");
    lastSeenDevices[i] = 0;
    buzzerBeeped[i] = false;
  }
  preferences.end();
  deviceDetected = false;
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

// ================== FUNGSI LOG ==================
void addLog(const String &entry) {
  String ts = "";
  time_t now = time(nullptr);
  if (timeSynced && now > 1600000000) {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[32];
    sprintf(buf, "[%02d:%02d:%02d] ", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    ts = String(buf);
  } else {
    unsigned long s = millis() / 1000;
    unsigned long hh = s / 3600;
    unsigned long mm = (s % 3600) / 60;
    unsigned long ss = s % 60;
    char buf[32];
    sprintf(buf, "[UP %02lu:%02lu:%02lu] ", hh, mm, ss);
    ts = String(buf);
  }
  logs[logIndex] = ts + entry;
  logIndex = (logIndex + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) logCount++;
}

// ================== FUNGSI RELAY ==================
void setRelay(bool state) {
  relayState = state;
  digitalWrite(RELAY_PIN, state ? LOW : HIGH); // aktif LOW
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

// ================== WIFI & NTP ==================
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

// ================== mDNS helper ==================
void startMDNS() {
  // mulai mDNS hanya jika WiFi STA connected
  if (WiFi.status() == WL_CONNECTED) {
    if (!MDNS.begin(MDNS_NAME)) {
      Serial.println("⚠️ Gagal memulai mDNS");
    } else {
      Serial.println("✅ mDNS berjalan: http://" + String(MDNS_NAME) + ".local");
      // optional: set TXT record atau hostname lain
      MDNS.addService("http", "tcp", 80);
      addLog("mDNS aktif: " + String(MDNS_NAME) + ".local");
    }
  }
}

void stopMDNSIfRunning() {
  // ada MDNS.end() di kasus disconnect untuk jaga-jaga
  MDNS.end();
}

// ================== CALLBACK SCAN BLE ==================
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

// ================== BANTUAN: Beep pendek ==================
void buzzerBeepShort() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(120);
  digitalWrite(BUZZER_PIN, LOW);
}

// ================== AUTH HELPERS ==================

// Ambil cookie "session" dari header Cookie (jika ada)
String getSessionFromCookie() {
  if (!server.hasHeader("Cookie")) return "";
  String cookie = server.header("Cookie");
  int idx = cookie.indexOf("session=");
  if (idx < 0) return "";
  int start = idx + 8;
  int end = cookie.indexOf(';', start);
  if (end < 0) end = cookie.length();
  return cookie.substring(start, end);
}

// Buat token session sederhana
String genSessionToken() {
  uint64_t r = ((uint64_t)esp_random() << 32) ^ esp_random();
  char buf[33];
  sprintf(buf, "%016llx", (unsigned long long)r);
  return String(buf);
}

// Middleware autentikasi: cek in-memory atau cookie
bool requireAuth() {
  // jika sudah autentikasi in-memory
  if (isAuthenticated) return true;
  // cek cookie session
  String s = getSessionFromCookie();
  if (s.length() && s == currentSessionToken) {
    isAuthenticated = true;
    return true;
  }
  // belum terautentikasi => redirect ke login
  server.sendHeader("Location", "/login");
  server.send(303);
  return false;
}

// ================== HANDLER: LOGIN PAGE & PROCESS ==================
void handleLoginPage() {
  // Halaman login sederhana (GET)
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Login</title><style>body{font-family:Arial;padding:20px;text-align:center}input{padding:10px;margin:8px;width:90%}button{padding:12px;margin-top:8px;width:95%}</style></head><body>";
  html += "<h2>🔑 Keyless Emergency 🔑</h2>";
  html += "<form action='/doLogin' method='GET'>";
  html += "<input name='user' placeholder='user' autofocus><br>";
  html += "<input name='pass' placeholder='password' type='password'><br>";
  html += "<button type='submit'>Login</button>";
  html += "</form>";
  //html += "<p>Default: <b>admin</b> / <b>1234</b></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleDoLogin() {
  if (!server.hasArg("user") || !server.hasArg("pass")) {
    server.send(400, "text/plain", "user & pass required");
    return;
  }
  String u = server.arg("user");
  String p = server.arg("pass");

  // hanya user 'admin' diterima
  if (u != String(DEFAULT_USER)) {
    addLog("Gagal login: user invalid " + u);
    String html = "<script>alert('Login gagal: user salah');window.location='/login';</script>";
    server.send(200, "text/html", html);
    return;
  }

  String h = sha256Hex(p);
  if (h == storedPassHash) {
    // sukses
    currentSessionToken = genSessionToken();
    isAuthenticated = true;
    addLog("Login berhasil via Web UI");
    server.sendHeader("Set-Cookie", "session=" + currentSessionToken + "; Path=/; HttpOnly");
    server.sendHeader("Location", "/");
    server.send(303);
  } else {
    addLog("Gagal login (password salah) user:" + u);
    String html = "<script>alert('Login gagal: password salah');window.location='/login';</script>";
    server.send(200, "text/html", html);
  }
}


void handleLogout() {
  // hapus session in-memory & cookie
  isAuthenticated = false;
  currentSessionToken = "";
  addLog("Logout Web UI");
  // set cookie kosong untuk override
  server.sendHeader("Set-Cookie", "session=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(303);
}

// ================== HANDLER: CHANGE PASSWORD ==================
void handleChangePassPage() {
  if (!requireAuth()) return;
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Change Password</title><style>body{font-family:Arial;padding:20px;text-align:center}input{padding:10px;margin:8px;width:90%}button{padding:12px;margin-top:8px;width:95%}</style></head><body>";
  html += "<h2>Ubah Password</h2>";
  html += "<form action='/doChangePass' method='GET'>";
  html += "<input name='old' placeholder='password lama' type='password'><br>";
  html += "<input name='newp' placeholder='password baru' type='password'><br>";
  html += "<button type='submit'>Ganti Password</button>";
  html += "</form>";
  html += "<p><a href='/'>Kembali</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleDoChangePass() {
  if (!requireAuth()) return;
  if (!server.hasArg("old") || !server.hasArg("newp")) {
    server.send(400, "text/plain", "old & newp required");
    return;
  }
  String oldp = server.arg("old");
  String newp = server.arg("newp");
  if (sha256Hex(oldp) != storedPassHash) {
    addLog("Gagal ubah password: password lama salah");
    server.send(401, "text/plain", "Password lama salah");
    return;
  }
  // simpan hash baru ke NVS
  String newHash = sha256Hex(newp);
  preferences.begin("auth", false);
  preferences.putString("passhash", newHash);
  preferences.end();
  storedPassHash = newHash;
  addLog("Password diubah via Web UI");
  server.sendHeader("Location", "/");
  server.send(303);
}

// ================== HANDLER: PWA manifest & service worker ==============
// simple manifest
void handleManifest() {
  String json = "{";
  json += "\"name\":\"ESP32 Keyless\",\"short_name\":\"ESP-Keyless\",\"start_url\":\"/\",\"display\":\"standalone\",";
  json += "\"background_color\":\"#f4f4f4\",\"theme_color\":\"#007bff\",";
  json += "\"icons\":[{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"},{\"src\":\"/icon-512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\"}]";
  json += "}";
  server.send(200, "application/json", json);
}

// service worker JS (small, cache-first strategy)
void handleServiceWorker() {
  String js = "const CACHE_NAME='esp32-cache-v1';const urls=['/','/manifest.json'];self.addEventListener('install',e=>{e.waitUntil(caches.open(CACHE_NAME).then(c=>c.addAll(urls)))});self.addEventListener('fetch',e=>{e.respondWith(caches.match(e.request).then(r=>r||fetch(e.request)))});";
  server.send(200, "application/javascript", js);
}

// ================== HANDLER: ROOT (Web UI utama responsif) ==================
void handleRoot() {
  if (!requireAuth()) return;

  // Ambil info WiFi/IP
  String ip = "";
  String wifiStat = "Disconnected";
  bool mdnsAvailable = false;
  if ((WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED) {
    ip = WiFi.localIP().toString();
    wifiStat = "Connected";
    mdnsAvailable = true;
  } else {
    ip = WiFi.softAPIP().toString();
    wifiStat = "AP Mode";
    mdnsAvailable = false;
  }

  // Build halaman HTML responsif (mirip V2) + link PWA & change password
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Keyless Management</title>";
  // PWA manifest + sw register
  html += "<link rel='manifest' href='/manifest.json'>";
  html += "<meta name='theme-color' content='#007bff'/>";
  html += "<style>";
  html += "body{font-family:Arial,Helvetica,sans-serif;margin:0;padding:20px;text-align:center;background:#f4f4f4;}";
  html += "h2{color:#333;}";
  html += ".status{margin:6px;font-size:1.05em;font-weight:bold;}";
  html += ".info{font-size:0.9em;color:#444;margin-bottom:8px}";
  html += ".container{display:flex;flex-wrap:wrap;justify-content:center;margin-top:12px;}";
  html += "button{flex:1 1 40%;max-width:200px;padding:15px;margin:10px;font-size:1.05em;border:none;border-radius:8px;color:#fff;cursor:pointer;}";
  html += "button:active{transform:scale(0.98);}";
  html += ".on{background:#28a745;} .off{background:#dc3545;} .mode{background:#007bff;} .sec{background:#6c757d;}";
  html += ".logs{width:100%;max-width:720px;margin:14px auto;text-align:left;background:#fff;padding:10px;border-radius:8px;box-shadow:0 1px 3px rgba(0,0,0,0.08)}";
  html += ".logs h3{margin:6px 0 8px 0;font-size:1em}";
  html += ".logitem{font-size:0.9em;padding:4px 0;border-bottom:1px solid #eee}";
  html += ".toprow{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center}";
  html += ".toprow .left{flex:1;text-align:left}";
  html += ".toprow .right{flex:1;text-align:right;font-size:0.9em}";
  html += "a.btn{color:#fff;padding:3px 10px;border-radius:6px;text-decoration:none}";
  html += "</style></head><body>";

  html += "<div class='toprow'><div class='left'><h2>🔑 ESP32 Keyless Emergency 🔑</h2></div>";
  html += "<div class='right'><a href='/changePass' class='btn sec'>Ganti Password</a> &nbsp; <a href='/logout' class='btn sec'>Logout</a></div></div>";

  // Link ke Devices & Wifi pages
  html += "<div style='margin-top:8px;'><a href='/devices' class='btn sec' style='margin-right:8px'>Kelola Devices</a><a href='/wifi' class='btn sec'>WiFi Config</a></div>";

  // Tampilkan info koneksi dan kedua opsi akses (IP + mDNS jika ada)
  html += "<div class='info'>WiFi: " + wifiStat + " &nbsp; | &nbsp; IP: " + ip + "</div>";
  if (mdnsAvailable) {
    html += "<div class='info'>Akses juga via domain lokal: <a href='http://" + String(MDNS_NAME) + ".local' target='_blank'>http://" + String(MDNS_NAME) + ".local</a></div>";
  } else {
    html += "<div class='info' style='font-size:0.85em;color:#888'>Domain lokal (keyless.local) hanya tersedia saat perangkat tersambung ke jaringan WiFi Anda.</div>";
  }

  html += "<div class='status'>Keyless: " + controlMode + " &nbsp; | &nbsp; Keyless: " + String(relayState ? "ON" : "OFF") + "</div>";

  // Tombol mode (AUTO / MANUAL)
  html += "<div class='container'>";
  html += "<form action='/setMode'><button class='mode' name='mode' value='AUTO' type='submit'>AUTO</button></form>";
  html += "<form action='/setMode'><button class='mode' name='mode' value='MANUAL' type='submit'>MANUAL</button></form>";
  html += "</div>";

  // Tombol relay (aktif hanya jika MANUAL)
  html += "<div class='container'>";
  html += "<form action='/relay'><button class='on' name='state' value='ON' type='submit'>ON</button></form>";
  html += "<form action='/relay'><button class='off' name='state' value='OFF' type='submit'>OFF</button></form>";
  html += "</div>";

  // Tampilkan log terakhir
  html += "<div class='logs'><h3>Log Aktivitas (terakhir " + String(logCount) + ")</h3>";
  if (logCount == 0) {
    html += "<div class='logitem'>(belum ada aktivitas)</div>";
  } else {
    int idx = (logIndex - 1 + LOG_CAPACITY) % LOG_CAPACITY;
    for (int k = 0; k < logCount; k++) {
      html += "<div class='logitem'>" + logs[idx] + "</div>";
      idx = (idx - 1 + LOG_CAPACITY) % LOG_CAPACITY;
    }
  }
  html += "</div>";

  // Service Worker register script
  html += "<script>";
  html += "if('serviceWorker' in navigator){navigator.serviceWorker.register('/sw.js').then(()=>console.log('SW registered')).catch(()=>console.log('SW reg failed'));}";
  html += "</script>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ================== HANDLER: SET MODE ==================
void handleSetMode() {
  if (!requireAuth()) return;

  if (server.hasArg("mode")) {
    String newMode = server.arg("mode");
    newMode.toUpperCase();
    if (newMode != controlMode) {
      controlMode = newMode;
      buzzerBeepShort();
      addLog("Mode diubah ke: " + controlMode);
      Serial.println("🔀 Mode diubah ke: " + controlMode);
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// ================== HANDLER: RELAY CONTROL (manual) ==================
void handleRelay() {
  if (!requireAuth()) return;

  if (controlMode != "MANUAL") {
    addLog("Percobaan kontrol keyless ditolak (mode bukan MANUAL)");
    server.send(200, "text/plain", "keyless hanya bisa dikontrol saat mode MANUAL aktif. Kembali ke halaman utama.");
    return;
  }

  if (server.hasArg("state")) {
    String s = server.arg("state");
    if (s == "ON") {
      setRelay(true);
      addLog("keyless di-ON-kan (manual)");
      Serial.println(getTimeStamp() + " 🔧 Keyless ON (manual)");
    } else {
      setRelay(false);
      addLog("keyless di-OFF-kan (manual)");
      Serial.println(getTimeStamp() + " 🔧 Keyless OFF (manual)");
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// ================== HANDLER: Devices Page (list + add form + remove) ==================
// ================== HANDLER: Scan Nearest BLE Device ==================
void handleScanNearestMac() {
  if (!requireAuth()) return;

  int scanTime = 5; // detik
  BLEScanResults foundDevices = pBLEScan->start(scanTime, false);
  int bestRSSI = -999;
  String bestMAC = "";

  for (int i = 0; i < foundDevices.getCount(); i++) {
    BLEAdvertisedDevice dev = foundDevices.getDevice(i);
    int rssi = dev.getRSSI();
    String mac = dev.getAddress().toString().c_str();

    if (rssi > bestRSSI) {
      bestRSSI = rssi;
      bestMAC = mac;
    }
  }
  pBLEScan->clearResults();

  if (bestMAC.length() > 0) {
    server.send(200, "text/plain", bestMAC);
  } else {
    server.send(200, "text/plain", "");
  }
}

// ================== UPDATE handleDevicesPage ==================
void handleDevicesPage() {
  if (!requireAuth()) return;

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Kelola Devices</title><style>body{font-family:Arial;padding:16px}input{padding:8px;width:70%;}button{padding:10px;margin:6px;}table{width:100%;border-collapse:collapse}th,td{padding:8px;border-bottom:1px solid #ddd;text-align:left} .small{font-size:0.9em;color:#666}</style></head><body>";
  html += "<h2>Kelola Devices BLE</h2>";
  html += "<p class='small'>Tambahkan MAC address device iTag (format: AA:BB:CC:DD:EE:FF)</p>";

  // Form add device + tombol scan
  html += "<form action='/doAddDevice' method='GET'>";
  html += "<input id='macInput' name='mac' placeholder='MAC address (contoh: AA:BB:CC:DD:EE:FF)'> ";
  html += "<button type='submit'>Tambah Device</button>";
  html += "<button type='button' onclick='scanMac()'>Scan MAC Terdekat</button>";
  html += "</form>";

  // Script untuk isi otomatis
  html += "<script>";
  html += "function scanMac(){";
  html += "fetch('/scanNearestMac').then(r=>r.text()).then(mac=>{";
  html += " if(mac){document.getElementById('macInput').value = mac; alert('Ditemukan: '+mac);} else {alert('Tidak ada device terdeteksi');}";
  html += "});}";
  html += "</script>";

  // Tampilkan daftar
  html += "<h3>Daftar Terdaftar (" + String(deviceCount) + ")</h3>";
  html += "<table><tr><th>No</th><th>MAC</th><th>Aksi</th></tr>";
  if (deviceCount == 0) {
    html += "<tr><td colspan='3'>(kosong)</td></tr>";
  } else {
    for (int i = 0; i < deviceCount; i++) {
      html += "<tr><td>" + String(i+1) + "</td><td>" + devices[i] + "</td>";
      html += "<td><a href='/doRemoveDevice?mac=" + devices[i] + "'><button>Hapus</button></a></td></tr>";
    }
  }
  html += "</table>";
  html += "<p><a href='/'>Kembali</a></p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}


void handleDoAddDevice() {
  if (!requireAuth()) return;

  if (!server.hasArg("mac")) {
    server.send(400, "text/plain", "mac required");
    return;
  }
  String mac = server.arg("mac");
  mac.trim();
  if (mac.length() == 0) {
    server.send(400, "text/plain", "mac empty");
    return;
  }
  // Cek sudah ada?
  bool exists = false;
  for (int i = 0; i < deviceCount; i++) if (devices[i].equalsIgnoreCase(mac)) exists = true;
  if (exists) {
    addLog("Percobaan tambah device (sudah ada): " + mac);
    server.sendHeader("Location", "/devices");
    server.send(303);
    return;
  }
  if (deviceCount >= MAX_DEVICES) {
    addLog("Percobaan tambah device gagal (penuh): " + mac);
    server.sendHeader("Location", "/devices");
    server.send(303);
    return;
  }
  devices[deviceCount] = mac;
  lastSeenDevices[deviceCount] = 0;
  buzzerBeeped[deviceCount] = false;
  deviceCount++;
  saveDevices();
  addLog("Device ditambahkan (web): " + mac);
  Serial.println("✅ Device ditambahkan (web): " + mac);
  server.sendHeader("Location", "/devices");
  server.send(303);
}

void handleDoRemoveDevice() {
  if (!requireAuth()) return;

  if (!server.hasArg("mac")) {
    server.send(400, "text/plain", "mac required");
    return;
  }
  String mac = server.arg("mac");
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
      addLog("Device dihapus (web): " + mac);
      Serial.println("🗑️ Device dihapus (web): " + mac);
      break;
    }
  }
  if (!found) {
    addLog("Percobaan hapus device gagal (tidak ditemukan): " + mac);
  }
  server.sendHeader("Location", "/devices");
  server.send(303);
}

// ================== HANDLER: WiFi config page & actions ==================
void handleWifiPage() {
  if (!requireAuth()) return;

  // ambil current wifi creds (tampil ssid saja, password kosong)
  preferences.begin("wifi", true);
  String ssid = preferences.getString("ssid", "");
  preferences.end();

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>WiFi Config</title><style>body{font-family:Arial;padding:16px}input{padding:8px;width:80%;}button{padding:10px;margin:6px;} .small{font-size:0.9em;color:#666}</style></head><body>";
  html += "<h2>Konfigurasi WiFi</h2>";
  html += "<p class='small'>Masukkan SSID dan Password jaringan Anda. Setelah simpan, ESP akan mencoba menghubungkan. Jika berhasil, mDNS (keyless.local) akan aktif.</p>";
  html += "<form action='/doWifiSave' method='GET'>";
  html += "<input name='ssid' placeholder='SSID' value='" + ssid + "'><br><br>";
  html += "<input name='pass' placeholder='Password' type='password'><br><br>";
  html += "<button type='submit'>Simpan & Hubungkan</button>";
  html += "</form>";
  html += "<form action='/doClearWifi' method='GET' style='margin-top:12px;'><button type='submit'>Clear WiFi (hapus kredensial)</button></form>";
  html += "<form action='/listWifi' method='GET' style='margin-top:12px;'><button type='submit'>List WiFi Tersimpan</button></form>";
  html += "<p><a href='/'>Kembali</a></p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// ================== HANDLER: List WiFi (tampil SSID tersimpan) ==================
void handleListWifi() {
  if (!requireAuth()) return;

  preferences.begin("wifi", true);
  String ssid = preferences.getString("ssid", "");
  preferences.end();

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>WiFi List</title>";
  html += "<style>body{font-family:Arial;padding:16px;} table{width:100%;border-collapse:collapse;} th,td{padding:8px;border-bottom:1px solid #ddd;} button{padding:8px;margin:6px;} </style></head><body>";
  html += "<h2>SSID WiFi Tersimpan</h2>";
  html += "<table><tr><th>No</th><th>SSID</th></tr>";
  if (ssid.length() == 0) {
    html += "<tr><td colspan='2'>(kosong)</td></tr>";
  } else {
    html += "<tr><td>1</td><td>" + ssid + "</td></tr>";
  }
  html += "</table>";
  html += "<p><a href='/wifi'>Kembali</a></p></body></html>";

  server.send(200, "text/html", html);
}


void handleDoWifiSave() {
  if (!requireAuth()) return;
  if (!server.hasArg("ssid") || !server.hasArg("pass")) {
    server.send(400, "text/plain", "ssid & pass required");
    return;
  }
  String ssid = server.arg("ssid"); ssid.trim();
  String pass = server.arg("pass"); pass.trim();
  if (ssid.length() == 0) {
    server.send(400, "text/plain", "ssid empty");
    return;
  }
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();
  addLog("Kredensial WiFi disimpan via Web UI: " + ssid);
  Serial.println("✅ Kredensial WiFi disimpan. Mencoba koneksi...");
  // coba koneksi sekarang
  if (connectWiFiFromPrefs()) {
    startNTP();
    addLog("WiFi terhubung via Web UI: " + WiFi.localIP().toString());
    startMDNS();
  } else {
    addLog("Kredensial WiFi disimpan tetapi gagal konek: " + ssid);
    Serial.println("⚠️ Koneksi WiFi gagal. Periksa SSID/password.");
  }
  server.sendHeader("Location", "/wifi");
  server.send(303);
}

void handleDoClearWifi() {
  if (!requireAuth()) return;
  preferences.begin("wifi", false);
  preferences.remove("ssid");
  preferences.remove("pass");
  preferences.end();
  addLog("Kredensial WiFi dihapus via Web UI");
  Serial.println("✅ Kredensial WiFi dihapus dari NVS (via Web UI)");
  // hentikan mDNS bila berjalan
  stopMDNSIfRunning();

  server.sendHeader("Location", "/wifi");
  server.send(303);
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);

  // pin
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // pastikan relay OFF saat boot
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  relayState = false;
  deviceDetected = false;
  lastBeepSecond = -1;

  // load device BLE terdaftar
  loadDevices();

  // Inisialisasi password hash di NVS jika belum ada
  preferences.begin("auth", true);
  storedPassHash = preferences.getString("passhash", "");
  preferences.end();
  if (storedPassHash.length() == 0) {
    // simpan default hash dari DEFAULT_PASS
    String ph = sha256Hex(String(DEFAULT_PASS));
    preferences.begin("auth", false);
    preferences.putString("passhash", ph);
    preferences.end();
    storedPassHash = ph;
    Serial.println("⚠️ Password default disimpan (silakan ganti segera)");
  }

  // init BLE scan
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);

  // coba konek WiFi dari NVS (jika ada)
  bool wifiOk = connectWiFiFromPrefs();
  if (wifiOk) {
    startNTP();
    addLog("WiFi connected: " + WiFi.localIP().toString());
    Serial.println("📡 Web UI akan aktif di IP: " + WiFi.localIP().toString());

    // mulai mDNS untuk domain lokal (keyless.local)
    startMDNS();
    Serial.println("🌐 Akses: http://" + WiFi.localIP().toString() + " atau http://" + String(MDNS_NAME) + ".local");
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-Setup");
    addLog("Access Point dibuat: ESP32-Setup (192.168.4.1)");
    Serial.println("📡 Web UI (AP) tersedia di 192.168.4.1");
    Serial.println("ℹ️ Mode AP - domain local tidak tersedia. Sambungkan ESP ke jaringan Anda agar keyless.local aktif.");
  }

  // Setup routes
  server.on("/login", HTTP_GET, handleLoginPage);
  server.on("/doLogin", HTTP_GET, handleDoLogin);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/changePass", HTTP_GET, handleChangePassPage);
  server.on("/doChangePass", HTTP_GET, handleDoChangePass);
  server.on("/manifest.json", HTTP_GET, handleManifest);
  server.on("/sw.js", HTTP_GET, handleServiceWorker);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/setMode", HTTP_GET, handleSetMode);
  server.on("/relay", HTTP_GET, handleRelay);

  // NEW: Devices & WiFi routes (Web UI)
  server.on("/devices", HTTP_GET, handleDevicesPage);
  server.on("/doAddDevice", HTTP_GET, handleDoAddDevice);
  server.on("/doRemoveDevice", HTTP_GET, handleDoRemoveDevice);
  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/doWifiSave", HTTP_GET, handleDoWifiSave);
  server.on("/doClearWifi", HTTP_GET, handleDoClearWifi);
  server.on("/scanNearestMac", handleScanNearestMac);
  server.on("/listWifi", HTTP_GET, handleListWifi);


  server.begin();
  Serial.println("\n=== MODE SERIAL ===");
  Serial.println("Gunakan perintah serial: add/remove/list/wifi/clearwifi");
  Serial.println("===================");
}

// ================== LOOP ==================
void loop() {
  // tangani client web
  server.handleClient();

  // tangani perintah serial (sama seperti V2)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      if (cmd.startsWith("add ")) {
        String mac = cmd.substring(4); mac.trim();
        bool exists = false;
        for (int i = 0; i < deviceCount; i++) if (devices[i].equalsIgnoreCase(mac)) exists = true;
        if (!exists && deviceCount < MAX_DEVICES) {
          devices[deviceCount] = mac;
          lastSeenDevices[deviceCount] = 0;
          buzzerBeeped[deviceCount] = false;
          deviceCount++;
          saveDevices();
          Serial.println("✅ Device ditambahkan: " + mac);
          addLog("Device ditambahkan (serial): " + mac);
        } else Serial.println("⚠️ Device sudah ada atau slot penuh");
      } else if (cmd.startsWith("remove ")) {
        String mac = cmd.substring(7); mac.trim();
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
            addLog("Device dihapus (serial): " + mac);
            break;
          }
        }
        if (!found) Serial.println("⚠️ Device tidak ditemukan");
      } else if (cmd.equalsIgnoreCase("list")) {
        Serial.println("📋 Daftar Device Terdaftar:");
        if (deviceCount == 0) Serial.println("   (kosong)");
        else for (int i = 0; i < deviceCount; i++) Serial.printf("   %d. %s\n", i + 1, devices[i].c_str());
      } else if (cmd.startsWith("wifi ")) {
        int sp = cmd.indexOf(' ');
        String rest = cmd.substring(sp + 1);
        int sp2 = rest.indexOf(' ');
        if (sp2 <= 0) { Serial.println("⚠️ Format wifi: wifi <SSID> <PASSWORD>"); }
        else {
          String ssid = rest.substring(0, sp2);
          String pass = rest.substring(sp2 + 1);
          ssid.trim(); pass.trim();
          preferences.begin("wifi", false);
          preferences.putString("ssid", ssid);
          preferences.putString("pass", pass);
          preferences.end();
          Serial.println("✅ Kredensial WiFi disimpan. Mencoba koneksi...");
          if (connectWiFiFromPrefs()) {
            startNTP();
            addLog("WiFi disimpan & terhubung: " + WiFi.localIP().toString());
            // restart mDNS jika perlu
            startMDNS();
            Serial.println("🌐 Akses: http://" + WiFi.localIP().toString() + " atau http://" + String(MDNS_NAME) + ".local");
          } else {
            addLog("Kredensial WiFi disimpan tetapi gagal konek: " + ssid);
            Serial.println("⚠️ Koneksi WiFi gagal. Periksa SSID/password.");
          }
        }
      } else if (cmd.equalsIgnoreCase("clearwifi")) {
        preferences.begin("wifi", false);
        preferences.remove("ssid");
        preferences.remove("pass");
        preferences.end();
        Serial.println("✅ Kredensial WiFi dihapus dari NVS");
        addLog("Kredensial WiFi dihapus (serial)");
        // hentikan mDNS bila berjalan
        stopMDNSIfRunning();
      } else {
        Serial.println("⚠️ Perintah tidak dikenali. Gunakan: add/remove/list/wifi/clearwifi");
      }
    }
  }

  // ================= BLE scan & AUTO logic =================
  if (controlMode == "AUTO") {
    pBLEScan->start(SCAN_TIME_SEC, false);
    pBLEScan->clearResults();

    unsigned long now = millis();
    bool anyDeviceActive = false;

    for (int i = 0; i < deviceCount; i++) {
      if (lastSeenDevices[i] > 0 && now - lastSeenDevices[i] < RELAY_HYSTERESIS_MS) {
        anyDeviceActive = true;
        if (!relayState) {
          setRelay(true);
          deviceDetected = true;
          buzzerBeepShort();
          addLog("Keyless ON (BLE detected) - " + devices[i]);
          Serial.println(getTimeStamp() + " 📡 Device resmi terdeteksi → Keyless ON");
        }
        break;
      }
    }

    if (!anyDeviceActive && relayState && deviceDetected) {
      unsigned long mostRecentSeen = 0;
      for (int i = 0; i < deviceCount; i++) if (lastSeenDevices[i] > mostRecentSeen) mostRecentSeen = lastSeenDevices[i];

      if (mostRecentSeen > 0) {
        unsigned long lostDuration = now - mostRecentSeen;
        unsigned long lostSec = lostDuration / 1000UL;

        if (lostSec >= 25 && lostSec < (RELAY_HYSTERESIS_MS/1000) && (int)lostSec != lastBeepSecond) {
          lastBeepSecond = (int)lostSec;
          digitalWrite(BUZZER_PIN, HIGH);
          delay(100);
          digitalWrite(BUZZER_PIN, LOW);
          Serial.println(getTimeStamp() + " ⏰ Warning: countdown sebelum Keyless OFF (detik " + String(lostSec) + ")");
        }

        if (lostDuration >= RELAY_HYSTERESIS_MS) {
          digitalWrite(BUZZER_PIN, HIGH);
          delay(400);
          digitalWrite(BUZZER_PIN, LOW);
          setRelay(false);
          deviceDetected = false;
          for (int i = 0; i < deviceCount; i++) buzzerBeeped[i] = false;
          lastBeepSecond = -1;
          addLog("Keyless OFF (BLE lost - auto)");
          Serial.println(getTimeStamp() + " ⏹ Semua device hilang → Keyless OFF");
        }
      } else {
        setRelay(false);
        deviceDetected = false;
        addLog("Keyless OFF (BLE lost - no record)");
      }
    }
  } // end AUTO

  delay(50);
}
