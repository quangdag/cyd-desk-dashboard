// Tuy chon. Copy file nay thanh secrets.h neu muon nap san WiFi / token vao firmware.
// Khong co secrets.h (hoac de trong) thi lan dau bat may CYD phat WiFi "CYD-Setup" de nhap qua dien thoai.
// secrets.h da nam trong .gitignore - khong dua len GitHub.
#pragma once

// ===== WiFi =====
static const char WIFI_SSID[]     = "";
static const char WIFI_PASSWORD[] = "";

// ===== Claude usage (server tren PC: pc_server/claude_usage_server.py) =====
#define CLAUDE_ENABLED 1                            // 0 = tat cac trang can PC
static const char CLAUDE_HOST[]  = "";              // IP LAN cua PC; de trong = tu tim qua UDP
static const uint16_t CLAUDE_PORT = 7842;
static const char CLAUDE_TOKEN[] = "";              // phai trung voi token cua server
