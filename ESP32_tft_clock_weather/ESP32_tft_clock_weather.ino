// ============================
// ESP32 CYD (ESP32-2432S028R, 2.8" ILI9341 + XPT2046) - Ngu Hanh Son, Da Nang
// Phong cach "Claude editorial": nen giay nga, muc nau, mau dat nung, so serif Fraunces.
//
// HOME: luoi 9 o. Tap o -> mo tab do (dung yen). Tap trong tab -> ve HOME.
//       O SLIDESHOW -> tu chuyen qua cac tab da chon. Giu 1 o (hoac giu trong tab) -> them/bot khoi slideshow.
//       Nut banh rang -> Settings (tieng bip, che do thong bao, toc do slideshow, canh bao).
// Tab: Clock (+ am lich), Weather (+ AQI), Forecast, Claude Code, Claude account, Music, System, Pomodoro.
//
// Cac file:
//   ESP32_tft_clock_weather.ino  cau hinh, bien dung chung, dieu huong, watchdog, setup/loop (file nay)
//   net.ino                      WiFi, Open-Meteo, PC server, anh bia album (task mang, core 0)
//   ui_build.ino                 tao giao dien + xu ly cham
//   setup_portal.ino             trang Wi-Fi setup (WiFi "CYD-Setup" + trang web 192.168.4.1)
//   ui_update.ino                cap nhat giao dien, thong bao, canh bao, Pomodoro
//   cyd_types.h                  kieu du lieu dung chung
// Den nen sang toi da (Settings > SCREEN = WITH PC: tat khi PC khoa / ngu). Mau dao o User_Setup.h.
// WiFi / token / vi tri nhap qua trang Wi-Fi setup (luu NVS); secrets.h (tuy chon) chi la gia tri mac dinh.
// Ban sao lv_conf.h va User_Setup.h can dung nam o thu muc config/ cua du an.
// ============================

#include <WebServer.h>     // trang Wi-Fi setup (phai dung truoc TFT_eSPI / lvgl)
#include <DNSServer.h>
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
#if __has_include("secrets.h")
#include "secrets.h"            // tuy chon: WiFi / token nap san (khong dua len GitHub)
#else
#include "secrets.example.h"    // ban build chung (web flasher): nhap moi thu qua trang Wi-Fi setup
#endif
#include "weather_images.h"
#include "lunar.h"
#include "cyd_types.h"
#include "src/fonts/mont_fonts.h"

// =========================
// CONFIG
// =========================
#define FW_VERSION "2.0.0"

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
// Bo dem ve 1/4 man hinh (truoc la 1/10): it lan day SPI hon -> doi tab va chu chay muot hon
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 4 * (LV_COLOR_DEPTH / 8))
// (Do bang lenh BENCH: 1/4 ve lai ca man 54.7 ms, 1/8 75.7 ms -> giu 1/4 du ton 19 KB RAM)

#define TFT_BL_PIN 21      // den nen: HIGH = sang 100% (khong PWM, khong tu giam)

// Cam ung XPT2046 nam tren bus SPI rieng cua CYD.
// TFT_eSPI dang dung VSPI (khong co USE_HSPI_PORT trong User_Setup.h) nen touch dung HSPI.
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define SPEAKER_PIN  26    // cong loa cua CYD (khong gan loa thi khong anh huong gi)

