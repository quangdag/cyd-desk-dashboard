// =========================
// WI-FI SETUP: CYD phat WiFi mo "CYD-Setup", dien thoai vao 192.168.4.1 (tu bat trang nho DNS),
// nhap WiFi nha, token PC server, vi tri thoi tiet, mui gio -> luu NVS -> khoi dong lai.
// Vao che do nay khi: chua co WiFi (ban build chung lan dau) hoac bam Settings > WI-FI SETUP.
// Chay tren task rieng (core 0) thay cho task mang; UI chi hien huong dan.
// =========================

#define PORTAL_SSID "CYD-Setup"

static WebServer* portal_web = nullptr;
static DNSServer* portal_dns = nullptr;
static String portal_networks;            // <option> cac WiFi quet duoc
static volatile int portal_clients = 0;   // so may dang noi vao CYD-Setup (cho UI)
static volatile bool portal_saved = false;

// Bat che do Wi-Fi setup o lan khoi dong ke tiep (tu Settings)
static void request_portal_and_restart() {
  Preferences np;
  np.begin("net", false);
  np.putBool("portal", true);
  np.end();
  delay(100);
  ESP.restart();
}

static bool portal_requested() {
  Preferences np;
  np.begin("net", false);
  bool want = np.getBool("portal", false);
  if (want) np.remove("portal");
  np.end();
  return want;
}

static String html_escape(const char* in) {
  String out;
  for (const char* p = in; *p; p++) {
    switch (*p) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += *p;
    }
  }
  return out;
}

static void portal_scan() {
  int n = WiFi.scanNetworks();
  portal_networks = "";
  for (int i = 0; i < n && i < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length() || portal_networks.indexOf(">" + html_escape(ssid.c_str()) + "<") >= 0) continue;
    portal_networks += "<option value=\"" + html_escape(ssid.c_str()) + "\">" + html_escape(ssid.c_str()) + "</option>";
  }
  WiFi.scanDelete();
}

static const char PORTAL_HEAD[] PROGMEM = R"(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>CYD setup</title><style>
body{font-family:system-ui,sans-serif;background:#F5F0E8;color:#2B2722;margin:0;padding:20px;max-width:440px}
h1{font-size:20px;color:#C15F3C;letter-spacing:1px;margin:0 0 4px}p{color:#8C8173;margin:0 0 18px;font-size:14px}
label{display:block;font-size:12px;letter-spacing:1px;color:#8C8173;margin:14px 0 4px;text-transform:uppercase}
input,select{width:100%;box-sizing:border-box;font-size:16px;padding:10px;border:1px solid #DDD2C1;border-radius:8px;background:#fff}
.row{display:flex;gap:10px}.row>div{flex:1}small{color:#8C8173;font-size:12px}
button{margin-top:22px;width:100%;padding:14px;font-size:16px;border:0;border-radius:10px;background:#C15F3C;color:#F5F0E8}
</style></head><body>)";

static void portal_page() {
  String h = FPSTR(PORTAL_HEAD);
  h += "<h1>CYD DESK DASHBOARD</h1><p>Firmware " FW_VERSION ". Settings are saved on the device.</p>";
  h += "<form method=post action=/save>";
  h += "<label>Wi-Fi network</label><input name=ssid list=nets required value=\"" + html_escape(netcfg.ssid) + "\">";
  h += "<datalist id=nets>" + portal_networks + "</datalist>";
  h += "<label>Wi-Fi password</label><input name=pass type=password placeholder=\"";
  h += netcfg.pass[0] ? "unchanged" : "";
  h += "\">";
  h += "<label>PC server token</label><input name=token value=\"" + html_escape(netcfg.token) +
       "\"><small>Same token as the PC server. Leave empty to use only clock and weather.</small>";
  h += "<label>PC server IP (optional)</label><input name=host value=\"" + html_escape(netcfg.host) +
       "\" placeholder=\"found automatically\">";
  h += "<div class=row><div><label>Latitude</label><input name=lat inputmode=decimal value=\"" +
       html_escape(netcfg.lat) + "\"></div><div><label>Longitude</label><input name=lon inputmode=decimal value=\"" +
       html_escape(netcfg.lon) + "\"></div></div>";
  h += "<label>Time zone (POSIX)</label><input name=tz value=\"" + html_escape(netcfg.tz) +
       "\"><small>Vietnam/Thailand: ICT-7 &middot; Japan: JST-9 &middot; UK: GMT0BST,M3.5.0/1,M10.5.0</small>";
  h += "<button>Save and restart</button></form></body></html>";
  portal_web->send(200, "text/html", h);
}

static void portal_save() {
  WebServer& w = *portal_web;
  if (!w.hasArg("ssid") || !w.arg("ssid").length()) {
    w.send(400, "text/plain", "Wi-Fi network is required");
    return;
  }
  Preferences np;
  np.begin("net", false);
  np.putString("ssid", w.arg("ssid"));
  if (w.arg("pass").length()) np.putString("pass", w.arg("pass"));
  np.putString("token", w.arg("token"));
  np.putString("host", w.arg("host"));
  if (w.arg("lat").length()) np.putString("lat", w.arg("lat"));
  if (w.arg("lon").length()) np.putString("lon", w.arg("lon"));
  if (w.arg("tz").length()) np.putString("tz", w.arg("tz"));
  np.end();
  String h = FPSTR(PORTAL_HEAD);
  h += "<h1>SAVED</h1><p>The CYD restarts and joins your Wi-Fi. You can close this page.</p></body></html>";
  w.send(200, "text/html", h);
  portal_saved = true;
}

void portal_task(void*) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(PORTAL_SSID);
  delay(200);
  portal_scan();
  portal_dns = new DNSServer();
  portal_dns->start(53, "*", WiFi.softAPIP());   // moi ten mien -> CYD: dien thoai tu mo trang setup
  portal_web = new WebServer(80);
  portal_web->on("/", HTTP_GET, portal_page);
  portal_web->on("/save", HTTP_POST, portal_save);
  portal_web->onNotFound([]() {                  // trang kiem tra Internet cua dien thoai -> chuyen ve form
    portal_web->sendHeader("Location", "http://192.168.4.1/", true);
    portal_web->send(302, "text/plain", "");
  });
  portal_web->begin();
  Serial.println("Wi-Fi setup: join " PORTAL_SSID ", open http://192.168.4.1");
  uint32_t saved_at = 0;
  for (;;) {
    net_heartbeat = millis();
    portal_dns->processNextRequest();
    portal_web->handleClient();
    portal_clients = WiFi.softAPgetStationNum();
    if (portal_saved && !saved_at) saved_at = millis();
    if (saved_at && millis() - saved_at > 1500) ESP.restart();  // de trang "SAVED" kip toi dien thoai
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
