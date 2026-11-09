// ============================
// ESP32 CYD (ESP32-2432S028R, 2.8" ILI9341 + XPT2046) - Ngu Hanh Son, Da Nang
// Phong cach "Claude editorial": nen giay nga, muc nau, mau dat nung, so serif Fraunces.
//
// HOME: luoi 9 o. Tap o -> mo tab do (dung yen). Tap trong tab -> ve HOME.
//       O SLIDESHOW -> tu chuyen qua cac tab da chon. Giu 1 o (hoac giu trong tab) -> them/bot khoi slideshow.
// Tab: Clock (+ am lich), Weather (+ AQI), Forecast, Claude Code, Claude account, Music, System, Pomodoro.
// Claude Code xong viec / dang cho -> the thong bao + bip.
// Den nen luon sang toi da. Mau dao xu ly o User_Setup.h (TFT_INVERSION_ON). WiFi / server trong secrets.h
// ============================

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "secrets.h"
#include "weather_images.h"
#include "lunar.h"
#include "cyd_types.h"
#include "src/fonts/mont_fonts.h"

// =========================
// CONFIG
// =========================
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))

#define TFT_BL_PIN 21      // den nen: HIGH = sang 100% (khong PWM, khong tu giam)

// Cam ung XPT2046 nam tren bus SPI rieng cua CYD.
// TFT_eSPI dang dung VSPI (khong co USE_HSPI_PORT trong User_Setup.h) nen touch dung HSPI.
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define SPEAKER_PIN  26    // cong loa cua CYD (khong gan loa thi khong anh huong gi)
#define BEEP_ENABLED 1

#define SLIDESHOW_INTERVAL       20000UL   // tu chuyen tab trong che do slideshow
#define DISPLAY_UPDATE_INTERVAL   1000UL
#define WEATHER_UPDATE_INTERVAL 600000UL
#define AQI_UPDATE_INTERVAL    1800000UL
#define NET_RETRY_INTERVAL       60000UL   // loi thi thu lai sau 60s (khong spam moi vong loop)
#define CLAUDE_POLL_ACTIVE        5000UL   // dang xem tab Claude / Account
#define PC_POLL_ACTIVE            3000UL   // dang xem tab System
#define MEDIA_POLL_ACTIVE         2000UL   // dang xem tab Music
#define CLAUDE_POLL_IDLE         60000UL
#define CLAUDE_BACKOFF_START      2000UL
#define CLAUDE_BACKOFF_MAX       60000UL
#define WIFI_RETRY_INTERVAL      15000UL
#define HTTP_TIMEOUT_MS           5000
#define LONG_PRESS_MS              800     // giu de them / bot tab khoi slideshow
#define FLASH_MS                  1500UL   // nhan thong bao ngan giua man, roi tu an
#define TOAST_MS                 25000UL   // the thong bao tu an sau 25s (hoac tap de an)
#define BEACON_PORT               7843     // PC server phat UDP: tim IP + bao su kien Claude

#define POMO_STEP_MIN 5                    // nut - / + doi 5 phut moi lan

const char* NTP_SERVER = "pool.ntp.org";
const char* TIMEZONE = "ICT-7";

// === Toa do Ngu Hanh Son, Da Nang ===
const char* LATITUDE = "16.0044";
const char* LONGITUDE = "108.2632";

#define TEMP_CELSIUS 1
#if TEMP_CELSIUS
  const char DEGREE_SYMBOL[] = "°C";
  const char TEMP_UNIT_PARAM[] = "celsius";
#else
  const char DEGREE_SYMBOL[] = "°F";
  const char TEMP_UNIT_PARAM[] = "fahrenheit";
#endif

// Phong cach "Claude editorial": nen giay nga, muc nau, nhan mau dat nung; so lon dung serif Fraunces
#define C_BG      0xF5F0E8   // giay nga
#define C_TILE    0xEBE3D6   // o tren Home / nut phu
#define C_TILE_P  0xDDD2C1   // o dang bi bam
#define C_FG      0x2B2722   // muc nau dam
#define C_MUTED   0x8C8173   // chu phu
#define C_ACCENT  0xC15F3C   // dat nung
#define C_BAR_BG  0xE6DCCD   // ranh bar / duong ke
#define C_OK      0x2E9E4F   // xanh la: dang ket noi / so lieu chinh xac
#define C_WARN    0xD9A21B   // vang: dang uoc tinh
#define C_ALERT   0xB3261E   // do: >= 85%, offline, qua nong
#define C_BLUE    0x4A6FA5   // xanh xam diu (do am, kha nang mua)
#define C_TEMP    C_ACCENT

static const char* const WEEKDAY_EN[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Thong tin hien thi cho tung tab (ten tren Home)
static const char* const SCREEN_NAMES[SCREEN_COUNT] = {
  "HOME", "CLOCK", "WEATHER", "FORECAST", "CLAUDE CODE", "ACCOUNT", "MUSIC", "SYSTEM", "POMODORO"
};
#define DEFAULT_SLIDES ((1 << SCR_CLOCK) | (1 << SCR_WEATHER) | (1 << SCR_FORECAST) | (1 << SCR_CLAUDE))

// =========================
// SHARED DATA (network task <-> UI loop), bao ve bang mutex
// =========================
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
  char app[20] = "", title[200] = "", artist[160] = "";
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
  int event_id = 0, event_age = 0;
  char event_type[16] = "", event_project[41] = "", event_msg[121] = "";
  PcData pc;
  MediaData media;
  AccountData account;
};

static SemaphoreHandle_t data_mutex;
static WeatherData shared_weather;
static AqiData shared_aqi;
static ClaudeData shared_claude;
static volatile bool weather_dirty = false, aqi_dirty = false, claude_dirty = false;
static volatile bool claude_poll_now = false;
static volatile int media_action = 0;   // 1 = play/pause, 2 = next, 3 = prev (UI dat, task mang gui)
static volatile ScreenState current_screen = SCR_HOME;

// =========================
// UI GLOBALS (chi dung trong loop task)
// =========================
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

SPIClass touch_spi(HSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
Preferences prefs;

static NavMode nav_mode = MODE_SLIDESHOW;
static uint16_t slides_mask = DEFAULT_SLIDES;
static unsigned long last_screen_switch = 0;
static unsigned long last_display_update = 0;

static WeatherData ui_weather;          // ban sao cho UI
static AqiData ui_aqi;
static MediaData ui_media;
static int ui_cpu_temp = -1;

static lv_obj_t* screens[SCREEN_COUNT];
static lv_obj_t* flash_badge;
static unsigned long flash_hide_at = 0;
// Home
static lv_obj_t *home_tile[SCREEN_COUNT + 1], *home_val[SCREEN_COUNT + 1], *home_dot[SCREEN_COUNT + 1];
static lv_obj_t* home_clock;
// Clock
static lv_obj_t *lb_clock_date, *lb_clock_time, *lb_clock_lunar, *lb_clock_weather;
// Weather
static lv_obj_t *img_weather, *lb_w_temp, *lb_w_feels, *lb_w_hum, *lb_w_wind, *lb_w_aqi, *lb_w_desc, *lb_w_info;
// Forecast
static lv_obj_t *lb_fh_hour[6], *img_fh[6], *lb_fh_temp[6], *lb_fh_rain[6];
static lv_obj_t *lb_fd_name[3], *img_fd[3], *lb_fd_rain[3], *lb_fd_temp[3];
// Claude Code
static lv_obj_t *cl_status_dot, *cl_status, *cl_session_pct, *cl_session_reset, *cl_session_left;
static lv_obj_t *cl_session_bar, *cl_week_pct, *cl_week_bar, *cl_week_meta, *cl_today, *cl_ctx_val, *cl_ctx_bar;
// Claude account
static lv_obj_t *ac_status_dot, *ac_status, *ac_sess_pct, *ac_sess_meta, *ac_sess_bar, *ac_week_pct, *ac_week_meta, *ac_week_bar;
static lv_obj_t *ac_name[4], *ac_bar[4], *ac_val[4];
// Music
static lv_obj_t *mu_app, *mu_title, *mu_artist, *mu_bar, *mu_pos, *mu_dur, *mu_play_icon;
// System
static lv_obj_t *pc_val[4], *pc_bar[4];
// Pomodoro
static lv_obj_t *po_phase, *po_time, *po_bar, *po_count, *po_btn_label, *po_len;
// Toast
static lv_obj_t *toast, *toast_title, *toast_sub;
static unsigned long toast_hide_at = 0;

// =========================
// UTIL
// =========================
void log_print(lv_log_level_t level, const char* buf) {
  LV_UNUSED(level);
  Serial.println(buf);
}

static uint32_t lv_tick_millis() { return millis(); }

static bool claude_enabled() { return CLAUDE_ENABLED && CLAUDE_HOST[0] != '\0'; }

// Lay gio khong chan (getLocalTime mac dinh cho toi 5s neu chua dong bo NTP)
static bool get_time_now(struct tm* t) { return getLocalTime(t, 0); }

static void no_click(lv_obj_t* obj) { lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE); }

static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

// Thu trong tuan tu ngay duong lich (0 = Chu nhat)
static int weekday_of(int y, int m, int d) { return (int)((lunar_jd(d, m, y) + 1) % 7); }

static void fmt_mmss(int s, char* out, size_t n) { snprintf(out, n, "%d:%02d", max(s, 0) / 60, max(s, 0) % 60); }

// =========================
// BEEP (khong chan) qua cong loa GPIO26
// =========================
static const Tone BEEP_DONE[] = {{1568, 90}, {0, 60}, {2093, 140}, {0, 0}};
static const Tone BEEP_WAIT[] = {{1319, 120}, {0, 80}, {1319, 120}, {0, 80}, {1760, 200}, {0, 0}};
static const Tone BEEP_POMO[] = {{2093, 160}, {0, 110}, {2093, 160}, {0, 110}, {2637, 320}, {0, 0}};
static const Tone* beep_seq = nullptr;
static int beep_idx = 0;
static unsigned long beep_next = 0;