#define DISPLAY_UPDATE_INTERVAL   1000UL
#define WEATHER_UPDATE_INTERVAL 600000UL
#define AQI_UPDATE_INTERVAL    1800000UL
#define NET_RETRY_INTERVAL       60000UL   // loi thi thu lai sau 60s (khong spam moi vong loop)
#define AQI_QUICK_RETRY          10000UL   // AQI hay bi het gio lan dau -> thu lai nhanh 1 lan
// PC server bao thay doi qua UDP ("CLAUDE_DIRTY") -> CYD lay ngay; poll chi la luoi an toan
#define CLAUDE_POLL_ACTIVE       15000UL   // dang xem tab Claude / Account
#define PC_POLL_ACTIVE            3000UL   // dang xem tab System (cam bien khong co "thay doi" de bao)
#define MEDIA_POLL_ACTIVE        10000UL   // dang xem tab Music (vi tri bai tu noi suy moi giay)
#define PERM_POLL_ACTIVE          2000UL   // dang hien the xin quyen: biet ngay khi da tra loi tren PC
#define CLAUDE_POLL_IDLE         60000UL
#define CLAUDE_BACKOFF_START      2000UL
#define CLAUDE_BACKOFF_MAX       60000UL
#define WIFI_RETRY_INTERVAL      15000UL
#define HTTP_TIMEOUT_MS           5000
#define AQI_TIMEOUT_MS           10000     // may chu AQI cham hon
#define LONG_PRESS_MS              800     // giu de them / bot tab khoi slideshow
#define FLASH_MS                  1500UL   // nhan thong bao ngan giua man, roi tu an
#define TOAST_MS                 25000UL   // the thong bao tu an sau 25s (hoac tap de an)
#define BEACON_PORT               7843     // PC server phat UDP: tim IP + bao su kien Claude
#define ART_SIZE                    80     // anh bia album 80x80 RGB565 (server da thu nho san)
#define ART_RADIUS                  10     // bo goc anh bia (to mau nen vao 4 goc ngay khi tai ve)

// Bao ve khi chay 24/7
#define NET_STALL_RESTART_MS    180000UL   // task mang dung > 3 phut -> khoi dong lai
#define HEAP_CHECK_INTERVAL      60000UL
// Khoi DRAM lon nhat (dram_largest). Binh thuong ~49 KB; ~18 KB khi giu anh bia tab Music.
// Ve 1 chu so lon can ~3 KB lien tuc: duoi muc nay LVGL het bo nho va treo -> khoi dong lai cho sach.
#define HEAP_CRITICAL_BYTES       6000     // < 6 KB -> khoi dong lai ngay
#define HEAP_LOW_BYTES           16000     // < 16 KB -> khoi dong lai luc 4:00 sang

#define SCREEN_OFF_AFTER_MS     120000UL   // SCREEN = WITH PC: mat PC 2 phut -> tat den nen
#define SCREEN_WAKE_MS           30000UL   // cham khi den tat -> sang 30s

#define NOTIFY_AWAY_IDLE_S          60     // che do AWAY: ban roi chuot/phim >= 1 phut
#define NOTIFY_AWAY_TASK_S         120     // ... hoac luot Claude chay >= 2 phut
#define RAIN_ALERT_PCT              70     // canh bao mua khi kha nang mua gio toi >= 70%

#define POMO_STEP_MIN 5                    // nut - / + doi 5 phut moi lan

const char* NTP_SERVER = "pool.ntp.org";

// Mac dinh khi chua nhap o trang Wi-Fi setup: Ngu Hanh Son, Da Nang, gio Viet Nam
#define DEFAULT_TZ  "ICT-7"
#define DEFAULT_LAT "16.0044"
#define DEFAULT_LON "108.2632"

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
#define C_ACCENT_P 0xA34F31  // dat nung khi bam
#define C_BAR_BG  0xE6DCCD   // ranh bar / duong ke
#define C_OK      0x2E9E4F   // xanh la: dang ket noi / so lieu chinh xac
#define C_WARN    0xD9A21B   // vang: dang uoc tinh
#define C_ALERT   0xB3261E   // do: >= 85%, offline, qua nong
#define C_BLUE    0x4A6FA5   // xanh xam diu (do am, kha nang mua)
#define C_TEMP    C_ACCENT

