// Am lich Viet Nam - thuat toan cua Ho Ngoc Duc (https://www.informatik.uni-leipzig.de/~duc/amlich/)
// Da kiem tra: 17/02/2026 = 1/1 Binh Ngo, 29/01/2025 = 1/1 At Ty, 10/02/2024 = 1/1 Giap Thin.
#pragma once
#include <math.h>

struct LunarDate {
  int day, month, year;
  bool leap;
};

static long lunar_jd(int dd, int mm, int yy) {
  long a = (14 - mm) / 12, y = yy + 4800 - a, m = mm + 12 * a - 3;
  long jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
  if (jd < 2299161) jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - 32083;
  return jd;
}

static long lunar_new_moon(long k, double tz) {
  double T = k / 1236.85, T2 = T * T, T3 = T2 * T, dr = M_PI / 180;
  double jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3 +
               0.00033 * sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
  double M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
  double Mp = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
  double F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
  double c = (0.1734 - 0.000393 * T) * sin(M * dr) + 0.0021 * sin(2 * dr * M) - 0.4068 * sin(Mp * dr) +
             0.0161 * sin(dr * 2 * Mp) - 0.0004 * sin(dr * 3 * Mp) + 0.0104 * sin(dr * 2 * F) -
             0.0051 * sin(dr * (M + Mp)) - 0.0074 * sin(dr * (M - Mp)) + 0.0004 * sin(dr * (2 * F + M)) -
             0.0004 * sin(dr * (2 * F - M)) - 0.0006 * sin(dr * (2 * F + Mp)) + 0.0010 * sin(dr * (2 * F - Mp)) +
             0.0005 * sin(dr * (2 * Mp + M));
  double dt = (T < -11) ? 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3
                        : -0.000278 + 0.000265 * T + 0.000262 * T2;
  return (long)floor(jd1 + c - dt + 0.5 + tz / 24);
}

static int lunar_sun_longitude(long jdn, double tz) {
  double T = (jdn - 2451545.5 - tz / 24) / 36525, T2 = T * T, dr = M_PI / 180;
  double M = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
  double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
  double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * sin(dr * M) +
              (0.019993 - 0.000101 * T) * sin(dr * 2 * M) + 0.000290 * sin(dr * 3 * M);
  double L = (L0 + DL) * dr;
  L -= M_PI * 2 * floor(L / (M_PI * 2));
  return (int)floor(L / M_PI * 6);
}

static long lunar_month11(int yy, double tz) {
  long off = lunar_jd(31, 12, yy) - 2415021;
  long k = (long)floor(off / 29.530588853);
  long nm = lunar_new_moon(k, tz);
  return lunar_sun_longitude(nm, tz) >= 9 ? lunar_new_moon(k - 1, tz) : nm;
}

static int lunar_leap_offset(long a11, double tz) {
  long k = (long)floor((a11 - 2415021.076998695) / 29.530588853 + 0.5);
  int i = 1, last;
  int arc = lunar_sun_longitude(lunar_new_moon(k + i, tz), tz);
  do {
    last = arc;
    i++;
    arc = lunar_sun_longitude(lunar_new_moon(k + i, tz), tz);
  } while (arc != last && i < 14);
  return i - 1;
}

static LunarDate solar_to_lunar(int dd, int mm, int yy, double tz = 7.0) {
  long day = lunar_jd(dd, mm, yy);
  long k = (long)floor((day - 2415021.076998695) / 29.530588853);
  long month_start = lunar_new_moon(k + 1, tz);
  if (month_start > day) month_start = lunar_new_moon(k, tz);
  long a11 = lunar_month11(yy, tz), b11 = a11;
  int lyear;
  if (a11 >= month_start) {
    lyear = yy;
    a11 = lunar_month11(yy - 1, tz);
  } else {
    lyear = yy + 1;
    b11 = lunar_month11(yy + 1, tz);
  }
  LunarDate r;
  r.day = (int)(day - month_start + 1);
  int diff = (int)floor((month_start - a11) / 29.0);
  r.leap = false;
  r.month = diff + 11;
  if (b11 - a11 > 365) {
    int leap_diff = lunar_leap_offset(a11, tz);
    if (diff >= leap_diff) {
      r.month = diff + 10;
      if (diff == leap_diff) r.leap = true;
    }
  }
  if (r.month > 12) r.month -= 12;
  if (r.month >= 11 && diff < 4) lyear -= 1;
  r.year = lyear;
  return r;
}

static const char* const LUNAR_CAN[] = {"Giáp", "Ất", "Bính", "Đinh", "Mậu", "Kỷ", "Canh", "Tân", "Nhâm", "Quý"};
static const char* const LUNAR_CHI[] = {"Tý", "Sửu", "Dần", "Mão", "Thìn", "Tỵ", "Ngọ", "Mùi", "Thân", "Dậu", "Tuất", "Hợi"};

// "Bính Ngọ" cho nam am lich
static void lunar_year_name(int lyear, char* out, size_t n) {
  snprintf(out, n, "%s %s", LUNAR_CAN[(lyear + 6) % 10], LUNAR_CHI[(lyear + 8) % 12]);
}
