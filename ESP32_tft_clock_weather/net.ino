// =========================
// NETWORK (task rieng tren core 0, khong bao gio goi LVGL o day)
// WiFi, thoi tiet + AQI (Open-Meteo), PC server (Claude, PC, nhac, anh bia), UDP beacon
// =========================

static bool http_get_json(const String& url, JsonDocument& doc, const char* bearer, int timeout_ms) {
  HTTPClient http;
  http.setConnectTimeout(timeout_ms);
  http.setTimeout(timeout_ms);
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
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + netcfg.lat + "&longitude=" + netcfg.lon +
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day,wind_speed_10m" +
               "&hourly=temperature_2m,precipitation_probability,weather_code,is_day&forecast_hours=8" +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&forecast_days=3" +
               "&temperature_unit=" + TEMP_UNIT_PARAM + "&timezone=auto";
  JsonDocument doc;
  uint32_t t0 = millis();
  bool got = http_get_json(url, doc, nullptr, HTTP_TIMEOUT_MS);
  dev.weather_ms = millis() - t0;
  if (!got) { dev.weather_fail++; return false; }
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
  String url = String("http://air-quality-api.open-meteo.com/v1/air-quality?latitude=") + netcfg.lat +
               "&longitude=" + netcfg.lon + "&current=us_aqi,pm2_5&timezone=auto";
  JsonDocument doc;
  if (!http_get_json(url, doc, nullptr, AQI_TIMEOUT_MS)) return false;
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

// Dia chi server hien tai: tu trang Wi-Fi setup / secrets.h, cap nhat khi nghe thay beacon (chi task mang dung)
static char claude_host[40];
static uint16_t claude_port = 7842;
static WiFiUDP beacon_udp;
static bool beacon_listening = false;

static String server_url(const char* path) { return String("http://") + claude_host + ":" + claude_port + path; }

// UDP tu PC: "CLAUDE_USAGE 1 <port>" (tim IP), "CLAUDE_EVENT <id>" (Claude Code vua xong / dang cho / xin quyen)
// hoac "CLAUDE_DIRTY" (so lieu doi: bai hat, % usage...). Tra ve true neu can lay so lieu ngay
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
    } else if ((sscanf(msg, "CLAUDE_EVENT %d", &ev) == 1 || strncmp(msg, "CLAUDE_DIRTY", 12) == 0) &&
               ip == claude_host) {
      poll = true;  // co thay doi -> lay ngay, khong doi lan poll ke tiep
    }
  }
  return poll;
}

// Giao thuc claudometer: GET /v1/stats, Bearer token, schema 1 (+ event, context, account, pc, media)
static bool fetch_claude(ScreenState scr) {
  if (!claude_host[0]) return false;  // chua biet IP PC: doi beacon UDP
  String screens = "home,pc";  // pc luon kem: o System tren Home co so ngay tu luc khoi dong
  if (scr == SCR_MUSIC) screens += ",media";
  JsonDocument doc;
  uint32_t t0 = millis();
  bool ok = http_get_json(server_url("/v1/stats?screens=") + screens, doc, netcfg.token, HTTP_TIMEOUT_MS) &&
            (doc["schema"] | 0) == 1;
  dev.server_ms = millis() - t0;
  if (!ok) {
    dev.server_fail++;
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    if (!shared_claude.stale) { shared_claude.stale = true; claude_dirty = true; }
    xSemaphoreGive(data_mutex);
    return false;
  }

  static ClaudeData c;  // static: struct ~1 KB, tranh day stack task mang
  c = ClaudeData();
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
    c.event_idle = ev["idle_s"] | 0;
    c.event_duration = ev["duration_s"] | -1;
    strlcpy(c.event_type, ev["type"] | "", sizeof(c.event_type));
    strlcpy(c.event_project, ev["project"] | "", sizeof(c.event_project));
    strlcpy(c.event_msg, ev["message"] | "", sizeof(c.event_msg));
  }

  JsonObjectConst pm = doc["permission"];
  if (!pm.isNull()) {
    c.perm.id = pm["id"] | 0;
    c.perm.expires_s = pm["expires_in"] | 0;
    strlcpy(c.perm.tool, pm["tool"] | "", sizeof(c.perm.tool));
    strlcpy(c.perm.project, pm["project"] | "", sizeof(c.perm.project));
    strlcpy(c.perm.detail, pm["detail"] | "", sizeof(c.perm.detail));
  }

  JsonObjectConst cal = doc["calendar"];
  if (!cal.isNull()) {
    c.cal.has = true;
    strlcpy(c.cal.when, cal["when"] | "", sizeof(c.cal.when));
    strlcpy(c.cal.start, cal["start"] | "", sizeof(c.cal.start));
    strlcpy(c.cal.title, cal["title"] | "", sizeof(c.cal.title));
  }
  c.pc_locked = doc["pc_locked"] | false;

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
      strlcpy(c.media.art_id, md["art_id"] | "", sizeof(c.media.art_id));
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