static const char* const WEEKDAY_EN[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Ten tab (tren Home va thong bao)
static const char* const SCREEN_NAMES[SCREEN_COUNT] = {
  "HOME", "CLOCK", "WEATHER", "FORECAST", "CLAUDE CODE", "ACCOUNT", "MUSIC", "SYSTEM", "POMODORO", "SETTINGS",
  "DEVICE", "WI-FI SETUP"
};
#define DEFAULT_SLIDES ((1 << SCR_CLOCK) | (1 << SCR_WEATHER) | (1 << SCR_FORECAST) | (1 << SCR_CLAUDE))

// =========================
// SHARED DATA (task mang <-> loop giao dien), bao ve bang mutex
// =========================
static SemaphoreHandle_t data_mutex;
static WeatherData shared_weather;
static AqiData shared_aqi;
static ClaudeData shared_claude;
static volatile bool weather_dirty = false, aqi_dirty = false, claude_dirty = false;
static volatile bool claude_poll_now = false;
static volatile int media_action = 0;   // 1 = play/pause, 2 = next, 3 = prev (UI dat, task mang gui)
static volatile int perm_answer_id = 0;  // tra loi the xin quyen (task mang gui): id ...
static volatile int perm_answer = 0;     // ... va 1 = allow / 2 = deny
static NetConfig netcfg;                 // doc 1 lan luc khoi dong (sau do chi doc)
static DevStats dev;
static volatile bool portal_active = false;   // dang o che do Wi-Fi setup
static volatile ScreenState current_screen = SCR_HOME;
static volatile uint32_t net_heartbeat = 0;  // task mang cap nhat moi vong -> loop phat hien treo

// Anh bia album: 2 bo dem (task mang ghi vao bo dem khong dang hien thi, roi doi).
// Chi cap phat khi o tab Music, tra lai sau khi roi tab ART_KEEP_MS (25 KB RAM cho phan con lai).
// Bao ve boi data_mutex: art_buf, art_busy, art_loaded_id.
#define ART_BYTES (ART_SIZE * ART_SIZE * 2)
#define ART_KEEP_MS 300000UL   // 5 phut: slideshow quay lai Music van con anh
static uint8_t* art_buf[2] = {nullptr, nullptr};
static lv_image_dsc_t art_dsc[2];
static volatile int art_front = -1;          // bo dem dang hien thi (-1 = khong co anh)
static volatile bool art_dirty = false;
static bool art_busy = false;                // task mang dang ghi vao bo dem sau
static char art_loaded_id[12] = "";

// =========================
// UI GLOBALS (chi dung trong loop task)
// =========================
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

SPIClass touch_spi(HSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
Preferences prefs;

static Settings settings;
static NavMode nav_mode = MODE_SLIDESHOW;
static uint16_t slides_mask = DEFAULT_SLIDES;
static unsigned long last_screen_switch = 0;
static unsigned long last_display_update = 0;

static lv_display_t* main_disp;
static bool ui_pc_locked = false;
static WeatherData ui_weather;          // ban sao cho UI
static AqiData ui_aqi;
static MediaData ui_media;

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
static lv_obj_t *mu_app, *mu_title, *mu_artist, *mu_bar, *mu_pos, *mu_dur, *mu_play_icon, *mu_art;
// System
static lv_obj_t *pc_val[4], *pc_bar[4];
// Pomodoro
static lv_obj_t *po_phase, *po_time, *po_bar, *po_count, *po_btn_label, *po_len;
// Settings: [hang][lua chon]
static lv_obj_t* set_btn[5][4];
// Device
static lv_obj_t *dv_val[8], *dv_uptime;
// Wi-Fi setup
static lv_obj_t* su_status;
// Clock: su kien lich
static lv_obj_t* lb_clock_event;
// The xin quyen
static lv_obj_t *perm_card, *perm_count, *perm_where, *perm_detail;
static int perm_shown_id = 0;
static unsigned long perm_deadline = 0;
// Den nen
static bool backlight_on = true;
static unsigned long wake_until = 0, pc_seen_ms = 0;
static bool swallow_touch = false;
// Toast
static lv_obj_t *toast, *toast_title, *toast_sub;
static unsigned long toast_hide_at = 0;

// Pomodoro (do dai moi pha luu trong NVS)
static PomoPhase pomo_phase = POMO_FOCUS;
static bool pomo_running = false;
static int pomo_min[3] = {25, 5, 15};               // focus / nghi ngan / nghi dai (phut)
static const int POMO_MAX[3] = {90, 30, 60};
static unsigned long pomo_end_ms = 0, pomo_remaining_ms = 25 * 60000UL;
static int pomo_done = 0;

// =========================
// UTIL
// =========================
void log_print(lv_log_level_t level, const char* buf) {
  LV_UNUSED(level);
  Serial.println(buf);
}

static uint32_t lv_tick_millis() { return millis(); }

// Trang can PC: bat khi da co token (IP PC co the tu tim qua UDP beacon)
static bool claude_enabled() { return CLAUDE_ENABLED && netcfg.token[0] != '\0'; }

// Lay gio khong chan (getLocalTime mac dinh cho toi 5s neu chua dong bo NTP)
static bool get_time_now(struct tm* t) { return getLocalTime(t, 0); }

static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

// RAM malloc() dung duoc (DRAM 8-bit). ESP.getFreeHeap() cong ca ~36 KB IRAM chi doc/ghi 32-bit,
// LVGL / WiFi / String khong dung duoc -> so do cao gia, khong dung de canh bao.
static uint32_t dram_free() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
static uint32_t dram_min() { return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT); }
static uint32_t dram_largest() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

// Thu trong tuan tu ngay duong lich (0 = Chu nhat)
static int weekday_of(int y, int m, int d) { return (int)((lunar_jd(d, m, y) + 1) % 7); }

static void fmt_mmss(int s, char* out, size_t n) { snprintf(out, n, "%d:%02d", max(s, 0) / 60, max(s, 0) % 60); }

static unsigned long pomo_duration(PomoPhase p) { return pomo_min[p] * 60000UL; }

// =========================
// BEEP (khong chan) qua cong loa GPIO26
// =========================
static const Tone BEEP_DONE[] = {{1568, 90}, {0, 60}, {2093, 140}, {0, 0}};
static const Tone BEEP_WAIT[] = {{1319, 120}, {0, 80}, {1319, 120}, {0, 80}, {1760, 200}, {0, 0}};
static const Tone BEEP_POMO[] = {{2093, 160}, {0, 110}, {2093, 160}, {0, 110}, {2637, 320}, {0, 0}};
static const Tone BEEP_ALERT[] = {{1760, 120}, {0, 60}, {1397, 200}, {0, 0}};
static const Tone* beep_seq = nullptr;
static int beep_idx = 0;
static unsigned long beep_next = 0;

static void beep(const Tone* seq) {
  if (!settings.beep) return;
  beep_seq = seq;
  beep_idx = 0;
  beep_next = 0;
}

static void beep_tick() {
  if (!beep_seq || millis() < beep_next) return;
  const Tone& t = beep_seq[beep_idx++];
  if (t.ms == 0) { ledcWriteTone(SPEAKER_PIN, 0); beep_seq = nullptr; return; }
  ledcWriteTone(SPEAKER_PIN, t.freq);
  beep_next = millis() + t.ms;
}

// =========================
// SETTINGS (NVS)
// =========================
// Cau hinh mang: NVS (trang Wi-Fi setup) > secrets.h > mac dinh
static void load_netcfg() {
  Preferences np;
  np.begin("net", true);
  strlcpy(netcfg.ssid, np.getString("ssid", WIFI_SSID).c_str(), sizeof(netcfg.ssid));
  strlcpy(netcfg.pass, np.getString("pass", WIFI_PASSWORD).c_str(), sizeof(netcfg.pass));
  strlcpy(netcfg.token, np.getString("token", CLAUDE_TOKEN).c_str(), sizeof(netcfg.token));
  strlcpy(netcfg.host, np.getString("host", CLAUDE_HOST).c_str(), sizeof(netcfg.host));
  netcfg.port = np.getUShort("port", CLAUDE_PORT);
  strlcpy(netcfg.lat, np.getString("lat", DEFAULT_LAT).c_str(), sizeof(netcfg.lat));
  strlcpy(netcfg.lon, np.getString("lon", DEFAULT_LON).c_str(), sizeof(netcfg.lon));
  strlcpy(netcfg.tz, np.getString("tz", DEFAULT_TZ).c_str(), sizeof(netcfg.tz));
  np.end();
}

static void load_settings() {
  settings.beep = prefs.getBool("beep", true);
  settings.notify = prefs.getUChar("notify", NOTIFY_AWAY);
  settings.slide_s = prefs.getUChar("slide", 20);
  settings.alerts = prefs.getBool("alerts", true);
  settings.screen = prefs.getUChar("scrmode", SCREEN_ALWAYS);
  slides_mask = prefs.getUShort("slides", DEFAULT_SLIDES);
  pomo_min[0] = prefs.getUChar("pf", 25);
  pomo_min[1] = prefs.getUChar("pb", 5);
  pomo_min[2] = prefs.getUChar("pl", 15);
  pomo_remaining_ms = pomo_duration(POMO_FOCUS);
}

static void save_settings() {
  prefs.putBool("beep", settings.beep);
  prefs.putUChar("notify", settings.notify);
  prefs.putUChar("slide", settings.slide_s);
  prefs.putBool("alerts", settings.alerts);
  prefs.putUChar("scrmode", settings.screen);
}

// =========================
// NAVIGATION: Home / tab co dinh / slideshow
// =========================
static bool screen_enabled(ScreenState s) {
  if ((s == SCR_CLAUDE || s == SCR_ACCOUNT || s == SCR_PC || s == SCR_MUSIC) && !claude_enabled()) return false;
  return s != SCR_HOME && s != SCR_SETTINGS && s != SCR_DEVICE && s != SCR_SETUP;
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

// Settings / Device it khi mo: tao luc mo, xoa luc roi (giu ~15 KB RAM cho WiFi + ve)
static bool screen_is_temporary(ScreenState s) { return s == SCR_SETTINGS || s == SCR_DEVICE; }

static void show_screen(ScreenState s) {
  if (!screens[s]) {
    if (s == SCR_SETTINGS) create_settings_screen();
    else if (s == SCR_DEVICE) create_device_screen();
    else return;   // che do Wi-Fi setup chi co man setup
  }
  ScreenState prev = current_screen;
  current_screen = s;
  lv_screen_load(screens[s]);   // doi trang tuc thi (khong hieu ung -> khong giat)
  if (prev != s && screen_is_temporary(prev) && screens[prev]) {
    lv_obj_delete_async(screens[prev]);   // async: co the dang o trong su kien cham cua chinh man do
    screens[prev] = nullptr;
    if (prev == SCR_SETTINGS) memset(set_btn, 0, sizeof(set_btn));
    else { memset(dv_val, 0, sizeof(dv_val)); dv_uptime = nullptr; }
  }
  last_screen_switch = millis();
  if (s == SCR_CLAUDE || s == SCR_ACCOUNT || s == SCR_PC || s == SCR_MUSIC) claude_poll_now = true;
  if (s == SCR_DEVICE) update_device_display();
}

static void go_home() {
  nav_mode = MODE_HOME;
  show_screen(SCR_HOME);
  save_nav();
}

static void open_tab(ScreenState s) {
  nav_mode = MODE_FIXED;
  show_screen(s);
  if (screen_enabled(s)) save_nav();   // Settings / Device / Wi-Fi setup khong nho qua lan khoi dong
}

static void start_slideshow() {
  if (!slides_mask) slides_mask = DEFAULT_SLIDES;
  nav_mode = MODE_SLIDESHOW;
  show_screen(next_slide(current_screen == SCR_HOME ? (ScreenState)(SCREEN_COUNT - 1) : current_screen));
  save_nav();
}

// =========================
// 24/7 GUARD: watchdog loop + task mang + bo nho
// =========================
static void guard_tick() {
  static unsigned long last_check = 0;
  unsigned long now = millis();
  if (now - last_check < HEAP_CHECK_INTERVAL) return;
  last_check = now;

  if (net_heartbeat && now - net_heartbeat > NET_STALL_RESTART_MS) {
    Serial.println("Network task stalled -> restart");
    delay(100);
    ESP.restart();
  }
  uint32_t largest = dram_largest();
  if (largest < HEAP_CRITICAL_BYTES) {
    Serial.printf("Heap critical (%u) -> restart\n", (unsigned)largest);
    delay(100);
    ESP.restart();
  }
  struct tm t;
  if (largest < HEAP_LOW_BYTES && get_time_now(&t) && t.tm_hour == 4 && t.tm_min == 0) {
    Serial.printf("Heap low (%u) -> nightly restart\n", (unsigned)largest);
    delay(100);
    ESP.restart();
  }
  static int n = 0;
  if (++n % 10 == 0) {  // log moi 10 phut
    Serial.printf("RAM: %u free, largest %u, lowest %u\n", (unsigned)dram_free(), (unsigned)largest, (unsigned)dram_min());
  }
}

// =========================
// DEN NEN: SCREEN = WITH PC -> tat khi PC khoa man hinh, ngu hoac tat (mat ket noi 2 phut)
// =========================
static void set_backlight(bool on) {
  if (on == backlight_on) return;
  backlight_on = on;
  digitalWrite(TFT_BL_PIN, on ? HIGH : LOW);
}

static void wake_screen(unsigned long ms) {
  wake_until = millis() + ms;
  set_backlight(true);
}

static void backlight_tick(bool pc_locked) {
  unsigned long now = millis();
  bool pc_here = pc_seen_ms && now - pc_seen_ms < SCREEN_OFF_AFTER_MS && !pc_locked;
  bool want = settings.screen == SCREEN_ALWAYS || portal_active || !claude_enabled() || pc_here ||
              (long)(wake_until - now) > 0;
  set_backlight(want);
}

// =========================
// DO HIEU NANG VE: thoi gian ve 1 khung (RENDER_START -> RENDER_READY)
// =========================
static void render_event_cb(lv_event_t* e) {
  static uint32_t t0 = 0, sum = 0, n = 0, mx = 0, window_start = 0;
  uint32_t now = micros();
  if (lv_event_get_code(e) == LV_EVENT_RENDER_START) { t0 = now; return; }
  uint32_t us = now - t0;
  sum += us; n++; mx = max(mx, us);
  dev.frames++;
  if (millis() - window_start >= 5000) {  // trung binh / lon nhat trong 5s gan nhat
    dev.frame_us_avg = n ? sum / n : 0;
    dev.frame_us_max = mx;
    sum = n = mx = 0;
    window_start = millis();
  }
}

// =========================
// LENH QUA CONG USB (115200): chup man hinh cho README, doi tab, so lieu
//   SHOT   -> "SHOT 320 240 921600", doi toc do 921600, gui 320x240 RGB565 tung dong, roi ve 115200
//   TAB n  -> mo trang n (ScreenState), tra "OK"
//   STATS  -> 1 dong JSON so lieu suc khoe
//   BENCH  -> thoi gian ve lai ca man cua 7 tab (do anh huong cua DRAW_BUF_SIZE, style, font)
//   SETUP  -> khoi dong lai vao trang Wi-Fi setup     RESTART -> khoi dong lai
// tools/cyd_capture.py dung cac lenh nay
// =========================
static void capture_screen(lv_display_t* disp) {
  TFT_eSPI* tft = *(TFT_eSPI**)lv_display_get_driver_data(disp);  // lv_tft_espi_t { TFT_eSPI* tft; }
  static uint16_t line[SCREEN_HEIGHT];
  Serial.printf("SHOT %d %d 921600\n", SCREEN_HEIGHT, SCREEN_WIDTH);
  Serial.flush();
  delay(50);
  Serial.updateBaudRate(921600);
  delay(300);
  for (int y = 0; y < SCREEN_WIDTH; y++) {
    tft->readRect(0, y, SCREEN_HEIGHT, 1, line);
    Serial.write((const uint8_t*)line, sizeof(line));
    if (y % 16 == 0) feedLoopWDT();
  }
  Serial.flush();
  delay(100);
  Serial.updateBaudRate(115200);
}

static void serial_tick(lv_display_t* disp) {
  static char cmd[24];
  static int len = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch != '\n' && ch != '\r') {
      if (len < (int)sizeof(cmd) - 1) cmd[len++] = ch;
      continue;
    }
    if (!len) continue;
    cmd[len] = '\0';
    len = 0;
    int n;
    if (strcmp(cmd, "SHOT") == 0) {
      lv_refr_now(disp);   // ve xong moi thu dang cho truoc khi doc lai man hinh
      capture_screen(disp);
    } else if (sscanf(cmd, "TAB %d", &n) == 1 && n >= 0 && n < SCREEN_COUNT && !portal_active) {
      if (n == SCR_HOME) go_home();
      else open_tab((ScreenState)n);
      lv_refr_now(disp);
      Serial.println("OK");
    } else if (strcmp(cmd, "BENCH") == 0 && !portal_active) {   // ve lai ca man 5 lan moi tab, in thoi gian
      ScreenState back = current_screen;
      const ScreenState tabs[] = {SCR_HOME, SCR_CLOCK, SCR_WEATHER, SCR_FORECAST, SCR_CLAUDE, SCR_MUSIC, SCR_POMO};
      uint32_t total = 0;
      for (ScreenState t : tabs) {
        show_screen(t);
        lv_refr_now(disp);
        uint32_t t0 = micros();
        for (int i = 0; i < 5; i++) { lv_obj_invalidate(screens[t]); lv_refr_now(disp); feedLoopWDT(); }
        uint32_t us = (micros() - t0) / 5;
        total += us;
        Serial.printf("BENCH %d %.1f ms\n", t, us / 1000.0);
      }
      Serial.printf("BENCH avg %.1f ms (buf %u B)\n", total / 7000.0, (unsigned)sizeof(draw_buf));
      show_screen(back);
    } else if (strcmp(cmd, "SETUP") == 0) {
      request_portal_and_restart();
    } else if (strcmp(cmd, "RESTART") == 0) {
      ESP.restart();
    } else if (strcmp(cmd, "STATS") == 0) {
      // heap / heap_min / largest: RAM malloc dung duoc (DRAM). iram: phan chi 32-bit (truoc day bi cong vao heap)
      Serial.printf("{\"fw\":\"%s\",\"uptime_s\":%lu,\"heap\":%u,\"heap_min\":%u,\"largest\":%u,\"iram\":%u,"
                    "\"rssi\":%d,\"wifi_drops\":%u,\"frame_ms_avg\":%.1f,\"frame_ms_max\":%.1f,\"server_ms\":%u,"
                    "\"server_fail\":%u,\"weather_ms\":%u,\"weather_fail\":%u,\"net_stack_free\":%u}\n",
                    FW_VERSION, millis() / 1000, (unsigned)dram_free(), (unsigned)dram_min(), (unsigned)dram_largest(),
                    (unsigned)(ESP.getFreeHeap() - dram_free()), WiFi.RSSI(), (unsigned)dev.wifi_drops,
                    dev.frame_us_avg / 1000.0, dev.frame_us_max / 1000.0, (unsigned)dev.server_ms,
                    (unsigned)dev.server_fail, (unsigned)dev.weather_ms, (unsigned)dev.weather_fail,
                    (unsigned)dev.net_stack_free);
    }
  }
}

