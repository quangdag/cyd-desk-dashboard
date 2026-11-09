// Copy file nay thanh secrets.h roi dien thong tin cua ban.
// secrets.h da nam trong .gitignore - khong dua len GitHub.
#pragma once

// ===== WiFi =====
static const char WIFI_SSID[]     = "your-wifi-name";
static const char WIFI_PASSWORD[] = "your-wifi-password";

// ===== Claude usage (server tren PC: pc_server/claude_usage_server.py) =====
#define CLAUDE_ENABLED 1                            // 0 = tat trang Claude
static const char CLAUDE_HOST[]  = "192.168.1.100"; // IP LAN cua may tinh chay server
static const uint16_t CLAUDE_PORT = 7842;
static const char CLAUDE_TOKEN[] = "change-me";     // phai trung voi token cua server