// Anh bia: tai 80x80 RGB565 (12.8 KB) vao bo dem khong dang hien thi, roi bao UI doi
// Bo goc anh bia: pha 4 goc vao mau nen (khu rang cua theo do phu cua cung tron).
// Lam mot lan tren task mang khi tai anh -> LVGL chi ve anh thuong, khong can clip_corner (ton RAM/CPU).
static void round_art_corners(uint16_t* px) {
  const lv_color_t bg = lv_color_hex(C_BG);
  const float r = ART_RADIUS;
  for (int y = 0; y < ART_RADIUS; y++) {
    for (int x = 0; x < ART_RADIUS; x++) {
      // do phu: khoang cach tu tam pixel toi tam cung tron (r, r)
      float dx = r - (x + 0.5f), dy = r - (y + 0.5f);
      float d = sqrtf(dx * dx + dy * dy);
      float cover = r + 0.5f - d;  // >=1: trong anh, <=0: ngoai -> mau nen
      if (cover >= 1.0f) continue;
      uint8_t a = cover <= 0 ? 0 : (uint8_t)(cover * 255);
      const int xs[2] = {x, ART_SIZE - 1 - x}, ys[2] = {y, ART_SIZE - 1 - y};
      for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
          uint16_t& p = px[ys[j] * ART_SIZE + xs[i]];
          lv_color_t c = lv_color_mix(lv_color_make((p >> 8) & 0xF8, (p >> 3) & 0xFC, (p << 3) & 0xF8), bg, a);
          p = lv_color_to_u16(c);
        }
      }
    }
  }
}

static void fetch_album_art() {
  char want[12];
  xSemaphoreTake(data_mutex, portMAX_DELAY);
  strlcpy(want, shared_claude.media.art_id, sizeof(want));
  xSemaphoreGive(data_mutex);
  if (art_dirty || strcmp(want, art_loaded_id) == 0) return;  // UI chua nhan anh truoc, hoac anh khong doi
  if (!want[0]) {                                             // bai moi khong co anh bia
    art_front = -1;
    art_loaded_id[0] = '\0';
    art_dirty = true;
    return;
  }
  int back = art_front == 0 ? 1 : 0;
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(server_url("/v1/media/art"))) return;
  http.addHeader("Authorization", String("Bearer ") + netcfg.token);
  const size_t need = sizeof(art_buf[0]);
  bool ok = false;
  if (http.GET() == HTTP_CODE_OK && http.getSize() == (int)need) {
    WiFiClient* st = http.getStreamPtr();
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < need && millis() - t0 < (uint32_t)HTTP_TIMEOUT_MS) {
      int n = st->available();
      if (n > 0) got += st->readBytes(art_buf[back] + got, min((size_t)n, need - got));
      else vTaskDelay(pdMS_TO_TICKS(5));
    }
    ok = got == need;
  }
  http.end();
  if (!ok) return;
  round_art_corners((uint16_t*)art_buf[back]);
  strlcpy(art_loaded_id, want, sizeof(art_loaded_id));
  art_front = back;
  art_dirty = true;
}

