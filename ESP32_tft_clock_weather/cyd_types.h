// Kieu du lieu dung chung cho cac file .ino.
// Phai nam trong header: Arduino IDE tu sinh khai bao ham o dau sketch, truoc cac kieu khai bao trong .ino.
#pragma once
#include <stdint.h>

struct Tone { uint16_t freq, ms; };                       // mot not bip: tan so (0 = im), thoi gian
struct AqiLevel { const char* text; uint32_t bg; uint32_t fg; };
enum PomoPhase { POMO_FOCUS, POMO_BREAK, POMO_LONG };

// Cac trang. SCR_HOME la man chon widget, SCR_SETTINGS mo tu nut banh rang tren Home,
// SCR_DEVICE / SCR_SETUP mo tu Settings; con lai la "tab" (co o tren Home, vao duoc slideshow).
enum ScreenState {
  SCR_HOME = 0, SCR_CLOCK, SCR_WEATHER, SCR_FORECAST, SCR_CLAUDE, SCR_ACCOUNT,
  SCR_MUSIC, SCR_PC, SCR_POMO, SCR_SETTINGS, SCR_DEVICE, SCR_SETUP, SCREEN_COUNT
};

// HOME: dung yen o Home | FIXED: dung yen o 1 tab | SLIDESHOW: tu chuyen qua cac tab da chon
enum NavMode { MODE_HOME = 0, MODE_FIXED, MODE_SLIDESHOW };

// Cai dat tren may (tab Settings), luu NVS
enum NotifyMode { NOTIFY_ALWAYS = 0, NOTIFY_AWAY };
enum ScreenMode { SCREEN_ALWAYS = 0, SCREEN_WITH_PC };   // WITH_PC: tat den nen khi PC khoa / ngu / tat
struct Settings {
  bool beep = true;
  uint8_t notify = NOTIFY_AWAY;   // AWAY: chi bao khi ban roi may >= 1 phut hoac Claude chay >= 2 phut
  uint8_t slide_s = 20;           // giay moi tab trong slideshow
  bool alerts = true;             // canh bao han muc Claude + mua sap toi
  uint8_t screen = SCREEN_ALWAYS;
};

// Cau hinh mang, nhap qua trang Wi-Fi setup (luu NVS); mac dinh lay tu secrets.h neu co
struct NetConfig {
  char ssid[33] = "", pass[65] = "";
  char token[48] = "";            // phai trung token cua PC server
  char host[40] = "";             // IP PC server; de trong = tu tim qua UDP beacon
  uint16_t port = 7842;
  char lat[12] = "", lon[12] = "";
  char tz[40] = "";               // chuoi POSIX TZ, vd "ICT-7"
};

// So lieu suc khoe cho trang Device (task mang ghi, UI doc; so nguyen 32-bit nen doc/ghi la nguyen tu)
struct DevStats {
  volatile uint32_t wifi_drops = 0, server_ms = 0, server_fail = 0, weather_ms = 0, weather_fail = 0;
  volatile uint32_t frame_us_avg = 0, frame_us_max = 0, frames = 0;
};

// ---- Du lieu tu mang ----
struct HourFc { char hour[4]; float temp; int code; bool day; int rain; };
struct DayFc  { int wday; float tmin, tmax; int code; int rain; };

struct WeatherData {
  bool valid = false;
  float temperature = 0, feels = 0, wind = 0;
  int humidity = 0;
  int weather_code = -1;
  bool is_day = true;
  char last_update[6] = "--:--";
  int n_hours = 0; HourFc hours[6];
  int n_days = 0;  DayFc days[3];
};

struct AqiData { bool valid = false; int aqi = 0; float pm25 = 0; };

struct PcData {
  bool valid = false;
  int cpu = -1, cpu_temp = -1, ram_pct = 0;
  float ram_used = 0, ram_total = 0;
  bool gpu = false;
  int gpu_temp = 0, gpu_util = 0;
  float vram_used = 0, vram_total = 0;
};

struct MediaData {
  bool valid = false;        // da hoi tab Music it nhat 1 lan
  bool has = false;          // co app dang phat / tam dung
  char error[32] = "";
  char app[20] = "", title[200] = "", artist[160] = "", art_id[12] = "";
  bool playing = false;
  int pos_s = 0, dur_s = 0;
  uint32_t rx_ms = 0;        // luc nhan -> noi suy vi tri moi giay
};

struct AccountData {
  bool valid = false;
  int n = 0;
  char name[4][16];
  int pct[4];                // ti le usage tuan theo noi dung (Claude Code / Chats / Cowork / Other)
};

// Claude Code xin quyen (PermissionRequest hook) - chi gui xuong khi ban vang mat
struct PermissionReq {
  int id = 0;                // 0 = khong co yeu cau
  int expires_s = 0;
  char tool[24] = "", project[41] = "", detail[161] = "";
};

// Su kien lich sap toi (tu file ICS ma PC server doc)
struct CalendarNext {
  bool has = false;
  char when[10] = "";        // "NOW" / "NEXT" / "TOMORROW"
  char start[6] = "";        // "14:00"
  char title[72] = "";
};

struct ClaudeData {
  bool valid = false;        // da tung nhan du lieu
  bool stale = false;        // lan poll gan nhat that bai
  bool exact = false;        // so lieu chinh xac tu Anthropic (khong phai uoc tinh)
  int session_pct = 0, minutes_remaining = 0;
  char resets_at[8] = "";
  int week_pct = 0;
  char week_resets_in[16] = "";
  long today_tokens = 0;
  float today_cost = 0;
  char plan[16] = "";
  long ctx_tokens = -1, ctx_limit = 0;  // ngu canh phien Claude Code gan nhat (-1 = khong co)
  int event_id = 0, event_age = 0, event_idle = 0, event_duration = -1;
  char event_type[16] = "", event_project[41] = "", event_msg[121] = "";
  PcData pc;
  MediaData media;
  AccountData account;
  PermissionReq perm;
  CalendarNext cal;
  bool pc_locked = false;    // Windows dang khoa man hinh
};
