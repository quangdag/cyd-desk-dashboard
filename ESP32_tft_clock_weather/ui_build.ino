// =========================
// UI BUILD: cam ung, widget dung chung, tao tung man hinh, xu ly cham
// =========================

// ---- Cam ung ----
// Hieu chinh do tren chinh tam cam ung nay (cham 4 goc, 21 lan): gia tri tho XPT2046 o
// setRotation(2) tai 4 canh cua man ngang 320x240. Ngang (trai->phai) = raw y giam dan,
// doc (tren->duoi) = raw x tang dan.
#define TOUCH_RAW_LEFT   3712
#define TOUCH_RAW_RIGHT   499
#define TOUCH_RAW_TOP     514
#define TOUCH_RAW_BOTTOM 3428
static void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
  LV_UNUSED(indev);
  bool touched = touchscreen.tirqTouched() && touchscreen.touched();
  if (touched && !backlight_on) swallow_touch = true;  // den dang tat: cham chi de bat man, khong bam nham
  if (swallow_touch) {
    wake_screen(SCREEN_WAKE_MS);
    if (!touched) swallow_touch = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (touched) {
    wake_screen(SCREEN_WAKE_MS);
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

// ---- Thong bao ngan giua man (them/bot slideshow, ...) ----
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
  if (!screen_enabled(s)) return;
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

static void settings_open_cb(lv_event_t* e) {
  LV_UNUSED(e);
  open_tab(SCR_SETTINGS);
}

// ---- Widget dung chung ----
static void no_click(lv_obj_t* obj) { lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE); }

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

// Nut (tron hoac bo goc). user_data duoc truyen vao callback.
static lv_obj_t* make_btn(lv_obj_t* parent, int x, int y, int w, int h, int radius, uint32_t bg, uint32_t fg,
                          const char* text, const lv_font_t* font, lv_event_cb_t cb, intptr_t user, lv_obj_t** label_out) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_radius(b, radius, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg == C_ACCENT ? C_ACCENT_P : C_TILE_P), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)user);
  lv_obj_t* l = make_label(b, font, fg, text);
  lv_obj_center(l);
  if (label_out) *label_out = l;
  return b;
}

