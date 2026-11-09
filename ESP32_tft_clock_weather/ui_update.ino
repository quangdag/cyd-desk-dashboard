// =========================
// UI UPDATE: dua du lieu len man hinh, thong bao, canh bao, Pomodoro
// =========================

// ---- Van ban ----
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

static bool is_rain_code(int code) {
  return (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95;
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

static void set_pct_bar(lv_obj_t* bar, int pct) {
  pct = constrain(pct, 0, 100);
  lv_bar_set_value(bar, pct, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, lv_color_hex(pct >= 85 ? C_ALERT : C_ACCENT), LV_PART_INDICATOR);
}

// =========================
// Clock + Weather + Forecast
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

// Bao mua: troi dang kho ma 1-2 gio toi kha nang mua >= 70% -> 1 the / 1 dot mua
static void check_rain_alert(const WeatherData& w) {
  static char alerted[12] = "";
  if (!settings.alerts || !w.valid || is_rain_code(w.weather_code)) return;
  struct tm t;
  if (!get_time_now(&t)) return;
  for (int i = 0; i < min(w.n_hours, 2); i++) {
    if (w.hours[i].rain < RAIN_ALERT_PCT) continue;
    char key[12];
    snprintf(key, sizeof(key), "%02d-%s", t.tm_mday, w.hours[i].hour);
    if (strcmp(key, alerted) == 0) return;
    strlcpy(alerted, key, sizeof(alerted));
    char title_buf[48], sub[64];
    snprintf(title_buf, sizeof(title_buf), "Rain likely at %s", w.hours[i].hour);
    snprintf(sub, sizeof(sub), "%d%% chance | bring an umbrella", w.hours[i].rain);
    show_toast(title_buf, sub, C_BLUE);
    beep(BEEP_ALERT);
    return;
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
    check_rain_alert(w);
  }
  update_clock_weather_line();
}

// =========================
// System
// =========================
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
  if (p.cpu_temp >= 0) snprintf(buf, sizeof(buf), "%d°C", p.cpu_temp);
  else if (p.cpu >= 0) snprintf(buf, sizeof(buf), "%d%%", p.cpu);
  else snprintf(buf, sizeof(buf), "--");
  lv_label_set_text(home_val[SCR_PC], buf);

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

// =========================
// Music
// =========================
// Noi suy vi tri moi giay khi dang phat (server chi hoi moi 2s)
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

// Anh bia: task mang da ghi vao bo dem art_front -> hien len (hoac an neu bai khong co anh)
void update_album_art() {
  if (!art_dirty) return;
  int f = art_front;
  if (f >= 0) {
    lv_image_set_src(mu_art, &art_dsc[f]);   // anh RGB565 trong RAM ve truc tiep (cache anh dang tat)
    lv_obj_invalidate(mu_art);               // ve lai du cung con tro (2 bo dem dung luan phien)
    lv_obj_remove_flag(mu_art, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(mu_art, LV_OBJ_FLAG_HIDDEN);
  }
  art_dirty = false;
}

// =========================
// Claude: Code + Account + thong bao + canh bao han muc
// =========================
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

// Thong bao Claude Code. Che do AWAY: im lang khi ban dang ngoi may va luot Claude ngan.
// "Can cap quyen" luon bao vi Claude dang dung cho ban.
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
    return;
  }
  bool away = c.event_idle >= NOTIFY_AWAY_IDLE_S || c.event_duration >= NOTIFY_AWAY_TASK_S;
  if (settings.notify == NOTIFY_AWAY && !away) return;
  if (c.event_duration >= 60) snprintf(sub, sizeof(sub), "%s | took %dm %02ds", c.event_project, c.event_duration / 60, c.event_duration % 60);
  else snprintf(sub, sizeof(sub), "%s | tap to dismiss", c.event_project);
  show_toast(LV_SYMBOL_OK " Claude is done", sub, C_OK);
  beep(BEEP_DONE);
}

// Canh bao han muc: phien 80% / 95%, tuan 80%, va "phien moi" khi han muc duoc lam moi
static void check_quota_alerts(const ClaudeData& c) {
  static int last_session = -1;
  static char last_resets[8] = "";
  static bool warned80 = false, warned95 = false, warned_week = false;
  if (!c.exact && !c.valid) return;
  if (last_session < 0) {  // lan dau sau khi bat may: chi ghi nhan, khong bao
    warned80 = c.session_pct >= 80;
    warned95 = c.session_pct >= 95;
    warned_week = c.week_pct >= 80;
    last_session = c.session_pct;
    strlcpy(last_resets, c.resets_at, sizeof(last_resets));
    return;
  }
  char sub[64];
  if (c.resets_at[0] && strcmp(c.resets_at, last_resets) != 0 && c.session_pct + 20 < last_session) {
    warned80 = warned95 = false;
    if (settings.alerts) {
      snprintf(sub, sizeof(sub), "Session reset | %d%% used", c.session_pct);
      show_toast("Fresh Claude quota", sub, C_OK);
      beep(BEEP_DONE);
    }
  } else if (c.session_pct >= 95 && !warned95) {
    warned80 = warned95 = true;
    if (settings.alerts) {
      snprintf(sub, sizeof(sub), "resets %s", c.resets_at);
      show_toast("Session 95% used", sub, C_ALERT);
      beep(BEEP_ALERT);
    }
  } else if (c.session_pct >= 80 && !warned80) {
    warned80 = true;
    if (settings.alerts) {
      snprintf(sub, sizeof(sub), "resets %s | pace yourself", c.resets_at);
      show_toast("Session 80% used", sub, C_WARN);
      beep(BEEP_ALERT);
    }
  } else if (c.week_pct >= 80 && !warned_week) {
    warned_week = true;
    if (settings.alerts) {
      snprintf(sub, sizeof(sub), "resets in %s", c.week_resets_in);
      show_toast("Week 80% used", sub, C_WARN);
      beep(BEEP_ALERT);
    }
  }
  if (c.week_pct < 80) warned_week = false;
  last_session = c.session_pct;
  strlcpy(last_resets, c.resets_at, sizeof(last_resets));
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
  if (!c.stale) {
    handle_claude_event(c);
    check_quota_alerts(c);
  }
}

// =========================
// Pomodoro
// =========================
static void pomo_update_display() {
  static const char* const phases[] = {"Focus", "Short break", "Long break"};
  unsigned long left = pomo_running ? (long)(pomo_end_ms - millis()) > 0 ? pomo_end_ms - millis() : 0 : pomo_remaining_ms;
  unsigned long secs = (left + 999) / 1000;
  char buf[48];
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