void network_task(void*) {
  uint32_t next_wifi_retry = millis() + WIFI_RETRY_INTERVAL;
  uint32_t next_weather = 0, next_aqi = 0, next_claude = 0;
  uint32_t claude_backoff = CLAUDE_BACKOFF_START;
  int aqi_fails = 0;
  strlcpy(claude_host, netcfg.host, sizeof(claude_host));
  claude_port = netcfg.port;
  bool was_connected = false;

  for (;;) {
    uint32_t now = millis();
    net_heartbeat = now;

    if (WiFi.status() != WL_CONNECTED) {
      if (was_connected) { dev.wifi_drops++; was_connected = false; }
      if (beacon_listening) { beacon_udp.stop(); beacon_listening = false; }
      if (due(now, next_wifi_retry)) {
        Serial.println("WiFi disconnected, reconnecting...");
        WiFi.reconnect();
        next_wifi_retry = now + WIFI_RETRY_INTERVAL;
    was_connected = true;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    next_wifi_retry = now + WIFI_RETRY_INTERVAL;

    // NTP chi bat sau khi WiFi len (goi som lam lwIP assert). Va dua san IP cho SNTP: neu SNTP tu tra DNS
    // dung luc HTTPClient tra DNS thi core 3.x assert "sys_untimeout ... lock TCPIP core" va khoi dong lai.
    static bool ntp_started = false;
    if (!ntp_started) {
      static char ntp_a[16], ntp_b[16];   // SNTP giu con tro, khong chep chuoi
      IPAddress a, b;
      bool ok_a = WiFi.hostByName(NTP_SERVER, a) == 1;
      bool ok_b = WiFi.hostByName("time.google.com", b) == 1;
      if (ok_a || ok_b) {
        strlcpy(ntp_a, (ok_a ? a : b).toString().c_str(), sizeof(ntp_a));
        strlcpy(ntp_b, (ok_b ? b : a).toString().c_str(), sizeof(ntp_b));
        configTzTime(netcfg.tz, ntp_a, ntp_b);
        ntp_started = true;
      }
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
        http_post_json(server_url("/v1/media"), String("{\"action\":\"") + name + "\"}", netcfg.token);
        vTaskDelay(pdMS_TO_TICKS(300));  // de app nhac kip doi bai / trang thai
        next_claude = millis();
      }
      int pid = perm_answer_id;
      if (pid) {  // the xin quyen: gui Allow / Deny cho PC (hook Claude Code dang cho)
        const char* d = perm_answer == 1 ? "allow" : "deny";
        http_post_json(server_url("/v1/permission"), String("{\"id\":") + pid + ",\"decision\":\"" + d + "\"}",
                       netcfg.token);
        perm_answer_id = 0;
        next_claude = millis();
      }
      if (claude_poll_now) { claude_poll_now = false; next_claude = now; }
      if (due(millis(), next_claude)) {
        ScreenState scr = current_screen;
        if (fetch_claude(scr)) {
          claude_backoff = CLAUDE_BACKOFF_START;
          bool perm_pending;
          xSemaphoreTake(data_mutex, portMAX_DELAY);
          perm_pending = shared_claude.perm.id != 0;
          xSemaphoreGive(data_mutex);
          uint32_t iv = perm_pending ? PERM_POLL_ACTIVE : scr == SCR_MUSIC ? MEDIA_POLL_ACTIVE
                      : scr == SCR_PC ? PC_POLL_ACTIVE
                      : (scr == SCR_CLAUDE || scr == SCR_ACCOUNT) ? CLAUDE_POLL_ACTIVE : CLAUDE_POLL_IDLE;
          next_claude = millis() + iv;
          if (scr == SCR_MUSIC) fetch_album_art();
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
      if (fetch_aqi()) {
        aqi_fails = 0;
        next_aqi = millis() + AQI_UPDATE_INTERVAL;
      } else {  // lan loi dau thu lai sau 10s, sau do 60s
        next_aqi = millis() + (aqi_fails++ == 0 ? AQI_QUICK_RETRY : NET_RETRY_INTERVAL);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