// =========================
// HOME: luoi 3x3. Moi o: so xem nhanh (serif) + ten in hoa; cham cam = dang co trong slideshow.
// =========================
void create_home_screen() {
  lv_obj_t* scr = screens[SCR_HOME] = make_screen();
  title(scr, "HOME");
  lv_obj_t* hint = make_caps(scr, C_MUTED, "HOLD = SLIDESHOW");
  lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -92, 10);
  home_clock = make_label(scr, &lv_font_montserrat_12, C_FG, "--:--");
  lv_obj_align(home_clock, LV_ALIGN_TOP_RIGHT, -46, 9);
  make_btn(scr, 280, 3, 32, 24, 8, C_TILE, C_FG, LV_SYMBOL_SETTINGS, &lv_font_montserrat_14, settings_open_cb, 0, NULL);

  const int order[9] = {SCR_CLOCK, SCR_WEATHER, SCR_FORECAST, SCR_CLAUDE, SCR_ACCOUNT, SCR_MUSIC, SCR_PC, SCR_POMO, SCREEN_COUNT};
  for (int i = 0; i < 9; i++) {
    int idx = order[i];
    bool slideshow = idx == SCREEN_COUNT;
    int x = 8 + (i % 3) * 103, y = 32 + (i / 3) * 69;
    lv_obj_t* t = lv_obj_create(scr);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 98, 63);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_radius(t, 10, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(slideshow ? C_ACCENT : C_TILE), 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(slideshow ? C_ACCENT_P : C_TILE_P), LV_STATE_PRESSED);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(t, tile_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(t, tile_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)idx);
    home_tile[idx] = t;

    home_val[idx] = make_label(t, &fr_num_24, slideshow ? C_BG : C_ACCENT, "--");
    lv_obj_set_pos(home_val[idx], 9, 6);
    // "CLAUDE CODE" (102px) khong vua o 98px -> tren Home ghi "CLAUDE"; tieu de trang van day du
    const char* label = slideshow ? "SLIDESHOW" : idx == SCR_CLAUDE ? "CLAUDE" : SCREEN_NAMES[idx];
    lv_obj_t* name = make_caps(t, slideshow ? C_BG : C_MUTED, label);
    lv_obj_set_pos(name, 9, 43);
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
  lv_obj_align(lb_clock_lunar, LV_ALIGN_CENTER, 0, 38);
  // Su kien lich sap toi (PC server doc file ICS); trong neu khong co
  lb_clock_event = make_label(scr, &mont_vi_14, C_FG, "");
  lv_obj_set_size(lb_clock_event, 292, lv_font_get_line_height(&mont_vi_14));  // 1 dong, dai qua thi "..."
  lv_obj_set_style_text_align(lb_clock_event, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lb_clock_event, LV_LABEL_LONG_DOT);
  lv_obj_align(lb_clock_event, LV_ALIGN_CENTER, 0, 62);
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

  // Anh bia 80x80 (khung giay + not nhac khi chua co anh)
  lv_obj_t* frame = lv_obj_create(scr);
  lv_obj_remove_style_all(frame);
  lv_obj_set_size(frame, ART_SIZE, ART_SIZE);
  lv_obj_set_pos(frame, 14, 32);
  lv_obj_set_style_radius(frame, ART_RADIUS, 0);
  lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(frame, lv_color_hex(C_TILE), 0);
  no_click(frame);
  lv_obj_t* note = make_label(frame, &lv_font_montserrat_24, C_MUTED, LV_SYMBOL_AUDIO);
  lv_obj_center(note);
  mu_art = lv_image_create(scr);
  lv_obj_set_pos(mu_art, 14, 32);
  lv_obj_add_flag(mu_art, LV_OBJ_FLAG_HIDDEN);
  no_click(mu_art);

  mu_title = make_label(scr, &mont_vi_20, C_FG, "Nothing playing");
  lv_obj_set_width(mu_title, 196);
  lv_label_set_long_mode(mu_title, LV_LABEL_LONG_SCROLL_CIRCULAR);  // ten dai -> chay chu
  lv_obj_set_pos(mu_title, 108, 38);  // font co them 2px tren dau cho dau tieng Thai
  mu_artist = make_label(scr, &mont_vi_14, C_MUTED, "Play something on Spotify or YouTube");
  lv_obj_set_width(mu_artist, 196);
  lv_label_set_long_mode(mu_artist, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(mu_artist, 108, 69);

  mu_bar = make_bar(scr, 14, 124, 292, 4);
  mu_pos = make_label(scr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(mu_pos, 14, 134);
  mu_dur = make_label(scr, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_align(mu_dur, LV_ALIGN_TOP_RIGHT, -14, 134);

  make_btn(scr, 76, 166, 48, 48, LV_RADIUS_CIRCLE, C_TILE, C_FG, LV_SYMBOL_PREV, &lv_font_montserrat_20, media_btn_cb, 3, NULL);
  make_btn(scr, 130, 158, 60, 60, LV_RADIUS_CIRCLE, C_ACCENT, C_BG, LV_SYMBOL_PLAY, &lv_font_montserrat_24, media_btn_cb, 1, &mu_play_icon);
  make_btn(scr, 196, 166, 48, 48, LV_RADIUS_CIRCLE, C_TILE, C_FG, LV_SYMBOL_NEXT, &lv_font_montserrat_20, media_btn_cb, 2, NULL);
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

// ---- Pomodoro ----
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
  make_btn(scr, 14, 66, 44, 44, LV_RADIUS_CIRCLE, C_TILE, C_FG, LV_SYMBOL_MINUS, &lv_font_montserrat_16, pomo_adjust_cb, -1, NULL);
  make_btn(scr, 262, 66, 44, 44, LV_RADIUS_CIRCLE, C_TILE, C_FG, LV_SYMBOL_PLUS, &lv_font_montserrat_16, pomo_adjust_cb, 1, NULL);

  po_bar = make_bar(scr, 40, 154, 240, 6);
  make_btn(scr, 40, 176, 140, 46, 12, C_ACCENT, C_BG, "Start", &lv_font_montserrat_16, pomo_start_pause_cb, 0, &po_btn_label);
  make_btn(scr, 192, 176, 88, 46, 12, C_TILE, C_FG, "Reset", &lv_font_montserrat_16, pomo_reset_cb, 0, NULL);
}

// ---- Settings: 5 hang lua chon (luu NVS) + nut Device info / Wi-Fi setup ----
#define SET_ROW_COUNT 5
static const char* const SET_ROWS[SET_ROW_COUNT] = {"SOUND", "NOTIFY", "SLIDESHOW", "ALERTS", "SCREEN"};
static const char* const SET_OPTS[SET_ROW_COUNT][4] = {
  {"ON", "OFF", nullptr, nullptr},
  {"ALWAYS", "AWAY", nullptr, nullptr},
  {"10S", "20S", "30S", "60S"},
  {"ON", "OFF", nullptr, nullptr},
  {"ALWAYS", "WITH PC", nullptr, nullptr},
};
static const uint8_t SLIDE_CHOICES[4] = {10, 20, 30, 60};

static int settings_selected(int row) {
  switch (row) {
    case 0: return settings.beep ? 0 : 1;
    case 1: return settings.notify == NOTIFY_ALWAYS ? 0 : 1;
    case 2: for (int i = 0; i < 4; i++) if (SLIDE_CHOICES[i] == settings.slide_s) return i; return 1;
    case 3: return settings.alerts ? 0 : 1;
    default: return settings.screen == SCREEN_ALWAYS ? 0 : 1;
  }
}

static void refresh_settings_buttons() {
  for (int r = 0; r < SET_ROW_COUNT; r++) {
    int sel = settings_selected(r);
    for (int o = 0; o < 4; o++) {
      lv_obj_t* b = set_btn[r][o];
      if (!b) continue;
      bool on = o == sel;
      lv_obj_set_style_bg_color(b, lv_color_hex(on ? C_ACCENT : C_TILE), 0);
      lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(on ? C_BG : C_FG), 0);
    }
  }
}

static void settings_cb(lv_event_t* e) {
  int v = (int)(intptr_t)lv_event_get_user_data(e);
  int row = v / 8, opt = v % 8;
  switch (row) {
    case 0: settings.beep = opt == 0; break;
    case 1: settings.notify = opt == 0 ? NOTIFY_ALWAYS : NOTIFY_AWAY; break;
    case 2: settings.slide_s = SLIDE_CHOICES[opt]; break;
    case 3: settings.alerts = opt == 0; break;
    default: settings.screen = opt == 0 ? SCREEN_ALWAYS : SCREEN_WITH_PC; wake_screen(SCREEN_WAKE_MS); break;
  }
  save_settings();
  refresh_settings_buttons();
  if (row == 0 && settings.beep) beep(BEEP_DONE);  // nghe thu
}

static void device_open_cb(lv_event_t* e) {
  LV_UNUSED(e);
  open_tab(SCR_DEVICE);
}

static void wifi_setup_cb(lv_event_t* e) {
  LV_UNUSED(e);
  request_portal_and_restart();
}

void create_settings_screen() {
  lv_obj_t* scr = screens[SCR_SETTINGS] = make_screen();
  title(scr, "SETTINGS");
  lv_obj_t* hint = make_caps(scr, C_MUTED, "TAP EMPTY SPACE = HOME");
  lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -14, 10);
  for (int r = 0; r < SET_ROW_COUNT; r++) {
    int y = 30 + r * 34;
    lv_obj_t* l = make_caps(scr, C_FG, SET_ROWS[r]);
    lv_obj_set_pos(l, 14, y + 8);
    int n = SET_OPTS[r][2] ? 4 : 2;
    int w = n == 4 ? 46 : 74, gap = 6;
    int x = 306 - n * w - (n - 1) * gap;
    for (int o = 0; o < n; o++) {
      set_btn[r][o] = make_btn(scr, x + o * (w + gap), y, w, 28, 8, C_TILE, C_FG, SET_OPTS[r][o],
                               &lv_font_montserrat_12, settings_cb, r * 8 + o, NULL);
    }
  }
  // Hai nut duoi cung: chua du khoang trong 10px voi mep duoi man
  make_btn(scr, 14, 200, 142, 30, 8, C_TILE, C_FG, "DEVICE INFO", &lv_font_montserrat_12, device_open_cb, 0, NULL);
  make_btn(scr, 164, 200, 142, 30, 8, C_TILE, C_FG, "WI-FI SETUP", &lv_font_montserrat_12, wifi_setup_cb, 0, NULL);
  refresh_settings_buttons();
}