static void beep(const Tone* seq) {
#if BEEP_ENABLED
  beep_seq = seq;
  beep_idx = 0;
  beep_next = 0;
#endif
}

static void beep_tick() {
  if (!beep_seq || millis() < beep_next) return;
  const Tone& t = beep_seq[beep_idx++];
  if (t.ms == 0) { ledcWriteTone(SPEAKER_PIN, 0); beep_seq = nullptr; return; }
  ledcWriteTone(SPEAKER_PIN, t.freq);
  beep_next = millis() + t.ms;
}

// =========================
// NETWORK (chay tren core 0, khong bao gio goi LVGL o day)
// =========================
static bool http_get_json(const String& url, JsonDocument& doc, const char* bearer = nullptr) {
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return false;
  if (bearer) http.addHeader("Authorization", String("Bearer ") + bearer);
  int code = http.GET();
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    DeserializationError err = deserializeJson(doc, http.getString());
    ok = !err;
    if (err) Serial.printf("JSON error %s: %s\n", err.c_str(), url.c_str());
  } else {
    Serial.printf("HTTP %d: %s\n", code, url.substring(0, 60).c_str());
  }
  http.end();
  return ok;
}

static bool http_post_json(const String& url, const String& body, const char* bearer) {
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + bearer);
  int code = http.POST(body);
  http.end();
  return code == HTTP_CODE_OK;
}

// Thoi tiet hien tai + 6 gio toi + 3 ngay (mot request Open-Meteo)
static bool fetch_weather() {
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + LATITUDE + "&longitude=" + LONGITUDE +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day,wind_speed_10m" +
               "&hourly=temperature_2m,precipitation_probability,weather_code,is_day&forecast_hours=8" +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&forecast_days=3" +
               "&temperature_unit=" + TEMP_UNIT_PARAM + "&timezone=auto";
  JsonDocument doc;
  if (!http_get_json(url, doc)) return false;
  JsonObjectConst cur = doc["current"];
  if (cur.isNull()) return false;

  WeatherData w;
  w.valid = true;
  w.temperature = cur["temperature_2m"] | 0.0f;
  w.feels = cur["apparent_temperature"] | 0.0f;
  w.wind = cur["wind_speed_10m"] | 0.0f;
  w.humidity = cur["relative_humidity_2m"] | 0;   // do am HIEN TAI
  w.weather_code = cur["weather_code"] | -1;
  w.is_day = (cur["is_day"] | 1) != 0;
  const char* now_time = cur["time"] | "";          // "2026-11-09T19:45" (gio dia phuong)
  const char* tp = strchr(now_time, 'T');
  if (tp) strlcpy(w.last_update, tp + 1, sizeof(w.last_update));

  JsonObjectConst h = doc["hourly"];
  JsonArrayConst ht = h["time"];
  for (size_t i = 0; i < ht.size() && w.n_hours < 6; i++) {
    const char* t = ht[i] | "";
    if (strcmp(t, now_time) <= 0) continue;          // chi lay cac gio sau hien tai
    HourFc& f = w.hours[w.n_hours++];
    snprintf(f.hour, sizeof(f.hour), "%.2sh", strlen(t) > 12 ? t + 11 : "--");
    f.temp = h["temperature_2m"][i] | 0.0f;
    f.code = h["weather_code"][i] | -1;
    f.day = (h["is_day"][i] | 1) != 0;
    f.rain = h["precipitation_probability"][i] | 0;
  }

  JsonObjectConst d = doc["daily"];
  JsonArrayConst dt = d["time"];
  for (size_t i = 0; i < dt.size() && w.n_days < 3; i++) {
    int y = 0, m = 0, dd = 0;
    sscanf(dt[i] | "", "%d-%d-%d", &y, &m, &dd);
    DayFc& f = w.days[w.n_days++];
    f.wday = weekday_of(y, m, dd);
    f.tmin = d["temperature_2m_min"][i] | 0.0f;
    f.tmax = d["temperature_2m_max"][i] | 0.0f;
    f.code = d["weather_code"][i] | -1;
    f.rain = d["precipitation_probability_max"][i] | 0;
  }

  xSemaphoreTake(data_mutex, portMAX_DELAY);
  shared_weather = w;
  weather_dirty = true;
  xSemaphoreGive(data_mutex);
  Serial.println("Weather updated");
  return true;
}

static bool fetch_aqi() {
  String url = String("http://air-quality-api.open-meteo.com/v1/air-quality?latitude=") + LATITUDE +
               "&longitude=" + LONGITUDE + "&current=us_aqi,pm2_5&timezone=auto";
  JsonDocument doc;
  if (!http_get_json(url, doc)) return false;
  JsonObjectConst cur = doc["current"];
  if (cur.isNull() || cur["us_aqi"].isNull()) return false;
  AqiData a;
  a.valid = true;
  a.aqi = cur["us_aqi"] | 0;
  a.pm25 = cur["pm2_5"] | 0.0f;
  xSemaphoreTake(data_mutex, portMAX_DELAY);
  shared_aqi = a;
  aqi_dirty = true;
  xSemaphoreGive(data_mutex);
  Serial.printf("AQI updated: %d\n", a.aqi);
  return true;
}

// Dia chi server hien tai: mac dinh tu secrets.h, cap nhat khi nghe thay beacon (chi task mang dung)
static char claude_host[16];
static uint16_t claude_port = CLAUDE_PORT;
static WiFiUDP beacon_udp;
static bool beacon_listening = false;

static String server_url(const char* path) { return String("http://") + claude_host + ":" + claude_port + path; }

// UDP tu PC: "CLAUDE_USAGE 1 <port>" (tim IP) hoac "CLAUDE_EVENT <id>" (Claude Code vua xong / dang cho)
// Tra ve true neu can lay so lieu ngay
static bool check_beacon() {
  bool poll = false;
  int len;
  while ((len = beacon_udp.parsePacket()) > 0) {
    char msg[48];
    len = beacon_udp.read(msg, sizeof(msg) - 1);
    if (len <= 0) continue;
    msg[len] = '\0';
    int port = 0, ev = 0;
    String ip = beacon_udp.remoteIP().toString();
    if (sscanf(msg, "CLAUDE_USAGE 1 %d", &port) == 1 && port > 0 && port <= 65535) {
      if (ip != claude_host || port != claude_port) {
        strlcpy(claude_host, ip.c_str(), sizeof(claude_host));
        claude_port = port;
        Serial.printf("Claude server found at %s:%d\n", claude_host, claude_port);
        poll = true;
      }
    } else if (sscanf(msg, "CLAUDE_EVENT %d", &ev) == 1 && ip == claude_host) {
      poll = true;  // su kien moi -> lay ngay, khong doi lan poll ke tiep
    }
  }
  return poll;
}

// Giao thuc claudometer: GET /v1/stats, Bearer token, schema 1 (+ event, context, account, pc, media)
static bool fetch_claude(ScreenState scr) {
  String screens = "home";
  if (scr == SCR_PC) screens += ",pc";
  if (scr == SCR_MUSIC) screens += ",media";
  JsonDocument doc;
  bool ok = http_get_json(server_url("/v1/stats?screens=") + screens, doc, CLAUDE_TOKEN) && (doc["schema"] | 0) == 1;
  if (!ok) {
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    if (!shared_claude.stale) { shared_claude.stale = true; claude_dirty = true; }
    xSemaphoreGive(data_mutex);
    return false;
  }

  ClaudeData c;
  c.valid = true;
  c.exact = strcmp(doc["source"] | "", "exact") == 0;
  JsonObjectConst s = doc["session"];
  c.session_pct = s["pct_used"] | 0;
  c.minutes_remaining = s["minutes_remaining"] | 0;
  strlcpy(c.resets_at, s["resets_at"] | "", sizeof(c.resets_at));
  JsonObjectConst b = doc["budgets"];
  c.week_pct = b["code_all"] | 0;
  strlcpy(c.week_resets_in, b["resets_in"] | "", sizeof(c.week_resets_in));
  strlcpy(c.plan, b["plan"] | "", sizeof(c.plan));
  JsonObjectConst m = doc["models_today"];
  c.today_tokens = m["total_tokens"] | 0L;
  c.today_cost = m["est_cost_usd"] | 0.0f;

  JsonObjectConst ctx = doc["context"];
  if (!ctx.isNull()) {
    c.ctx_tokens = ctx["tokens"] | 0L;
    c.ctx_limit = ctx["limit"] | 0L;
  }

  JsonArrayConst rows = doc["account"]["breakdown"];
  if (!rows.isNull()) {
    c.account.valid = true;
    for (JsonObjectConst r : rows) {
      if (c.account.n >= 4) break;
      strlcpy(c.account.name[c.account.n], r["name"] | "", sizeof(c.account.name[0]));
      c.account.pct[c.account.n] = r["percent"] | 0;
      c.account.n++;
    }
  }

  JsonObjectConst ev = doc["event"];
  if (!ev.isNull()) {
    c.event_id = ev["id"] | 0;
    c.event_age = ev["age_s"] | 0;
    strlcpy(c.event_type, ev["type"] | "", sizeof(c.event_type));
    strlcpy(c.event_project, ev["project"] | "", sizeof(c.event_project));
    strlcpy(c.event_msg, ev["message"] | "", sizeof(c.event_msg));
  }

  JsonObjectConst pc = doc["pc"];
  if (!pc.isNull()) {
    c.pc.valid = true;
    c.pc.cpu = pc["cpu_pct"].isNull() ? -1 : (int)(pc["cpu_pct"] | 0);
    c.pc.cpu_temp = pc["cpu_temp_c"].isNull() ? -1 : (int)(pc["cpu_temp_c"] | 0);
    c.pc.ram_pct = pc["ram"]["pct"] | 0;
    c.pc.ram_used = pc["ram"]["used_gb"] | 0.0f;
    c.pc.ram_total = pc["ram"]["total_gb"] | 0.0f;
    JsonObjectConst g = pc["gpu"];
    if (!g.isNull()) {
      c.pc.gpu = true;
      c.pc.gpu_temp = g["temp_c"] | 0;
      c.pc.gpu_util = g["util_pct"] | 0;
      c.pc.vram_used = g["mem_used_gb"] | 0.0f;
      c.pc.vram_total = g["mem_total_gb"] | 0.0f;
    }
  }

  bool media_asked = scr == SCR_MUSIC;
  JsonVariantConst md = doc["media"];
  if (media_asked) {
    c.media.valid = true;
    c.media.rx_ms = millis();
    if (md.is<JsonObjectConst>()) {
      strlcpy(c.media.error, md["error"] | "", sizeof(c.media.error));
      c.media.has = !c.media.error[0];
      strlcpy(c.media.app, md["app"] | "", sizeof(c.media.app));
      strlcpy(c.media.title, md["title"] | "", sizeof(c.media.title));
      strlcpy(c.media.artist, md["artist"] | "", sizeof(c.media.artist));
      c.media.playing = md["playing"] | false;
      c.media.pos_s = md["pos_s"] | 0;
      c.media.dur_s = md["dur_s"] | 0;
    }
  }

  xSemaphoreTake(data_mutex, portMAX_DELAY);
  if (!c.pc.valid) c.pc = shared_claude.pc;          // tab khac khong hoi PC -> giu so cu
  if (!media_asked) c.media = shared_claude.media;   // tuong tu cho nhac
  if (!c.account.valid) c.account = shared_claude.account;
  shared_claude = c;
  claude_dirty = true;
  xSemaphoreGive(data_mutex);
  return true;
}

