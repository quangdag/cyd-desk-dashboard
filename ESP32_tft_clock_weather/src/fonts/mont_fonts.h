// Font tao bang tools/make_lvgl_font.py (SIL OFL 1.1 - xem OFL-Montserrat.txt, OFL-Fraunces.txt)
#pragma once
#include <lvgl.h>

// Montserrat co dau tieng Viet: dong am lich, ten bai hat / ca si
LV_FONT_DECLARE(mont_vi_14)
LV_FONT_DECLARE(mont_vi_16)
LV_FONT_DECLARE(mont_vi_20)

// Noto Sans Thai: font du phong cua mont_vi_20 / mont_vi_14 cho ten bai hat tieng Thai (OFL-NotoSansThai.txt)
LV_FONT_DECLARE(thai_20)
LV_FONT_DECLARE(thai_14)

// Fraunces (serif) chi so: 0-9 : % . - + / ° C F - phong cach "Claude editorial"
LV_FONT_DECLARE(fr_num_64)     // dong ho, Pomodoro (so rong deu)
LV_FONT_DECLARE(fr_num_56)     // % phien Claude (so rong deu)
LV_FONT_DECLARE(fr_num_34)     // nhiet do thoi tiet
LV_FONT_DECLARE(fr_num_24)     // % tuan, o xem nhanh tren Home
