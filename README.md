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