void network_task(void*) {
  uint32_t next_wifi_retry = millis() + WIFI_RETRY_INTERVAL;
  uint32_t next_weather = 0, next_aqi = 0, next_claude = 0;
  uint32_t claude_backoff = CLAUDE_BACKOFF_START;
  strlcpy(claude_host, CLAUDE_HOST, sizeof(claude_host));

  for (;;) {
    uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (beacon_listening) { beacon_udp.stop(); beacon_listening = false; }
      if (due(now, next_wifi_retry)) {
        Serial.println("WiFi disconnected, reconnecting...");
        WiFi.reconnect();
        next_wifi_retry = now + WIFI_RETRY_INTERVAL;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    next_wifi_retry = now + WIFI_RETRY_INTERVAL;

    // NTP chi bat sau khi WiFi len: goi configTzTime qua som lam lwIP assert (ESP32 core 3.x)
    static bool ntp_started = false;
    if (!ntp_started) {
      configTzTime(TIMEZONE, NTP_SERVER);
      ntp_started = true;
    }

    if (claude_enabled()) {
      if (!beacon_listening) beacon_listening = beacon_udp.begin(BEACON_PORT);
      if (beacon_listening && check_beacon()) {
        claude_backoff = CLAUDE_BACKOFF_START;
        next_claude = now;
      }
      int act = media_action;
      if (act) {  // nut Music: gui lenh roi lay trang thai moi
        media_action = 0;
        const char* name = act == 1 ? "toggle" : act == 2 ? "next" : "prev";
        http_post_json(server_url("/v1/media"), String("{\"action\":\"") + name + "\"}", CLAUDE_TOKEN);
        vTaskDelay(pdMS_TO_TICKS(300));  // de app nhac kip doi bai / trang thai
        next_claude = millis();
      }
      if (claude_poll_now) { claude_poll_now = false; next_claude = now; }
      if (due(millis(), next_claude)) {
        ScreenState scr = current_screen;
        if (fetch_claude(scr)) {
          claude_backoff = CLAUDE_BACKOFF_START;
          uint32_t iv = scr == SCR_MUSIC ? MEDIA_POLL_ACTIVE : scr == SCR_PC ? PC_POLL_ACTIVE
                      : (scr == SCR_CLAUDE || scr == SCR_ACCOUNT) ? CLAUDE_POLL_ACTIVE : CLAUDE_POLL_IDLE;
          next_claude = millis() + iv;
        } else {
          next_claude = millis() + claude_backoff;
          claude_backoff = min<uint32_t>(claude_backoff * 2, CLAUDE_BACKOFF_MAX);
        }
      }
    }

    if (due(now, next_weather)) {
      next_weather = millis() + (fetch_weather() ? WEATHER_UPDATE_INTERVAL : NET_RETRY_INTERVAL);
    }
    if (due(now, next_aqi)) {
      next_aqi = millis() + (fetch_aqi() ? AQI_UPDATE_INTERVAL : NET_RETRY_INTERVAL);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// =========================
// TOUCH -> LVGL input device
// =========================
// Hieu chinh do tren chinh tam cam ung nay (cham 4 goc, 21 lan): gia tri tho XPT2046 o
// setRotation(2) tai 4 canh cua man ngang 320x240. Ngang (trai->phai) = raw y giam dan,
// doc (tren->duoi) = raw x tang dan.
#define TOUCH_RAW_LEFT   3712
#define TOUCH_RAW_RIGHT   499
#define TOUCH_RAW_TOP     514
#define TOUCH_RAW_BOTTOM 3428
static void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
  LV_UNUSED(indev);
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    // Toa do man ngang (0..319, 0..239)
    int lx = constrain(map(p.y, TOUCH_RAW_LEFT, TOUCH_RAW_RIGHT, 0, SCREEN_HEIGHT - 1), 0, SCREEN_HEIGHT - 1);
    int ly = constrain(map(p.x, TOUCH_RAW_TOP, TOUCH_RAW_BOTTOM, 0, SCREEN_WIDTH - 1), 0, SCREEN_WIDTH - 1);
    // LVGL 9 tu xoay diem cham theo ROTATION_90 (x' = 319 - y, y' = x), nen tra ve toa do doc goc
    data->point.x = ly;
    data->point.y = SCREEN_HEIGHT - 1 - lx;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// =========================
// TEXT HELPERS
// =========================
static const char* weather_desc(int code) {
  switch (code) {
    case 0: return "Clear sky";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, hail";
    case -1: return "Loading...";
    default: return "Unknown";
  }
}

static const lv_image_dsc_t* weather_image(int code, bool is_day) {
  switch (code) {
    case 0: case 1: return is_day ? &image_weather_sun : &image_weather_night;
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 65: case 66: case 67:
    case 80: case 81: case 82: return &image_weather_rain;
    case 71: case 73: case 75: case 77: case 85: case 86: return &image_weather_snow;
    case 95: case 96: case 99: return &image_weather_thunder;
    default: return &image_weather_cloud;  // 2, 3, suong mu, chua co du lieu, ma la
  }
}

static AqiLevel aqi_level(int aqi) {
  if (aqi <= 50)  return {"Good", 0x16A34A, 0xFFFFFF};
  if (aqi <= 100) return {"Moderate", 0xFACC15, 0x1F1E1B};
  if (aqi <= 150) return {"Unhealthy (SG)", 0xF97316, 0xFFFFFF};
  if (aqi <= 200) return {"Unhealthy", 0xDC2626, 0xFFFFFF};
  if (aqi <= 300) return {"Very unhealthy", 0x7E22CE, 0xFFFFFF};
  return {"Hazardous", 0x7F1D1D, 0xFFFFFF};
}

// Dong am lich giu tieng Viet: "Mùng 1 tháng 10 âm lịch · Bính Ngọ"
static void format_lunar(const struct tm& t, char* out, size_t n) {
  LunarDate l = solar_to_lunar(t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  char yname[24], dname[16];
  lunar_year_name(l.year, yname, sizeof(yname));
  if (l.day == 15) snprintf(dname, sizeof(dname), "Rằm");
  else if (l.day <= 10) snprintf(dname, sizeof(dname), "Mùng %d", l.day);
  else snprintf(dname, sizeof(dname), "%d", l.day);
  snprintf(out, n, "%s tháng %d%s âm lịch · %s", dname, l.month, l.leap ? " nhuận" : "", yname);
}

// =========================
// POMODORO STATE (do dai moi pha luu trong NVS)
// =========================
static PomoPhase pomo_phase = POMO_FOCUS;
static bool pomo_running = false;
static int pomo_min[3] = {25, 5, 15};               // focus / nghi ngan / nghi dai (phut)
static const int POMO_MAX[3] = {90, 30, 60};
static unsigned long pomo_end_ms = 0, pomo_remaining_ms = 25 * 60000UL;
static int pomo_done = 0;

static unsigned long pomo_duration(PomoPhase p) { return pomo_min[p] * 60000UL; }

// =========================
// NAVIGATION: Home / tab co dinh / slideshow
// =========================
static bool screen_enabled(ScreenState s) {
  if ((s == SCR_CLAUDE || s == SCR_ACCOUNT || s == SCR_PC || s == SCR_MUSIC) && !claude_enabled()) return false;
  return s != SCR_HOME;
}

static bool in_slideshow(ScreenState s) { return screen_enabled(s) && (slides_mask & (1 << s)); }

static ScreenState next_slide(ScreenState s) {
  for (int i = 0; i < SCREEN_COUNT; i++) {
    s = (ScreenState)((s + 1) % SCREEN_COUNT);
    if (in_slideshow(s)) return s;
  }
  return SCR_CLOCK;
}

static void save_nav() {
  prefs.putUChar("mode", nav_mode);
  prefs.putUChar("screen", current_screen);
}

static void show_screen(ScreenState s) {
  current_screen = s;
  lv_screen_load(screens[s]);   // doi trang tuc thi (khong hieu ung -> khong giat)
  last_screen_switch = millis();
  if (s == SCR_CLAUDE || s == SCR_ACCOUNT || s == SCR_PC || s == SCR_MUSIC) claude_poll_now = true;
}

static void go_home() {
  nav_mode = MODE_HOME;
  show_screen(SCR_HOME);
  save_nav();
}

static void open_tab(ScreenState s) {
  nav_mode = MODE_FIXED;
  show_screen(s);
  save_nav();
}

static void start_slideshow() {
  if (!slides_mask) slides_mask = DEFAULT_SLIDES;
  nav_mode = MODE_SLIDESHOW;
  show_screen(next_slide(current_screen == SCR_HOME ? (ScreenState)(SCREEN_COUNT - 1) : current_screen));
  save_nav();
}

// Nhan thong bao ngan giua man, roi tu an
static void flash(const char* text) {
  lv_label_set_text(flash_badge, text);
  lv_obj_remove_flag(flash_badge, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(flash_badge);
  flash_hide_at = millis() + FLASH_MS;
}

static void update_home_dots() {
  for (int s = SCR_CLOCK; s < SCREEN_COUNT; s++) {
    if (!home_dot[s]) continue;
    if (slides_mask & (1 << s)) lv_obj_remove_flag(home_dot[s], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(home_dot[s], LV_OBJ_FLAG_HIDDEN);
  }
}

static void toggle_slide(ScreenState s) {
  slides_mask ^= (1 << s);
  prefs.putUShort("slides", slides_mask);
  update_home_dots();
  char buf[48];
  snprintf(buf, sizeof(buf), "%s %s", SCREEN_NAMES[s], (slides_mask & (1 << s)) ? "ADDED TO SLIDESHOW" : "REMOVED");
  flash(buf);
}

// Tap trong 1 tab -> ve Home. Giu -> them / bot tab nay khoi slideshow.
static void screen_touch_cb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (current_screen == SCR_HOME) return;
  if (code == LV_EVENT_SHORT_CLICKED) go_home();
  else if (code == LV_EVENT_LONG_PRESSED) toggle_slide(current_screen);
}

// O tren Home: tap -> mo tab (dung yen) / slideshow. Giu -> them / bot khoi slideshow.
static void tile_cb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (idx == SCREEN_COUNT) {               // o SLIDESHOW
    if (code == LV_EVENT_SHORT_CLICKED) start_slideshow();
    return;
  }
  if (code == LV_EVENT_SHORT_CLICKED) open_tab((ScreenState)idx);
  else if (code == LV_EVENT_LONG_PRESSED) toggle_slide((ScreenState)idx);
}

// =========================
// WIDGET HELPERS
// =========================
static lv_obj_t* make_screen() {
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_LONG_PRESSED, NULL);
  return scr;
}

static lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, text);
  no_click(l);
  return l;
}

// Nhan co do rong co dinh, can giua (cot du bao)
static lv_obj_t* make_cell(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int x, int y, int w) {
  lv_obj_t* l = make_label(parent, font, color, "");
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(l, x, y);
  return l;
}

static lv_obj_t* make_icon(lv_obj_t* parent, int x, int y, int size) {
  lv_obj_t* img = lv_image_create(parent);
  lv_image_set_src(img, &image_weather_cloud);
  lv_obj_set_size(img, size, size);
  lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);  // thu nho icon 128px vua khung
  lv_obj_set_pos(img, x, y);
  no_click(img);
  return img;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* bar = lv_bar_create(parent);
  lv_obj_set_size(bar, w, h);
  lv_obj_set_pos(bar, x, y);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  no_click(bar);
  return bar;
}

static void make_divider(lv_obj_t* parent, int y) {
  lv_obj_t* d = lv_obj_create(parent);
  lv_obj_remove_style_all(d);
  lv_obj_set_size(d, 292, 1);
  lv_obj_set_pos(d, 14, y);
  lv_obj_set_style_bg_color(d, lv_color_hex(C_BAR_BG), 0);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  no_click(d);
}

static void set_pct_bar(lv_obj_t* bar, int pct) {
  pct = constrain(pct, 0, 100);
  lv_bar_set_value(bar, pct, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, lv_color_hex(pct >= 85 ? C_ALERT : C_ACCENT), LV_PART_INDICATOR);
}

// Nhan chu in hoa kieu tap chi: nho, gian chu
static lv_obj_t* make_caps(lv_obj_t* parent, uint32_t color, const char* text) {
  lv_obj_t* l = make_label(parent, &lv_font_montserrat_12, color, text);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  return l;
}

static void title(lv_obj_t* scr, const char* text) {
  lv_obj_t* t = make_caps(scr, C_ACCENT, text);
  lv_obj_set_pos(t, 14, 10);
}

static lv_obj_t* make_dot(lv_obj_t* parent, int size, uint32_t color) {
  lv_obj_t* d = lv_obj_create(parent);
  lv_obj_remove_style_all(d);
  lv_obj_set_size(d, size, size);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
  no_click(d);
  return d;
}

// Nut tron (Music, Pomodoro): nen + ky hieu LV_SYMBOL_*
static lv_obj_t* make_round_button(lv_obj_t* parent, int x, int y, int size, uint32_t bg, uint32_t fg,
                                   const char* symbol, const lv_font_t* font, lv_event_cb_t cb, lv_obj_t** label_out) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, size, size);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg == C_ACCENT ? 0xA34F31 : C_TILE_P), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = make_label(b, font, fg, symbol);
  lv_obj_center(l);
  if (label_out) *label_out = l;
  return b;
}