// =========================
// SETUP / LOOP
// =========================
void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32 CYD...");
  data_mutex = xSemaphoreCreateMutex();
  prefs.begin("cyd", false);
  load_settings();
  load_netcfg();

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

  // === Anh bia album: mo ta anh RGB565 80x80 (bo dem cap phat khi o tab Music) ===
  for (int i = 0; i < 2; i++) {
    memset(&art_dsc[i], 0, sizeof(art_dsc[i]));
    art_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
    art_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
    art_dsc[i].header.w = ART_SIZE;
    art_dsc[i].header.h = ART_SIZE;
    art_dsc[i].header.stride = ART_SIZE * 2;
    art_dsc[i].data_size = ART_BYTES;
  }

  // === Khoi tao LVGL + man hinh ===
  lv_init();
  lv_tick_set_cb(lv_tick_millis);   // tick chinh xac theo millis()
  lv_log_register_print_cb(log_print);
  lv_display_t* disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);   // lat lai man (doi 270 -> 90)
  lv_display_add_event_cb(disp, render_event_cb, LV_EVENT_RENDER_START, NULL);
  lv_display_add_event_cb(disp, render_event_cb, LV_EVENT_RENDER_READY, NULL);
  main_disp = disp;
  digitalWrite(TFT_BL_PIN, HIGH);                          // dam bao van sang toi da sau khi driver khoi tao

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);
  lv_indev_set_long_press_time(indev, LONG_PRESS_MS);

  portal_active = !netcfg.ssid[0] || portal_requested();  // chua co WiFi (ban build chung) / bam WI-FI SETUP
  create_all_screens();   // che do Wi-Fi setup: chi man setup (WiFi phat + web server can RAM)
  if (!portal_active) pomo_update_display();

  // Khoi phuc che do + tab truoc khi mat dien
  nav_mode = (NavMode)prefs.getUChar("mode", MODE_SLIDESHOW);
  uint8_t s = prefs.getUChar("screen", SCR_CLOCK);
  ScreenState start = (s < SCREEN_COUNT && screen_enabled((ScreenState)s)) ? (ScreenState)s : SCR_CLOCK;
  if (nav_mode == MODE_HOME) start = SCR_HOME;
  if (nav_mode == MODE_SLIDESHOW && !in_slideshow(start)) start = next_slide(start);
  if (portal_active) start = SCR_SETUP;
  show_screen(start);
  lv_timer_handler();

  // LVGL dung heap chung cua ESP32 (LV_STDLIB_CLIB trong lv_conf.h)
  Serial.printf("RAM after UI: %u bytes free, largest block %u\n", (unsigned)dram_free(), (unsigned)dram_largest());

  // === WiFi khong chan: mang chay o task rieng, UI van chay khi mat mang ===
  if (portal_active) {
    xTaskCreatePinnedToCore(portal_task, "portal", 8192, NULL, 1, NULL, 0);
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(netcfg.ssid, netcfg.pass);
    xTaskCreatePinnedToCore(network_task, "net", 8192, NULL, 1, NULL, 0);   // dung toi da ~4.7 KB (STATS net_stack_free)
  }

  enableLoopWDT();   // loop() treo > 5s -> chip tu khoi dong lai
  Serial.println("Setup completed!");
}

// Che do Wi-Fi setup: chi man huong dan (khong co tab nao khac de cap nhat)
static void portal_loop() {
  static unsigned long last = 0;
  if (millis() - last >= DISPLAY_UPDATE_INTERVAL) {
    update_setup_display();
    last = millis();
  }
  serial_tick(main_disp);
  guard_tick();
  backlight_tick(false);
  lv_timer_handler();
  delay(5);
}

void loop() {
  if (portal_active) { portal_loop(); return; }
  unsigned long now = millis();

  if (nav_mode == MODE_SLIDESHOW && now - last_screen_switch >= settings.slide_s * 1000UL) {
    show_screen(next_slide(current_screen));
  }

  if (now - last_display_update >= DISPLAY_UPDATE_INTERVAL) {
    update_clock_display();
    pomo_update_display();
    if (current_screen == SCR_MUSIC) update_music_progress();
    if (current_screen == SCR_DEVICE) update_device_display();
    release_album_art_tick();
    last_display_update = now;
  }
  update_weather_display();
  update_claude_display();
  update_album_art();
  update_permission_card();
  serial_tick(main_disp);
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
  guard_tick();
  backlight_tick(ui_pc_locked);

  lv_timer_handler();
  delay(5);
}