// ---- Device: so lieu suc khoe (cap nhat moi giay khi dang mo) ----
static const char* const DEV_ROWS[8] = {"FRAME TIME", "FREE RAM", "LOWEST RAM", "WI-FI", "IP", "PC SERVER",
                                        "WEATHER API", "FIRMWARE"};
void create_device_screen() {
  lv_obj_t* scr = screens[SCR_DEVICE] = make_screen();
  title(scr, "DEVICE");
  dv_uptime = make_caps(scr, C_MUTED, "");
  lv_obj_align(dv_uptime, LV_ALIGN_TOP_RIGHT, -14, 10);
  for (int i = 0; i < 8; i++) {
    int y = 34 + i * 25;
    lv_obj_t* l = make_caps(scr, C_MUTED, DEV_ROWS[i]);
    lv_obj_set_pos(l, 14, y + 2);
    dv_val[i] = make_label(scr, &lv_font_montserrat_14, C_FG, "--");
    lv_obj_align(dv_val[i], LV_ALIGN_TOP_RIGHT, -14, y);
  }
}

// ---- Wi-Fi setup: huong dan noi dien thoai vao CYD ----
static void setup_cancel_cb(lv_event_t* e) {
  LV_UNUSED(e);
  ESP.restart();   // giu cau hinh cu
}