static lv_obj_t* make_button(lv_obj_t* parent, int x, int y, int w, uint32_t bg, uint32_t fg, const char* text,
                             lv_event_cb_t cb, lv_obj_t** label_out) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, w, 46);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg == C_ACCENT ? 0xA34F31 : C_TILE_P), LV_STATE_PRESSED);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = make_label(b, &lv_font_montserrat_16, fg, text);
  lv_obj_center(l);
  if (label_out) *label_out = l;
  return b;
}

// =========================
// CREATE SCREENS
// =========================
// HOME: luoi 3x3. Moi o: so xem nhanh (serif) + ten in hoa; cham cam = dang co trong slideshow.
void create_home_screen() {
  lv_obj_t* scr = screens[SCR_HOME] = make_screen();
  title(scr, "HOME");
  lv_obj_t* hint = make_caps(scr, C_MUTED, "HOLD TILE = SLIDESHOW");
  lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -60, 10);
  home_clock = make_label(scr, &lv_font_montserrat_12, C_FG, "--:--");
  lv_obj_align(home_clock, LV_ALIGN_TOP_RIGHT, -12, 9);

  const int order[9] = {SCR_CLOCK, SCR_WEATHER, SCR_FORECAST, SCR_CLAUDE, SCR_ACCOUNT, SCR_MUSIC, SCR_PC, SCR_POMO, SCREEN_COUNT};
  for (int i = 0; i < 9; i++) {
    int idx = order[i];
    bool slideshow = idx == SCREEN_COUNT;
    int x = 8 + (i % 3) * 103, y = 30 + (i / 3) * 70;
    lv_obj_t* t = lv_obj_create(scr);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 98, 64);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_radius(t, 10, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(slideshow ? C_ACCENT : C_TILE), 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(slideshow ? 0xA34F31 : C_TILE_P), LV_STATE_PRESSED);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(t, tile_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(t, tile_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)idx);
    home_tile[idx] = t;

    uint32_t fg = slideshow ? C_BG : C_FG;
    home_val[idx] = make_label(t, &fr_num_24, slideshow ? C_BG : C_ACCENT, "--");
    lv_obj_set_pos(home_val[idx], 9, 7);
    lv_obj_t* name = make_caps(t, slideshow ? C_BG : C_MUTED, slideshow ? "SLIDESHOW" : SCREEN_NAMES[idx]);
    lv_obj_set_pos(name, 9, 44);
    LV_UNUSED(fg);
    if (!slideshow) {
      home_dot[idx] = make_dot(t, 8, C_ACCENT);
      lv_obj_align(home_dot[idx], LV_ALIGN_TOP_RIGHT, -8, 8);
    }
  }
  // Cac o khong co so -> dung ky hieu
  lv_obj_set_style_text_font(home_val[SCR_MUSIC], &lv_font_montserrat_20, 0);
  lv_label_set_text(home_val[SCR_MUSIC], LV_SYMBOL_AUDIO);
  lv_obj_set_style_text_font(home_val[SCREEN_COUNT], &lv_font_montserrat_20, 0);
  lv_label_set_text(home_val[SCREEN_COUNT], LV_SYMBOL_PLAY);
  lv_label_set_text(home_val[SCR_POMO], "25:00");
  update_home_dots();
}

void create_clock_screen() {
  lv_obj_t* scr = screens[SCR_CLOCK] = make_screen();
  lb_clock_date = make_label(scr, &lv_font_montserrat_16, C_MUTED, "Syncing time...");
  lv_obj_align(lb_clock_date, LV_ALIGN_TOP_MID, 0, 24);
  lb_clock_time = make_label(scr, &fr_num_64, C_FG, "--:--:--");
  lv_obj_align(lb_clock_time, LV_ALIGN_CENTER, 0, -14);
  lb_clock_lunar = make_label(scr, &mont_vi_16, C_ACCENT, "");
  lv_obj_align(lb_clock_lunar, LV_ALIGN_CENTER, 0, 44);
  // duong ke mong ngan cach dong thoi tiet, kieu tap chi
  lv_obj_t* rule = lv_obj_create(scr);
  lv_obj_remove_style_all(rule);
  lv_obj_set_size(rule, 120, 1);
  lv_obj_set_style_bg_color(rule, lv_color_hex(C_BAR_BG), 0);
  lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
  lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, -46);
  no_click(rule);
  lb_clock_weather = make_label(scr, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_align(lb_clock_weather, LV_ALIGN_BOTTOM_MID, 0, -18);
}

