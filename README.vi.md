# 🌐 LCD Control ESP32 CYD

Một giao diện LCD **ESP32 CYD** (ESP32-2432S028R, 2.8" ILI9341 + cảm ứng XPT2046), phong cách **Claude editorial**:
- Nền giấy ngà, chữ màu mực nâu, nhấn bằng màu đất nung.
- Các con số lớn dùng font serif **Fraunces**; chữ nhỏ dùng Montserrat.
- Giao diện bằng tiếng Anh; riêng dòng âm lịch giữ tiếng Việt.

**HOME** là lưới 9 ô. Mỗi ô hiện một con số xem nhanh của tab đó. Có 8 tab:

1. **Clock**: thứ, ngày, giờ (NTP), dòng **âm lịch** tiếng Việt kèm can chi, và một dòng tóm tắt thời tiết.
2. **Weather**: nhiệt độ, cảm giác như, độ ẩm, gió và **AQI**. Nguồn Open-Meteo.
3. **Forecast**: 6 giờ tới và 3 ngày tới, kèm khả năng mưa.
4. **Claude Code**: % phiên 5 giờ, % tuần, token và chi phí hôm nay, và **CONTEXT**: độ đầy ngữ cảnh của phiên đang chạy. Ý tưởng lấy từ [claudometer](https://github.com/krizdingus/claudometer).
5. **Account**: hạn mức chung của cả tài khoản Claude (Chat, Code, Cowork dùng chung), kèm tỷ lệ sử dụng tuần này theo từng nơi (Claude Code / Chats / Cowork / Other).
6. **Music**: bài đang phát trên PC (Spotify, YouTube trên Chrome/Edge, Apple Music…), kèm nút trước / phát-tạm dừng / sau.
7. **System** (Acer Nitro 5): CPU (tải + nhiệt độ), RAM, GPU (tải + nhiệt độ), VRAM.
8. **Pomodoro**: nút − / + hai bên đồng hồ để chỉnh thời lượng pha hiện tại, mỗi lần 5 phút. Thời lượng được lưu lại.

**Điều khiển:**
- **Tap một ô trên HOME:** mở tab đó. Tab đứng yên, không tự chuyển.
- **Tap ở bất kỳ đâu trong tab:** quay về HOME. Các nút trong tab Music và Pomodoro vẫn bấm bình thường.
- **Ô SLIDESHOW:** các tab đã chọn tự chuyển mỗi 20 giây.
- **Giữ một ô trên HOME (hoặc giữ ngay trong tab):** thêm vào hoặc bớt tab đó khỏi slideshow. Ô có chấm cam là đang nằm trong slideshow.
- Chế độ đang dùng và tab đang mở được nhớ lại sau khi mất điện.
- Đèn nền luôn sáng 100%.

**Thông báo Claude Code:** khi Claude làm xong việc hoặc đang chờ bạn (cần cấp quyền), CYD hiện thẻ thông báo kèm tiếng bíp (nếu có gắn loa vào cổng loa của CYD, GPIO26). Tap vào thẻ để đóng.

---

## 📸 Giao diện chính (Main Interface)

![CYD trên bàn làm việc](docs/images/desk.jpg)

| | |
|---|---|
| ![Home](docs/images/home.jpg) | ![Claude Code](docs/images/claude.jpg) |
| ![Music](docs/images/music.jpg) | ![Clock](docs/images/clock.jpg) |
| ![Weather](docs/images/weather.jpg) | ![Forecast](docs/images/forecast.jpg) |

## 📂 Cách sử dụng (How to Use)

### 1. Thư viện Arduino
Cần ESP32 core 3.x, `lvgl` 9.x, `TFT_eSPI` (dùng `User_Setup.h` cho CYD), `XPT2046_Touchscreen` và `ArduinoJson` 7.

Trong `lv_conf.h` cần đặt `LV_USE_STDLIB_MALLOC` bằng `LV_STDLIB_CLIB`, để LVGL dùng heap của ESP32: 6 trang không đủ chỗ trong pool mặc định 36 KB. Ngoài ra cần bật `LV_FONT_MONTSERRAT_14`, font này dùng cho các biểu tượng.

Các font tự tạo nằm ở `src/fonts/`:
- Montserrat bản có dấu tiếng Việt, cho dòng âm lịch.
- Chữ số Fraunces các cỡ 24, 34, 56 và 64.

Muốn tạo lại hoặc thêm cỡ chữ, dùng `tools/make_lvgl_font.py` (chỉ cần Python và Pillow, không cần Node.js). Với font biến thể như Fraunces, dùng thêm `--axes opsz,wght,SOFT,WONK`.

### 2. Cấu hình
Copy `ESP32_tft_clock_weather/secrets.example.h` thành `secrets.h`, rồi điền:
- `WIFI_SSID` và `WIFI_PASSWORD`
- `CLAUDE_HOST`: IP LAN của máy tính chạy server (xem bước 4)
- `CLAUDE_TOKEN`: một chuỗi ngẫu nhiên bất kỳ. Server sẽ tự đọc token từ file này.

Muốn tắt trang Claude thì đặt `CLAUDE_ENABLED` bằng `0`.

Múi giờ và toạ độ chỉnh ở đầu file `.ino`.

`secrets.h` đã nằm trong `.gitignore`, nên sẽ không bị đẩy lên GitHub.

### 3. Nạp code
Mở `ESP32_tft_clock_weather.ino` trong Arduino IDE, chọn board **ESP32 Dev Module** và chọn **Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**. Bắt buộc phải chọn, vì firmware khoảng 2 MB do icon ARGB8888, vượt phân vùng mặc định 1,3 MB. Sau đó nạp code.

### 4. Server Claude usage trên PC
CYD không đọc được log của Claude Code, nên cần chạy một server nhỏ trên chính máy bạn dùng Claude Code. Server chỉ dùng thư viện chuẩn Python 3.9+ và chạy được trên Windows, macOS và Linux.

```bash
python pc_server/claude_usage_server.py --plan max-5x
```

- `--plan`: chọn `free`, `pro`, `max-5x` hoặc `max-20x`. Gói này quyết định mức trần dùng để tính %.
- Server đọc `~/.claude/projects/**/*.jsonl` và trả về `GET /v1/stats` qua cổng `7842`, đúng schema v1 của claudometer. Nếu bạn chạy daemon claudometer gốc trên Mac/Linux thì CYD cũng đọc được.
- Lần chạy đầu, Windows sẽ hỏi quyền tường lửa. Hãy cho phép **Private network**.
- Lệnh `--print` in ra một mẫu JSON rồi thoát, dùng để kiểm tra nhanh.
- **Tự tìm IP:** server phát gói UDP broadcast `CLAUDE_USAGE 1 7842` mỗi 5 giây lên cổng 7843. Khi laptop đổi IP, CYD tự nhận địa chỉ mới; `CLAUDE_HOST` chỉ dùng làm mặc định.
- **Tự khởi động cùng Windows:** tạo shortcut tới `pythonw.exe` (chạy ngầm, không hiện cửa sổ) trong thư mục `shell:startup`, với tham số `claude_usage_server.py --plan pro --log server.log`. Muốn tắt tự khởi động thì xoá shortcut đó.

**Số chính xác (khuyên dùng):** cài [Claude Code CLI](https://claude.ai/install.ps1) rồi chạy `claude auth login` một lần.

- Server sẽ lấy % và giờ reset từ chính nguồn mà lệnh `/usage` dùng (`api.anthropic.com/api/oauth/usage`). Mỗi lần lấy cách nhau 2 phút, và chỉ lấy khi CYD đang hỏi số liệu. Nếu bị giới hạn tần suất (HTTP 429), server tự giãn thời gian chờ, tối đa 15 phút.
- Giữa hai lần lấy, server cộng thêm phần dùng mới từ log để số trên CYD vẫn chạy theo thời gian thực.
- Token chỉ gửi tới Anthropic. Khi token hết hạn, server tự làm mới.
- Nếu không lấy được (mất mạng, bị đăng xuất…), server tự quay về cách ước tính bên dưới.
- Tắt tính năng này bằng `--no-exact`.

**Thông báo Claude Code (hooks):** trong `~/.claude/settings.json` có hai hook `Stop` và `Notification`. Mỗi hook chạy `pc_server/notify.py`, script này báo cho server, rồi server đẩy ngay sang CYD qua UDP. Muốn tắt thông báo thì xoá mục `hooks` trong file đó.

**Tab Music:** dùng trung tâm điều khiển media có sẵn của Windows (Global System Media Transport Controls), nên đọc được mọi app phát nhạc: Spotify, YouTube trên trình duyệt, Apple Music…
- Cần cài thư viện cho Python: `pip install winrt-runtime winrt-Windows.Media.Control winrt-Windows.Foundation`.
- Server chỉ đọc khi CYD đang mở tab này.
- Nút bấm trên CYD gửi lệnh `POST /v1/media` về server.

**Tab Account:** số liệu lấy từ cùng nguồn usage chính xác của Anthropic. Cần đã đăng nhập bằng `claude auth login`.

**Trang System:** server chỉ đo khi CYD đang mở trang này.
- CPU và RAM đọc bằng Windows API.
- Nhiệt độ CPU lấy từ cảm biến nhiệt ACPI (`typeperf`, không cần quyền admin). Mỗi lượt `typeperf` tự tắt sau 60 giây.
- GPU đọc bằng `nvidia-smi`.

**Hiệu chỉnh % cho khớp claude.ai (chế độ ước tính):** mở trang usage trên claude.ai, rồi chạy ngay lệnh dưới với hai số % đang hiển thị (session, tuần). Server đang chạy sẽ tự đọc lại, không cần khởi động lại.
```bash
python pc_server/claude_usage_server.py --calibrate 13 17
```
Sau khi hiệu chỉnh, mức dùng được tính theo chi phí quy đổi API, tức là có trọng số theo model (Opus tính nặng hơn Haiku). Cách này bám sát claude.ai hơn so với đếm token thô.

> ⚠️ % chỉ là **ước tính**. Anthropic không công bố trần token; số liệu ở đây do claudometer suy ngược. Nếu lệch so với trang usage trên claude.ai, chỉnh bằng `--session-cap` và `--weekly-cap`. Chi phí $ cũng là ước tính theo bảng giá trong script.