void create_setup_screen() {
  lv_obj_t* scr = screens[SCR_SETUP] = lv_obj_create(NULL);  // khong ve Home khi cham: dang cai dat
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  title(scr, "WI-FI SETUP");
  static const char* const steps[3] = {"On your phone, join Wi-Fi", "A setup page opens. If not, go to",
                                       "Enter your Wi-Fi and the PC token"};
  static const char* const big[2] = {"CYD-Setup", "192.168.4.1"};
  int y = 36;
  for (int i = 0; i < 3; i++) {
    char num[4];
    snprintf(num, sizeof(num), "%d", i + 1);
    lv_obj_t* n = make_caps(scr, C_MUTED, num);
    lv_obj_set_pos(n, 14, y + 2);
    lv_obj_t* t = make_label(scr, &lv_font_montserrat_14, C_FG, steps[i]);
    lv_obj_set_pos(t, 32, y);
    y += 22;
    if (i < 2) {
      lv_obj_t* b = make_label(scr, i == 0 ? &lv_font_montserrat_24 : &fr_num_24, i == 0 ? C_ACCENT : C_FG, big[i]);
      lv_obj_set_pos(b, 32, y);
      y += 36;
    }
  }
  su_status = make_caps(scr, C_MUTED, "");
  lv_obj_set_pos(su_status, 14, 212);
  if (netcfg.ssid[0]) {  // vao tu Settings: cho phep huy
    make_btn(scr, 226, 202, 80, 30, 8, C_TILE, C_FG, "CANCEL", &lv_font_montserrat_12, setup_cancel_cb, 0, NULL);
  }
}

// ---- Lop tren cung: thong bao ngan + the thong bao ----
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

// ---- The xin quyen Claude Code: Deny / Allow (chi hien khi ban vang mat, xem pc_server/permission.py) ----
static void perm_btn_cb(lv_event_t* e) {
  int allow = (int)(intptr_t)lv_event_get_user_data(e);
  if (!perm_shown_id) return;
  perm_answer = allow ? 1 : 2;
  perm_answer_id = perm_shown_id;     // task mang gui len PC
  flash(allow ? "ALLOWED" : "DENIED");
  lv_obj_add_flag(perm_card, LV_OBJ_FLAG_HIDDEN);
  perm_deadline = 0;
}

void create_permission_card() {
  perm_card = lv_obj_create(lv_layer_top());
  lv_obj_set_size(perm_card, 300, 220);
  lv_obj_center(perm_card);
  lv_obj_set_style_bg_color(perm_card, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(perm_card, 3, 0);
  lv_obj_set_style_border_color(perm_card, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_radius(perm_card, 14, 0);
  lv_obj_set_style_pad_all(perm_card, 12, 0);
  lv_obj_remove_flag(perm_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* t = make_caps(perm_card, C_ACCENT, "CLAUDE ASKS PERMISSION");
  lv_obj_set_pos(t, 0, 0);
  perm_count = make_caps(perm_card, C_MUTED, "");
  lv_obj_align(perm_count, LV_ALIGN_TOP_RIGHT, 0, 0);
  perm_where = make_label(perm_card, &mont_vi_14, C_MUTED, "");
  lv_obj_set_width(perm_where, 270);
  lv_label_set_long_mode(perm_where, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(perm_where, 0, 22);
  perm_detail = make_label(perm_card, &mont_vi_16, C_FG, "");
  lv_obj_set_size(perm_detail, 270, 66);
  lv_label_set_long_mode(perm_detail, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(perm_detail, 0, 48);
  make_btn(perm_card, 0, 144, 128, 44, 12, C_TILE, C_FG, "Deny", &lv_font_montserrat_16, perm_btn_cb, 0, NULL);
  make_btn(perm_card, 142, 144, 128, 44, 12, C_ACCENT, C_BG, "Allow", &lv_font_montserrat_16, perm_btn_cb, 1, NULL);
  lv_obj_add_flag(perm_card, LV_OBJ_FLAG_HIDDEN);
}

void create_all_screens() {
  create_home_screen();
  create_clock_screen();
  create_weather_screen();
  create_forecast_screen();
  create_claude_screen();
  create_account_screen();
  create_music_screen();
  create_pc_screen();
  create_pomo_screen();
  create_settings_screen();
  if (portal_active) create_setup_screen();   // Device: tao khi mo lan dau (show_screen)
  create_flash_badge();
  create_toast();
  create_permission_card();
}