void create_weather_screen() {
  lv_obj_t* scr = screens[SCR_WEATHER] = make_screen();
  img_weather = lv_image_create(scr);
  lv_image_set_src(img_weather, &image_weather_cloud);
  lv_obj_set_pos(img_weather, 10, 34);
  no_click(img_weather);

  lv_obj_t* loc = make_caps(scr, C_MUTED, "NGU HANH SON, DA NANG");
  lv_obj_align(loc, LV_ALIGN_TOP_RIGHT, -14, 12);
  lb_w_temp = make_label(scr, &fr_num_34, C_TEMP, "--");
  lv_obj_align(lb_w_temp, LV_ALIGN_TOP_RIGHT, -14, 32);
  lb_w_feels = make_label(scr, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_align(lb_w_feels, LV_ALIGN_TOP_RIGHT, -14, 80);
  lb_w_hum = make_label(scr, &lv_font_montserrat_16, C_BLUE, "Humidity --%");
  lv_obj_align(lb_w_hum, LV_ALIGN_TOP_RIGHT, -14, 104);
  lb_w_wind = make_label(scr, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_align(lb_w_wind, LV_ALIGN_TOP_RIGHT, -14, 128);

  lb_w_aqi = make_label(scr, &lv_font_montserrat_14, C_MUTED, "AQI --");
  lv_obj_set_style_bg_opa(lb_w_aqi, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(lb_w_aqi, lv_color_hex(C_BAR_BG), 0);
  lv_obj_set_style_radius(lb_w_aqi, 10, 0);
  lv_obj_set_style_pad_hor(lb_w_aqi, 10, 0);
  lv_obj_set_style_pad_ver(lb_w_aqi, 3, 0);
  lv_obj_align(lb_w_aqi, LV_ALIGN_TOP_RIGHT, -14, 152);

  lb_w_desc = make_label(scr, &lv_font_montserrat_20, C_FG, "Loading...");
  lv_obj_align(lb_w_desc, LV_ALIGN_BOTTOM_MID, 0, -30);
  lb_w_info = make_label(scr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_align(lb_w_info, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void create_forecast_screen() {
  lv_obj_t* scr = screens[SCR_FORECAST] = make_screen();
  title(scr, "FORECAST");
  lv_obj_t* sub = make_caps(scr, C_MUTED, "6 HOURS | 3 DAYS");
  lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, -12, 10);

  for (int i = 0; i < 6; i++) {
    int x = 4 + i * 52;
    lb_fh_hour[i] = make_cell(scr, &lv_font_montserrat_12, C_MUTED, x, 32, 52);
    img_fh[i] = make_icon(scr, x + 8, 48, 36);
    lb_fh_temp[i] = make_cell(scr, &lv_font_montserrat_16, C_FG, x, 86, 52);
    lb_fh_rain[i] = make_cell(scr, &lv_font_montserrat_12, C_BLUE, x, 106, 52);
  }
  make_divider(scr, 126);
  for (int i = 0; i < 3; i++) {
    int y = 132 + i * 36;
    lb_fd_name[i] = make_label(scr, &lv_font_montserrat_16, C_FG, "");
    lv_obj_set_pos(lb_fd_name[i], 12, y + 7);
    img_fd[i] = make_icon(scr, 122, y + 2, 30);
    lb_fd_rain[i] = make_label(scr, &lv_font_montserrat_12, C_BLUE, "");
    lv_obj_set_pos(lb_fd_rain[i], 160, y + 10);
    lb_fd_temp[i] = make_label(scr, &lv_font_montserrat_16, C_FG, "");
    lv_obj_align(lb_fd_temp[i], LV_ALIGN_TOP_RIGHT, -12, y + 7);
  }
}

// Goc phai tieu de: ten goi + cham trang thai (xanh = chinh xac, vang = uoc tinh, do = offline)
static void make_status(lv_obj_t* scr, lv_obj_t** label, lv_obj_t** dot) {
  *label = make_caps(scr, C_MUTED, "WAITING FOR PC...");
  lv_obj_align(*label, LV_ALIGN_TOP_RIGHT, -30, 10);
  *dot = make_dot(scr, 12, C_MUTED);
  lv_obj_align(*dot, LV_ALIGN_TOP_RIGHT, -12, 10);
}

// Tab Claude Code - bo cuc theo man Home cua claudometer, 320x240 ngang
void create_claude_screen() {
  lv_obj_t* scr = screens[SCR_CLAUDE] = make_screen();
  title(scr, "CLAUDE CODE");
  make_status(scr, &cl_status, &cl_status_dot);

  cl_session_pct = make_label(scr, &fr_num_56, C_FG, "--%");
  lv_obj_set_pos(cl_session_pct, 12, 22);
  lv_obj_t* l = make_caps(scr, C_MUTED, "SESSION");
  lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -14, 34);
  cl_session_reset = make_label(scr, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_align(cl_session_reset, LV_ALIGN_TOP_RIGHT, -14, 52);
  cl_session_left = make_label(scr, &lv_font_montserrat_14, C_FG, "");
  lv_obj_align(cl_session_left, LV_ALIGN_TOP_RIGHT, -14, 72);
  cl_session_bar = make_bar(scr, 14, 98, 292, 6);

  make_divider(scr, 114);

  l = make_caps(scr, C_MUTED, "WEEK");
  lv_obj_set_pos(l, 14, 126);
  cl_week_pct = make_label(scr, &fr_num_24, C_FG, "--%");
  lv_obj_align(cl_week_pct, LV_ALIGN_TOP_RIGHT, -14, 118);
  cl_week_bar = make_bar(scr, 14, 150, 292, 4);
  cl_week_meta = make_label(scr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(cl_week_meta, 14, 158);

  make_divider(scr, 180);

  // Hai dong cuoi: token tieu thu hom nay, va do day ngu canh cua phien dang chay
  l = make_caps(scr, C_MUTED, "TODAY");
  lv_obj_set_pos(l, 14, 190);
  cl_today = make_label(scr, &lv_font_montserrat_14, C_FG, "--");
  lv_obj_align(cl_today, LV_ALIGN_TOP_RIGHT, -14, 188);

  l = make_caps(scr, C_MUTED, "CONTEXT");
  lv_obj_set_pos(l, 14, 214);
  cl_ctx_bar = make_bar(scr, 90, 219, 100, 4);
  cl_ctx_val = make_label(scr, &lv_font_montserrat_14, C_FG, "--");
  lv_obj_align(cl_ctx_val, LV_ALIGN_TOP_RIGHT, -14, 212);
}

// Tab tai khoan Claude: han muc chung cua moi noi dung (Chat, Code, Cowork) + ti le tuan theo noi dung
void create_account_screen() {
  lv_obj_t* scr = screens[SCR_ACCOUNT] = make_screen();
  title(scr, "CLAUDE ACCOUNT");
  make_status(scr, &ac_status, &ac_status_dot);

  lv_obj_t* l = make_caps(scr, C_MUTED, "SESSION");
  lv_obj_set_pos(l, 14, 32);
  ac_sess_pct = make_label(scr, &fr_num_34, C_FG, "--%");
  lv_obj_set_pos(ac_sess_pct, 12, 44);
  ac_sess_meta = make_label(scr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(ac_sess_meta, 14, 88);
  ac_sess_bar = make_bar(scr, 14, 106, 136, 4);

  l = make_caps(scr, C_MUTED, "WEEK");
  lv_obj_set_pos(l, 170, 32);
  ac_week_pct = make_label(scr, &fr_num_34, C_FG, "--%");
  lv_obj_set_pos(ac_week_pct, 168, 44);
  ac_week_meta = make_label(scr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(ac_week_meta, 170, 88);
  ac_week_bar = make_bar(scr, 170, 106, 136, 4);

  make_divider(scr, 122);
  l = make_caps(scr, C_MUTED, "THIS WEEK BY APP");
  lv_obj_set_pos(l, 14, 130);
  for (int i = 0; i < 4; i++) {
    int y = 150 + i * 21;
    ac_name[i] = make_label(scr, &lv_font_montserrat_14, C_FG, "");
    lv_obj_set_pos(ac_name[i], 14, y);
    ac_bar[i] = make_bar(scr, 120, y + 6, 130, 4);
    ac_val[i] = make_label(scr, &lv_font_montserrat_14, C_FG, "");
    lv_obj_align(ac_val[i], LV_ALIGN_TOP_RIGHT, -14, y);
  }
}

static void media_btn_cb(lv_event_t* e) {
  media_action = (int)(intptr_t)lv_event_get_user_data(e);
  if (media_action == 1) {  // doi icon ngay cho cam giac nhanh, server se xac nhan sau
    ui_media.playing = !ui_media.playing;
    lv_label_set_text(mu_play_icon, ui_media.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  }
}

// Tab Music: bai dang phat tren PC (Spotify, YouTube tren Chrome/Edge, ...) qua Windows media controls
void create_music_screen() {
  lv_obj_t* scr = screens[SCR_MUSIC] = make_screen();
  title(scr, "NOW PLAYING");
  mu_app = make_caps(scr, C_MUTED, "");
  lv_obj_align(mu_app, LV_ALIGN_TOP_RIGHT, -14, 10);

  mu_title = make_label(scr, &mont_vi_20, C_FG, "Nothing playing");
  lv_obj_set_width(mu_title, 292);
  lv_label_set_long_mode(mu_title, LV_LABEL_LONG_SCROLL_CIRCULAR);  // ten dai -> chay chu
  lv_obj_set_pos(mu_title, 14, 40);
  mu_artist = make_label(scr, &mont_vi_14, C_MUTED, "Play something on Spotify or YouTube");
  lv_obj_set_width(mu_artist, 292);
  lv_label_set_long_mode(mu_artist, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(mu_artist, 14, 70);

  mu_bar = make_bar(scr, 14, 104, 292, 4);
  mu_pos = make_label(scr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(mu_pos, 14, 114);
  mu_dur = make_label(scr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_align(mu_dur, LV_ALIGN_TOP_RIGHT, -14, 114);

  lv_obj_t* b = make_round_button(scr, 72, 154, 52, C_TILE, C_FG, LV_SYMBOL_PREV, &lv_font_montserrat_20, media_btn_cb, NULL);
  lv_obj_set_user_data(b, NULL);
  lv_obj_remove_event_cb(b, media_btn_cb);
  lv_obj_add_event_cb(b, media_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)3);
  b = make_round_button(scr, 128, 142, 64, C_ACCENT, C_BG, LV_SYMBOL_PLAY, &lv_font_montserrat_24, media_btn_cb, &mu_play_icon);
  lv_obj_remove_event_cb(b, media_btn_cb);
  lv_obj_add_event_cb(b, media_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);
  b = make_round_button(scr, 196, 154, 52, C_TILE, C_FG, LV_SYMBOL_NEXT, &lv_font_montserrat_20, media_btn_cb, NULL);
  lv_obj_remove_event_cb(b, media_btn_cb);
  lv_obj_add_event_cb(b, media_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)2);
}

// Tab System: 4 hang CPU / RAM / GPU / VRAM, cach deu tren man 240px
void create_pc_screen() {
  lv_obj_t* scr = screens[SCR_PC] = make_screen();
  title(scr, "SYSTEM");
  lv_obj_t* name = make_caps(scr, C_MUTED, "ACER NITRO 5");
  lv_obj_align(name, LV_ALIGN_TOP_RIGHT, -14, 10);
  static const char* const names[4] = {"CPU", "RAM", "GPU", "VRAM"};
  for (int i = 0; i < 4; i++) {
    int y = 40 + i * 50;
    lv_obj_t* l = make_caps(scr, C_MUTED, names[i]);
    lv_obj_set_pos(l, 14, y + 2);
    pc_val[i] = make_label(scr, &lv_font_montserrat_16, C_FG, "--");
    lv_obj_align(pc_val[i], LV_ALIGN_TOP_RIGHT, -14, y);
    pc_bar[i] = make_bar(scr, 14, y + 26, 292, 6);
  }
}

static void pomo_update_display();

static void pomo_start_pause_cb(lv_event_t* e) {
  LV_UNUSED(e);
  if (pomo_running) {
    pomo_remaining_ms = pomo_end_ms - millis();
    pomo_running = false;
  } else {
    pomo_end_ms = millis() + pomo_remaining_ms;
    pomo_running = true;
  }
  pomo_update_display();
}

static void pomo_reset_cb(lv_event_t* e) {
  LV_UNUSED(e);
  pomo_running = false;
  pomo_remaining_ms = pomo_duration(pomo_phase);
  pomo_update_display();
}

// Nut - / +: doi do dai pha hien tai 5 phut moi lan (chi khi dang dung), luu NVS
static void pomo_adjust_cb(lv_event_t* e) {
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  if (pomo_running) { flash("PAUSE TO CHANGE TIME"); return; }
  int v = constrain(pomo_min[pomo_phase] + dir * POMO_STEP_MIN, POMO_STEP_MIN, POMO_MAX[pomo_phase]);
  pomo_min[pomo_phase] = v;
  static const char* const keys[3] = {"pf", "pb", "pl"};
  prefs.putUChar(keys[pomo_phase], v);
  pomo_remaining_ms = pomo_duration(pomo_phase);
  pomo_update_display();
}

void create_pomo_screen() {
  lv_obj_t* scr = screens[SCR_POMO] = make_screen();
  title(scr, "POMODORO");
  po_count = make_caps(scr, C_MUTED, "");
  lv_obj_align(po_count, LV_ALIGN_TOP_RIGHT, -14, 10);
  po_phase = make_label(scr, &lv_font_montserrat_16, C_FG, "");
  lv_obj_align(po_phase, LV_ALIGN_TOP_MID, 0, 32);
  po_time = make_label(scr, &fr_num_64, C_FG, "25:00");
  lv_obj_align(po_time, LV_ALIGN_TOP_MID, 0, 54);
  po_len = make_caps(scr, C_MUTED, "");
  lv_obj_align(po_len, LV_ALIGN_TOP_MID, 0, 132);

  // - / + hai ben dong ho, cung kieu nut tron nhu tab Music
  lv_obj_t* b = make_round_button(scr, 14, 66, 44, C_TILE, C_FG, LV_SYMBOL_MINUS, &lv_font_montserrat_16, pomo_adjust_cb, NULL);
  lv_obj_remove_event_cb(b, pomo_adjust_cb);
  lv_obj_add_event_cb(b, pomo_adjust_cb, LV_EVENT_CLICKED, (void*)(intptr_t)-1);
  b = make_round_button(scr, 262, 66, 44, C_TILE, C_FG, LV_SYMBOL_PLUS, &lv_font_montserrat_16, pomo_adjust_cb, NULL);
  lv_obj_remove_event_cb(b, pomo_adjust_cb);
  lv_obj_add_event_cb(b, pomo_adjust_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);

  po_bar = make_bar(scr, 40, 154, 240, 6);
  make_button(scr, 40, 176, 140, C_ACCENT, C_BG, "Start", pomo_start_pause_cb, &po_btn_label);
  make_button(scr, 192, 176, 88, C_TILE, C_FG, "Reset", pomo_reset_cb, NULL);
}

// Nhan thong bao ngan tren lop tren cung (them/bot slideshow, ...)
void create_flash_badge() {
  flash_badge = lv_label_create(lv_layer_top());
  lv_obj_set_style_text_font(flash_badge, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(flash_badge, lv_color_hex(C_BG), 0);
  lv_obj_set_style_text_letter_space(flash_badge, 1, 0);
  lv_obj_set_style_bg_color(flash_badge, lv_color_hex(C_FG), 0);
  lv_obj_set_style_bg_opa(flash_badge, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(flash_badge, 12, 0);
  lv_obj_set_style_pad_hor(flash_badge, 14, 0);
  lv_obj_set_style_pad_ver(flash_badge, 8, 0);
  lv_obj_align(flash_badge, LV_ALIGN_CENTER, 0, 0);
  no_click(flash_badge);
  lv_obj_add_flag(flash_badge, LV_OBJ_FLAG_HIDDEN);
}

static void toast_click_cb(lv_event_t* e) {
  LV_UNUSED(e);
  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

// The thong bao noi tren moi trang; tap vao the de an
void create_toast() {
  toast = lv_obj_create(lv_layer_top());
  lv_obj_set_size(toast, 300, 80);
  lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_color(toast, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(toast, 3, 0);
  lv_obj_set_style_border_color(toast, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_radius(toast, 14, 0);
  lv_obj_set_style_pad_all(toast, 10, 0);
  lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(toast, toast_click_cb, LV_EVENT_CLICKED, NULL);
  toast_title = make_label(toast, &lv_font_montserrat_20, C_FG, "");
  lv_obj_set_pos(toast_title, 0, 0);
  toast_sub = make_label(toast, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_set_width(toast_sub, 274);
  lv_label_set_long_mode(toast_sub, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(toast_sub, 0, 32);
  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

static void show_toast(const char* t, const char* sub, uint32_t color) {
  lv_label_set_text(toast_title, t);
  lv_label_set_text(toast_sub, sub);
  lv_obj_set_style_border_color(toast, lv_color_hex(color), 0);
  lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(toast);
  toast_hide_at = millis() + TOAST_MS;
}

// =========================
// UI UPDATE
// =========================
static void update_clock_weather_line() {
  char buf[96];
  if (!ui_weather.valid) { lv_label_set_text(lb_clock_weather, ""); return; }
  int n = snprintf(buf, sizeof(buf), "%s | %.0f%s", weather_desc(ui_weather.weather_code), ui_weather.temperature, DEGREE_SYMBOL);
  if (ui_aqi.valid) snprintf(buf + n, sizeof(buf) - n, " | AQI %d", ui_aqi.aqi);
  lv_label_set_text(lb_clock_weather, buf);
}

void update_clock_display() {
  struct tm t;
  if (!get_time_now(&t)) return;
  char buf[96];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  lv_label_set_text(lb_clock_time, buf);
  strftime(buf, sizeof(buf), "%H:%M", &t);
  lv_label_set_text(home_val[SCR_CLOCK], buf);
  lv_label_set_text(home_clock, buf);

  static int last_mday = -1;
  if (t.tm_mday != last_mday) {  // ngay + am lich chi tinh lai khi sang ngay moi
    last_mday = t.tm_mday;
    strftime(buf, sizeof(buf), "%A, %d %b %Y", &t);
    lv_label_set_text(lb_clock_date, buf);
    format_lunar(t, buf, sizeof(buf));
    lv_label_set_text(lb_clock_lunar, buf);
  }
}

void update_weather_display() {
  bool w_new = false, a_new = false;
  if (weather_dirty || aqi_dirty) {
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    if (weather_dirty) { ui_weather = shared_weather; weather_dirty = false; w_new = true; }
    if (aqi_dirty) { ui_aqi = shared_aqi; aqi_dirty = false; a_new = true; }
    xSemaphoreGive(data_mutex);
  }
  if (!w_new && !a_new) return;
  char buf[64];

  if (a_new && ui_aqi.valid) {
    AqiLevel lv = aqi_level(ui_aqi.aqi);
    snprintf(buf, sizeof(buf), "AQI %d | %s", ui_aqi.aqi, lv.text);
    lv_label_set_text(lb_w_aqi, buf);
    lv_obj_set_style_bg_color(lb_w_aqi, lv_color_hex(lv.bg), 0);
    lv_obj_set_style_text_color(lb_w_aqi, lv_color_hex(lv.fg), 0);
  }

  if (w_new && ui_weather.valid) {
    const WeatherData& w = ui_weather;
    lv_image_set_src(img_weather, weather_image(w.weather_code, w.is_day));
    snprintf(buf, sizeof(buf), "%.1f%s", w.temperature, DEGREE_SYMBOL);
    lv_label_set_text(lb_w_temp, buf);
    snprintf(buf, sizeof(buf), "%.0f°", w.temperature);
    lv_label_set_text(home_val[SCR_WEATHER], buf);
    snprintf(buf, sizeof(buf), "Feels like %.0f°", w.feels);
    lv_label_set_text(lb_w_feels, buf);
    snprintf(buf, sizeof(buf), "Humidity %d%%", w.humidity);
    lv_label_set_text(lb_w_hum, buf);
    snprintf(buf, sizeof(buf), "Wind %.0f km/h", w.wind);
    lv_label_set_text(lb_w_wind, buf);
    lv_label_set_text(lb_w_desc, weather_desc(w.weather_code));
    snprintf(buf, sizeof(buf), "Updated %s | Open-Meteo", w.last_update);
    lv_label_set_text(lb_w_info, buf);

    // Forecast
    for (int i = 0; i < 6; i++) {
      bool has = i < w.n_hours;
      const HourFc& f = w.hours[i];
      lv_label_set_text(lb_fh_hour[i], has ? f.hour : "");
      if (has) {
        lv_image_set_src(img_fh[i], weather_image(f.code, f.day));
        lv_obj_remove_flag(img_fh[i], LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "%.0f°", f.temp);
        lv_label_set_text(lb_fh_temp[i], buf);
        snprintf(buf, sizeof(buf), "%d%%", f.rain);
        lv_label_set_text(lb_fh_rain[i], buf);
      } else {
        lv_obj_add_flag(img_fh[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lb_fh_temp[i], "");
        lv_label_set_text(lb_fh_rain[i], "");
      }
    }
    for (int i = 0; i < 3; i++) {
      bool has = i < w.n_days;
      const DayFc& f = w.days[i];
      lv_label_set_text(lb_fd_name[i], !has ? "" : i == 0 ? "Today" : i == 1 ? "Tomorrow" : WEEKDAY_EN[f.wday]);
      if (has) {
        lv_image_set_src(img_fd[i], weather_image(f.code, true));
        snprintf(buf, sizeof(buf), "%d%%", f.rain);
        lv_label_set_text(lb_fd_rain[i], buf);
        snprintf(buf, sizeof(buf), "%.0f° - %.0f°", f.tmin, f.tmax);
        lv_label_set_text(lb_fd_temp[i], buf);
      }
    }
    if (w.n_days > 1) {  // Home: nhiet do ngay mai
      snprintf(buf, sizeof(buf), "%.0f-%.0f°", w.days[1].tmin, w.days[1].tmax);
      lv_label_set_text(home_val[SCR_FORECAST], buf);
    }
  }
  update_clock_weather_line();
}

static void set_temp_color(lv_obj_t* label, int temp_c) {
  lv_obj_set_style_text_color(label, lv_color_hex(temp_c >= 85 ? C_ALERT : C_FG), 0);
}

static void update_pc_display(const PcData& p) {
  char buf[48];
  if (!p.valid) return;
  // CPU: tai + nhiet do (cam bien nhiet ACPI cua mainboard)
  if (p.cpu >= 0 && p.cpu_temp >= 0) snprintf(buf, sizeof(buf), "%d%% | %d°C", p.cpu, p.cpu_temp);
  else if (p.cpu >= 0) snprintf(buf, sizeof(buf), "%d%%", p.cpu);
  else snprintf(buf, sizeof(buf), "--");
  lv_label_set_text(pc_val[0], buf);
  set_temp_color(pc_val[0], p.cpu_temp);
  set_pct_bar(pc_bar[0], max(p.cpu, 0));
  if (p.cpu_temp >= 0) {
    snprintf(buf, sizeof(buf), "%d°C", p.cpu_temp);
    lv_label_set_text(home_val[SCR_PC], buf);
  }

  snprintf(buf, sizeof(buf), "%.1f / %.1f GB | %d%%", p.ram_used, p.ram_total, p.ram_pct);
  lv_label_set_text(pc_val[1], buf);
  set_pct_bar(pc_bar[1], p.ram_pct);

  if (p.gpu) {
    snprintf(buf, sizeof(buf), "%d%% | %d°C", p.gpu_util, p.gpu_temp);
    lv_label_set_text(pc_val[2], buf);
    set_temp_color(pc_val[2], p.gpu_temp);
    set_pct_bar(pc_bar[2], p.gpu_util);
    snprintf(buf, sizeof(buf), "%.1f / %.1f GB", p.vram_used, p.vram_total);
    lv_label_set_text(pc_val[3], buf);
    set_pct_bar(pc_bar[3], p.vram_total > 0 ? (int)(p.vram_used * 100 / p.vram_total) : 0);
  } else {
    lv_label_set_text(pc_val[2], "no NVIDIA GPU");
    lv_label_set_text(pc_val[3], "--");
  }
}

// Tab Music: noi suy vi tri moi giay khi dang phat (server chi hoi moi 2s)
static void update_music_progress() {
  const MediaData& m = ui_media;
  if (!m.has || m.dur_s <= 0) { lv_bar_set_value(mu_bar, 0, LV_ANIM_OFF); return; }
  int pos = m.pos_s + (m.playing ? (int)((millis() - m.rx_ms) / 1000) : 0);
  pos = min(pos, m.dur_s);
  char buf[16];
  fmt_mmss(pos, buf, sizeof(buf));
  lv_label_set_text(mu_pos, buf);
  fmt_mmss(m.dur_s, buf, sizeof(buf));
  lv_label_set_text(mu_dur, buf);
  lv_bar_set_value(mu_bar, pos * 100 / m.dur_s, LV_ANIM_OFF);
}

static void update_music_display(const MediaData& m) {
  if (!m.valid) return;
  bool changed = strcmp(m.title, ui_media.title) != 0 || strcmp(m.artist, ui_media.artist) != 0 || !ui_media.valid;
  ui_media = m;
  lv_label_set_text(home_val[SCR_MUSIC], m.has && m.playing ? LV_SYMBOL_PLAY : m.has ? LV_SYMBOL_PAUSE : LV_SYMBOL_AUDIO);
  if (m.error[0]) {
    lv_label_set_text(mu_title, "Media reader unavailable");
    lv_label_set_text(mu_artist, "pip install winrt-Windows.Media.Control on the PC");
    lv_label_set_text(mu_app, "");
  } else if (!m.has || !m.title[0]) {
    lv_label_set_text(mu_title, "Nothing playing");
    lv_label_set_text(mu_artist, "Play something on Spotify or YouTube");
    lv_label_set_text(mu_app, "");
    lv_label_set_text(mu_pos, "");
    lv_label_set_text(mu_dur, "");
  } else {
    if (changed) {  // dat lai chu chi khi doi bai, de hieu ung chay chu khong bi giat
      lv_label_set_text(mu_title, m.title);
      lv_label_set_text(mu_artist, m.artist[0] ? m.artist : " ");
    }
    String app = m.app;
    app.toUpperCase();
    lv_label_set_text(mu_app, app.c_str());
  }
  lv_label_set_text(mu_play_icon, m.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  update_music_progress();
}

static void update_account_display(const ClaudeData& c, const char* status, uint32_t dot) {
  char buf[48];
  lv_label_set_text(ac_status, status);
  lv_obj_set_style_bg_color(ac_status_dot, lv_color_hex(dot), 0);
  snprintf(buf, sizeof(buf), "%d%%", c.session_pct);
  lv_label_set_text(ac_sess_pct, buf);
  set_pct_bar(ac_sess_bar, c.session_pct);
  if (c.resets_at[0]) snprintf(buf, sizeof(buf), "resets %s", c.resets_at); else buf[0] = '\0';
  lv_label_set_text(ac_sess_meta, buf);
  snprintf(buf, sizeof(buf), "%d%%", c.week_pct);
  lv_label_set_text(ac_week_pct, buf);
  lv_label_set_text(home_val[SCR_ACCOUNT], buf);
  set_pct_bar(ac_week_bar, c.week_pct);
  if (c.week_resets_in[0]) snprintf(buf, sizeof(buf), "resets in %s", c.week_resets_in); else buf[0] = '\0';
  lv_label_set_text(ac_week_meta, buf);

  for (int i = 0; i < 4; i++) {
    bool has = c.account.valid && i < c.account.n;
    lv_label_set_text(ac_name[i], has ? c.account.name[i] : (i == 0 && !c.account.valid ? "needs claude auth login" : ""));
    if (has) {
      snprintf(buf, sizeof(buf), "%d%%", c.account.pct[i]);
      lv_label_set_text(ac_val[i], buf);
      lv_bar_set_value(ac_bar[i], constrain(c.account.pct[i], 0, 100), LV_ANIM_OFF);
      lv_obj_remove_flag(ac_bar[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_label_set_text(ac_val[i], "");
      lv_obj_add_flag(ac_bar[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void handle_claude_event(const ClaudeData& c) {
  static int last_event_id = -1;
  if (c.event_id <= 0) { if (last_event_id < 0) last_event_id = 0; return; }
  if (c.event_id == last_event_id) return;
  bool first = last_event_id < 0;
  last_event_id = c.event_id;
  if (first && c.event_age > 120) return;  // su kien cu luc vua khoi dong -> bo qua

  char sub[180];
  if (strcmp(c.event_type, "notification") == 0) {
    const char* what = strstr(c.event_msg, "permission") ? "needs your permission"
                     : (strstr(c.event_msg, "waiting") || strstr(c.event_msg, "idle")) ? "waiting for your reply"
                     : c.event_msg;
    snprintf(sub, sizeof(sub), "%s | %s", c.event_project, what);
    show_toast(LV_SYMBOL_BELL " Claude needs you", sub, C_WARN);
    beep(BEEP_WAIT);
  } else {
    snprintf(sub, sizeof(sub), "%s | tap to dismiss", c.event_project);
    show_toast(LV_SYMBOL_OK " Claude is done", sub, C_OK);
    beep(BEEP_DONE);
  }
}

void update_claude_display() {
  if (!claude_dirty) return;
  static ClaudeData c;  // static: struct lon (~1 KB), tranh tran stack loop
  xSemaphoreTake(data_mutex, portMAX_DELAY);
  c = shared_claude;
  claude_dirty = false;
  xSemaphoreGive(data_mutex);

  char buf[64];
  if (!c.valid) {
    const char* st = c.stale ? "PC OFFLINE" : "WAITING FOR PC...";
    uint32_t dot = c.stale ? C_ALERT : C_MUTED;
    lv_label_set_text(cl_status, st);
    lv_obj_set_style_bg_color(cl_status_dot, lv_color_hex(dot), 0);
    lv_label_set_text(ac_status, st);
    lv_obj_set_style_bg_color(ac_status_dot, lv_color_hex(dot), 0);
    return;
  }

  // Header: goi + trang thai. Xanh = so chinh xac, vang = uoc tinh, do = mat ket noi (giu so cu)
  String plan = c.plan[0] ? String(c.plan) : String("plan ?");
  plan.toUpperCase();
  if (c.stale) plan += "  OFFLINE";
  else if (!c.exact) plan += "  ~EST";
  uint32_t dot = c.stale ? C_ALERT : c.exact ? C_OK : C_WARN;
  lv_label_set_text(cl_status, plan.c_str());
  lv_obj_set_style_bg_color(cl_status_dot, lv_color_hex(dot), 0);

  snprintf(buf, sizeof(buf), "%d%%", c.session_pct);
  lv_label_set_text(cl_session_pct, buf);
  lv_label_set_text(home_val[SCR_CLAUDE], buf);
  set_pct_bar(cl_session_bar, c.session_pct);
  if (c.resets_at[0]) {
    snprintf(buf, sizeof(buf), "resets %s", c.resets_at);
    lv_label_set_text(cl_session_reset, buf);
    int m = max(c.minutes_remaining, 0);
    if (m >= 60) snprintf(buf, sizeof(buf), "%dh %dm left", m / 60, m % 60);
    else snprintf(buf, sizeof(buf), "%dm left", m);
    lv_label_set_text(cl_session_left, buf);
  } else {
    lv_label_set_text(cl_session_reset, "no active session");
    lv_label_set_text(cl_session_left, "");
  }

  snprintf(buf, sizeof(buf), "%d%%", c.week_pct);
  lv_label_set_text(cl_week_pct, buf);
  set_pct_bar(cl_week_bar, c.week_pct);
  if (c.week_resets_in[0]) snprintf(buf, sizeof(buf), "resets in %s", c.week_resets_in);
  else buf[0] = '\0';
  lv_label_set_text(cl_week_meta, buf);

  if (c.today_tokens >= 1000000) snprintf(buf, sizeof(buf), "%.1fM tok  |  $%.2f", c.today_tokens / 1000000.0, c.today_cost);
  else snprintf(buf, sizeof(buf), "%ldk tok  |  $%.2f", c.today_tokens / 1000, c.today_cost);
  lv_label_set_text(cl_today, buf);

  // Ngu canh: "672k / 1M" + thanh nho (do khi >= 85% -> nen mo phien moi)
  if (c.ctx_tokens >= 0 && c.ctx_limit > 0) {
    char lim[12];
    if (c.ctx_limit >= 1000000) snprintf(lim, sizeof(lim), "%ldM", c.ctx_limit / 1000000);
    else snprintf(lim, sizeof(lim), "%ldk", c.ctx_limit / 1000);
    snprintf(buf, sizeof(buf), "%ldk / %s", c.ctx_tokens / 1000, lim);
    lv_label_set_text(cl_ctx_val, buf);
    set_pct_bar(cl_ctx_bar, (int)(c.ctx_tokens * 100 / c.ctx_limit));
  } else {
    lv_label_set_text(cl_ctx_val, "no active chat");
    set_pct_bar(cl_ctx_bar, 0);
  }

  update_account_display(c, plan.c_str(), dot);
  update_pc_display(c.pc);
  update_music_display(c.media);
  if (!c.stale) handle_claude_event(c);
}

static void pomo_update_display() {
  static const char* const phases[] = {"Focus", "Short break", "Long break"};
  unsigned long left = pomo_running ? (long)(pomo_end_ms - millis()) > 0 ? pomo_end_ms - millis() : 0 : pomo_remaining_ms;
  unsigned long secs = (left + 999) / 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", secs / 60, secs % 60);
  lv_label_set_text(po_time, buf);
  lv_label_set_text(home_val[SCR_POMO], buf);
  lv_label_set_text(po_phase, phases[pomo_phase]);
  lv_obj_set_style_text_color(po_phase, lv_color_hex(pomo_phase == POMO_FOCUS ? C_ACCENT : C_OK), 0);
  unsigned long total = pomo_duration(pomo_phase);
  lv_bar_set_value(po_bar, (int)((total - min(left, total)) * 100 / total), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(po_bar, lv_color_hex(pomo_phase == POMO_FOCUS ? C_ACCENT : C_OK), LV_PART_INDICATOR);
  lv_label_set_text(po_btn_label, pomo_running ? "Pause" : "Start");
  snprintf(buf, sizeof(buf), "%d SESSION%s DONE", pomo_done, pomo_done == 1 ? "" : "S");
  lv_label_set_text(po_count, buf);
  snprintf(buf, sizeof(buf), "FOCUS %d  |  BREAK %d  |  LONG %d MIN", pomo_min[0], pomo_min[1], pomo_min[2]);
  lv_label_set_text(po_len, buf);
}

static void pomo_tick() {
  if (!pomo_running || (long)(pomo_end_ms - millis()) > 0) return;
  pomo_running = false;
  if (pomo_phase == POMO_FOCUS) {
    pomo_done++;
    pomo_phase = (pomo_done % 4 == 0) ? POMO_LONG : POMO_BREAK;
    char sub[64];
    snprintf(sub, sizeof(sub), "Take a %d min break | tap Start", pomo_min[pomo_phase]);
    show_toast("Focus time is up!", sub, C_ACCENT);
  } else {
    pomo_phase = POMO_FOCUS;
    show_toast("Break is over!", "Back to focus | tap Start", C_OK);
  }
  pomo_remaining_ms = pomo_duration(pomo_phase);
  beep(BEEP_POMO);
  if (current_screen != SCR_POMO) show_screen(SCR_POMO);
  pomo_update_display();
}

// =========================
// SETUP / LOOP
// =========================
void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32 CYD...");
  data_mutex = xSemaphoreCreateMutex();
  prefs.begin("cyd", false);

  // === DEN NEN: luon sang toi da (chan BL keo HIGH co dinh, khong PWM / khong tu giam) ===
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  // === Loa: tat (chi keu khi co thong bao) ===
  ledcAttach(SPEAKER_PIN, 2000, 8);
  ledcWriteTone(SPEAKER_PIN, 0);

  // === Cam ung ===
  touch_spi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touch_spi);
  touchscreen.setRotation(2);

  // === Cai dat da luu ===
  slides_mask = prefs.getUShort("slides", DEFAULT_SLIDES);
  pomo_min[0] = prefs.getUChar("pf", 25);
  pomo_min[1] = prefs.getUChar("pb", 5);
  pomo_min[2] = prefs.getUChar("pl", 15);
  pomo_remaining_ms = pomo_duration(POMO_FOCUS);

  // === Khoi tao LVGL + man hinh ===
  lv_init();
  lv_tick_set_cb(lv_tick_millis);   // tick chinh xac theo millis()
  lv_log_register_print_cb(log_print);
  lv_display_t* disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);   // lat lai man (doi 270 -> 90)
  digitalWrite(TFT_BL_PIN, HIGH);                          // dam bao van sang toi da sau khi driver khoi tao

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);
  lv_indev_set_long_press_time(indev, LONG_PRESS_MS);

  create_home_screen();
  create_clock_screen();
  create_weather_screen();
  create_forecast_screen();
  create_claude_screen();
  create_account_screen();
  create_music_screen();
  create_pc_screen();
  create_pomo_screen();
  create_flash_badge();
  create_toast();
  pomo_update_display();

  // Khoi phuc che do + tab truoc khi mat dien
  nav_mode = (NavMode)prefs.getUChar("mode", MODE_SLIDESHOW);
  uint8_t s = prefs.getUChar("screen", SCR_CLOCK);
  ScreenState start = (s < SCREEN_COUNT && (s == SCR_HOME || screen_enabled((ScreenState)s))) ? (ScreenState)s : SCR_CLOCK;
  if (nav_mode == MODE_HOME) start = SCR_HOME;
  if (nav_mode == MODE_SLIDESHOW && !in_slideshow(start)) start = next_slide(start);
  show_screen(start);
  lv_timer_handler();

  // LVGL dung heap chung cua ESP32 (LV_STDLIB_CLIB trong lv_conf.h)
  Serial.printf("Heap after UI: %u bytes free, largest block %u\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());

  // === WiFi khong chan: mang chay o task rieng, UI van chay khi mat mang ===
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  xTaskCreatePinnedToCore(network_task, "net", 10240, NULL, 1, NULL, 0);

  Serial.println("Setup completed!");
}

void loop() {
  unsigned long now = millis();

  if (nav_mode == MODE_SLIDESHOW && now - last_screen_switch >= SLIDESHOW_INTERVAL) {
    show_screen(next_slide(current_screen));
  }

  if (now - last_display_update >= DISPLAY_UPDATE_INTERVAL) {
    update_clock_display();
    pomo_update_display();
    if (current_screen == SCR_MUSIC) update_music_progress();
    last_display_update = now;
  }
  update_weather_display();
  update_claude_display();
  pomo_tick();
  beep_tick();
  if (toast_hide_at && now >= toast_hide_at) {
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    toast_hide_at = 0;
  }
  if (flash_hide_at && now >= flash_hide_at) {
    lv_obj_add_flag(flash_badge, LV_OBJ_FLAG_HIDDEN);
    flash_hide_at = 0;
  }

  lv_timer_handler();
  delay(5);
}
