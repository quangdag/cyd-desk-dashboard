// Kieu du lieu dung trong chu ky ham cua .ino.
// Phai nam trong header: Arduino IDE tu sinh khai bao ham o dau file .ino, truoc cac kieu khai bao trong do.
#pragma once
#include <stdint.h>

struct Tone { uint16_t freq, ms; };                       // mot not bip: tan so (0 = im), thoi gian
struct AqiLevel { const char* text; uint32_t bg; uint32_t fg; };
enum PomoPhase { POMO_FOCUS, POMO_BREAK, POMO_LONG };

// Cac trang. SCR_HOME la man chon widget; cac trang con lai la "tab".
enum ScreenState {
  SCR_HOME = 0, SCR_CLOCK, SCR_WEATHER, SCR_FORECAST, SCR_CLAUDE, SCR_ACCOUNT,
  SCR_MUSIC, SCR_PC, SCR_POMO, SCREEN_COUNT
};

// HOME: dung yen o Home | FIXED: dung yen o 1 tab | SLIDESHOW: tu chuyen qua cac tab da chon
enum NavMode { MODE_HOME = 0, MODE_FIXED, MODE_SLIDESHOW };
