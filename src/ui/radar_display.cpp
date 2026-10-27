#include "ui/radar_display.h"

#include <Arduino.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorTagRoute = 0x87F1;
uint16_t kColorSweep = 0x47ED;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {&fonts::FreeSansBold12pt7b,
                                                  &fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {&fonts::FreeSansBold9pt7b,
                                               &fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  applyScaleStyle();
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangePresetCount; ++i) {
    for (bool miles : {false, true}) {
      radar::formatRing3Label(label, sizeof(label), radar::kRangePresets[i].ring3_km,
                              miles);
      const int w = tft.textWidth(label);
      if (w > s_scale_label_max_w) {
        s_scale_label_max_w = w;
      }
    }
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {&fonts::FreeSansBold12pt7b,
                                               &fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initPalette() {
  radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = tft.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  radar::kColorLabel = tft.color565(255, 255, 255);
  radar::kColorCenter = tft.color565(255, 255, 255);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR);
  } else {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  }
  radar::kColorTrackVector =
      tft.color565(radar::kTrackR, radar::kTrackG, radar::kTrackB);
  radar::kColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorTagAltitude =
      tft.color565(radar::kTagAltR, radar::kTagAltG, radar::kTagAltB);
  radar::kColorTagRoute =
      tft.color565(radar::kTagRouteR, radar::kTagRouteG, radar::kTagRouteB);
  radar::kColorSweep =
      tft.color565(radar::kSweepR, radar::kSweepG, radar::kSweepB);
  radar::kColorRunway =
      tft.color565(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::kColorRunwayLabel = tft.color565(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                          radar::kRunwayLabelB);
}

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

/** Mix two RGB565 colors: t = 0 gives `from`, t = 1 gives `to`. */
uint16_t blend565(uint16_t from, uint16_t to, float t) {
  t = std::max(0.0f, std::min(1.0f, t));
  const auto mix = [t](int a, int b) {
    return a + static_cast<int>(lroundf(static_cast<float>(b - a) * t));
  };
  const int r = mix((from >> 11) & 0x1F, (to >> 11) & 0x1F);
  const int g = mix((from >> 5) & 0x3F, (to >> 5) & 0x3F);
  const int b = mix(from & 0x1F, to & 0x1F);
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

/** Fade `color` toward the background by `brightness` (1 = full color). */
uint16_t dimmed(uint16_t color, float brightness) {
  return blend565(radar::kColorBackground, color, brightness);
}

/** Beam bearing now: degrees clockwise from north, 0-360. */
float sweepAngleDeg() {
  const unsigned long phase = millis() % config::kRadarSweepPeriodMs;
  return 360.0f * static_cast<float>(phase) /
         static_cast<float>(config::kRadarSweepPeriodMs);
}

/** Screen bearing of (x, y) from the center: degrees clockwise from north. */
float screenBearingDeg(int x, int y) {
  const float deg = atan2f(static_cast<float>(x - radar::kCenterX),
                           static_cast<float>(radar::kCenterY - y)) /
                    kDegToRad;
  return deg < 0.0f ? deg + 360.0f : deg;
}

/**
 * Brightness of a target at `bearing_deg`: 1 right after the beam passes it,
 * fading linearly to `min_brightness` just before the beam comes round again.
 */
float sweepBrightness(float bearing_deg, float sweep_deg, float min_brightness) {
  if (!radar::sweepEnabled()) {
    return 1.0f;
  }
  float since = sweep_deg - bearing_deg;  // degrees since the beam passed
  if (since < 0.0f) {
    since += 360.0f;
  }
  return 1.0f - (1.0f - min_brightness) * (since / 360.0f);
}

/** Point at `radius` px along screen bearing `deg` from the center. */
void pointOnBearing(float deg, int radius, int* x, int* y) {
  const float rad = deg * kDegToRad;
  *x = radar::kCenterX + static_cast<int>(lroundf(sinf(rad) * radius));
  *y = radar::kCenterY - static_cast<int>(lroundf(cosf(rad) * radius));
}

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  // Longitude degrees shrink toward the poles; scale by cos(latitude) so
  // east-west distance isn't overstated away from the equator.
  const float center_lat_rad =
      static_cast<float>(services::location::lat()) * kDegToRad;
  *dx_km = static_cast<float>(lon - services::location::lon()) * kKmPerDeg *
           cosf(center_lat_rad);
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

/**
 * Dead-reckon an aircraft's position from its last-fetched fix along its ground
 * track, so it moves smoothly between ADS-B updates. Uses the same flat
 * 1° ≈ 111 km projection as offsetKmFromCenter(), so it round-trips exactly.
 */
void extrapolatedLatLon(const services::adsb::Aircraft& plane,
                        unsigned long base_ms, float* lat, float* lon) {
  *lat = plane.lat;
  *lon = plane.lon;
  if (base_ms == 0 || plane.gs_knots <= 0.0f) {
    return;
  }
  // Elapsed since the fix was measured = time since fetch + the fix's own age.
  const unsigned long elapsed_ms = (millis() - base_ms) + plane.pos_age_ms;
  const float elapsed_h = static_cast<float>(elapsed_ms) / 3600000.0f;
  const float dist_km = plane.gs_knots * 1.852f * elapsed_h;  // knots -> km
  if (dist_km <= 0.0f) {
    return;
  }
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = plane.track_deg * kDegToRad;  // track: 0 = N, 90 = E
  *lat = plane.lat + (dist_km * cosf(rad)) / kKmPerDeg;
  *lon = plane.lon + (dist_km * sinf(rad)) / kKmPerDeg;
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return distSqFromCenter(x, y) <= max_r * max_r;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y, uint16_t color) {
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx, color);
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const int base_x =
      cx - static_cast<int>(lroundf(sin_h * static_cast<float>(radar::kAircraftTailLenPx)));
  const int base_y =
      cy + static_cast<int>(lroundf(cos_h * static_cast<float>(radar::kAircraftTailLenPx)));

  const int wing_x = static_cast<int>(lroundf(cos_h * radar::kAircraftTailHalfPx));
  const int wing_y = static_cast<int>(lroundf(sin_h * radar::kAircraftTailHalfPx));

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_tag_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
  }
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  if (plane.callsign[0] != '\0') {
    const int w = s_draw->textWidth(plane.callsign);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.route[0] != '\0') {
    const int w = s_draw->textWidth(plane.route);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.type[0] != '\0') {
    const int w = s_draw->textWidth(plane.type);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.alt[0] != '\0') {
    const int w = s_draw->textWidth(plane.alt);
    if (w > max_w) {
      max_w = w;
    }
  }
  return max_w;
}

void drawAircraftTag(int x, int y, const services::adsb::Aircraft& plane,
                     float brightness) {
  initTagLabelMetrics();
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  const int block_w = measureTagBlockWidth(plane);
  // The route line only takes space once it is known.
  const bool has_route = plane.route[0] != '\0';
  const int block_h = line_h * (has_route ? 4 : 3);
  int ly = y - block_h / 2;

  const int symbol_half =
      radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx;
  // West (left): tag toward center on the right; east (right): tag on the left.
  const bool tag_on_right = x < radar::kCenterX;
  int anchor_x = 0;
  if (tag_on_right) {
    anchor_x = x + symbol_half + radar::kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, radar::kSize - block_w - 1);
    s_draw->setTextDatum(textdatum_t::top_left);
  } else {
    anchor_x = x - symbol_half - radar::kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 1);
    s_draw->setTextDatum(textdatum_t::top_right);
  }
  ly = std::max(1, std::min(ly, radar::kSize - block_h - 1));

  if (plane.callsign[0] != '\0') {
    s_draw->setTextColor(dimmed(radar::kColorLabel, brightness),
                         radar::kColorBackground);
    s_draw->drawString(plane.callsign, anchor_x, ly);
  }
  ly += line_h;

  if (has_route) {
    s_draw->setTextColor(dimmed(radar::kColorTagRoute, brightness),
                         radar::kColorBackground);
    s_draw->drawString(plane.route, anchor_x, ly);
    ly += line_h;
  }

  if (plane.type[0] != '\0') {
    s_draw->setTextColor(dimmed(radar::kColorTagType, brightness),
                         radar::kColorBackground);
    s_draw->drawString(plane.type, anchor_x, ly);
  }
  ly += line_h;

  if (plane.alt[0] != '\0') {
    s_draw->setTextColor(dimmed(radar::kColorTagAltitude, brightness),
                         radar::kColorBackground);
    s_draw->drawString(plane.alt, anchor_x, ly);
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
  /** Screen bearing from the center, for the sweep fade. */
  float bearing_deg = 0.0f;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
  float bearing_deg = 0.0f;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

/** One render pass's aircraft, projected to screen and sorted far-first. */
struct AircraftFrame {
  services::adsb::Aircraft planes[services::adsb::kMaxAircraft];
  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;
  /** Beam bearing for this frame (degrees clockwise from north). */
  float sweep_deg = 0.0f;
};

/** Static: ~5 KB, too big for the loop task's stack. */
AircraftFrame s_aircraft_frame;

void collectAircraft(AircraftFrame* frame) {
  initLabelMetrics();

  // Snapshot under the adsb lock (the fetch may run on another thread).
  unsigned long base_ms = 0;
  const size_t n = services::adsb::snapshotAircraft(
      frame->planes, services::adsb::kMaxAircraft, &base_ms);

  frame->draw_count = 0;
  frame->dot_count = 0;
  frame->sweep_deg = sweepAngleDeg();

  for (size_t i = 0; i < n; ++i) {
    // Dead-reckoned position for smooth motion between fetches.
    float lat = 0.0f;
    float lon = 0.0f;
    extrapolatedLatLon(frame->planes[i], base_ms, &lat, &lon);

    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(lat, lon, &x, &y);
      AircraftDrawItem& item = frame->items[frame->draw_count++];
      item.index = i;
      item.x = x;
      item.y = y;
      item.dist_sq = distSqFromCenter(x, y);
      item.bearing_deg = screenBearingDeg(x, y);
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(lat, lon, &dot_x, &dot_y)) {
      continue;
    }
    BeyondDotDrawItem& dot = frame->dots[frame->dot_count++];
    dot.x = dot_x;
    dot.y = dot_y;
    dot.dist_sq = distSqFromCenter(dot_x, dot_y);
    dot.bearing_deg = screenBearingDeg(dot_x, dot_y);
  }

  sortBeyondDotsFarFirst(frame->dots, frame->dot_count);
  sortDrawItemsFarFirst(frame->items, frame->draw_count);
}

/** `with_sweep` = fade by the beam; the clock face has no beam, so no fade. */
void drawBeyondRingDots(const AircraftFrame& frame, bool with_sweep) {
  for (size_t d = 0; d < frame.dot_count; ++d) {
    const float brightness =
        with_sweep ? sweepBrightness(frame.dots[d].bearing_deg, frame.sweep_deg,
                                     config::kRadarSweepMinBrightness)
                   : 1.0f;
    drawBeyondRingDot(frame.dots[d].x, frame.dots[d].y,
                      dimmed(radar::kColorAircraft, brightness));
  }
}

void drawAircraftSymbols(const AircraftFrame& frame) {
  for (size_t d = 0; d < frame.draw_count; ++d) {
    const services::adsb::Aircraft& plane = frame.planes[frame.items[d].index];
    const int x = frame.items[d].x;
    const int y = frame.items[d].y;
    const float brightness =
        sweepBrightness(frame.items[d].bearing_deg, frame.sweep_deg,
                        config::kRadarSweepMinBrightness);
    drawSpeedVector(x, y, plane.nose_deg, plane.track_deg, plane.gs_knots,
                    dimmed(radar::kColorTrackVector, brightness));
    drawHeadingTriangle(x, y, plane.nose_deg,
                        dimmed(radar::kColorAircraft, brightness));
  }
  for (size_t d = 0; d < frame.draw_count; ++d) {
    const float brightness =
        sweepBrightness(frame.items[d].bearing_deg, frame.sweep_deg,
                        config::kRadarSweepTagMinBrightness);
    drawAircraftTag(frame.items[d].x, frame.items[d].y,
                    frame.planes[frame.items[d].index], brightness);
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_cardinal_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_scale_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th / 2 - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2,
                   radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    s_draw->drawCircle(cx, cy, r - i, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenter);
}

void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

/** Fading trail behind the beam: pie slices dimming away from it. */
void drawSweepTrail(float sweep_deg) {
  const int r = radar::kGridOuterRadius;
  const float step = config::kRadarSweepTrailDeg / radar::kSweepTrailSegments;
  for (int i = 0; i < radar::kSweepTrailSegments; ++i) {
    const float t = radar::kSweepTrailPeak *
                    (1.0f - static_cast<float>(i) / radar::kSweepTrailSegments);
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    pointOnBearing(sweep_deg - step * i, r, &x0, &y0);
    pointOnBearing(sweep_deg - step * (i + 1), r, &x1, &y1);
    s_draw->fillTriangle(radar::kCenterX, radar::kCenterY, x0, y0, x1, y1,
                         dimmed(radar::kColorSweep, t));
  }
}

void drawSweepBeam(float sweep_deg) {
  int x = 0;
  int y = 0;
  pointOnBearing(sweep_deg, radar::kGridOuterRadius, &x, &y);
  s_draw->drawWideLine(radar::kCenterX, radar::kCenterY, x, y,
                       radar::kSweepBeamHalfWidth, radar::kColorSweep);
}

/** Grid, runways and labels; with the sweep on, the trail sits under the
 *  rings and the beam over them. */
template <typename Gfx>
void drawStaticGrid(Gfx& gfx, float sweep_deg) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  initPalette();
  if (radar::sweepEnabled()) {
    drawSweepTrail(sweep_deg);
  }
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  runway::drawLargeAirportRunways(gfx);
  if (radar::sweepEnabled()) {
    drawSweepBeam(sweep_deg);
  }
  drawCenterDot(cx, cy);
  drawCardinalLabels();
  drawScaleLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

/** millis() when an aircraft was last inside the ring (0 = never). */
unsigned long s_last_aircraft_in_ring_ms = 0;

/**
 * Clock instead of the radar while the ring has been empty for
 * kClockIdleDelayMs, once NTP has set the time. Any aircraft inside the ring
 * switches straight back to the radar.
 */
bool shouldShowClock(const AircraftFrame& frame, struct tm* now) {
  if (frame.draw_count > 0) {
    s_last_aircraft_in_ring_ms = millis();
    return false;
  }
  if (!config::kIdleClockEnabled) {
    return false;
  }
  if (s_last_aircraft_in_ring_ms != 0 &&
      millis() - s_last_aircraft_in_ring_ms < config::kClockIdleDelayMs) {
    return false;
  }
  // Timeout 0: don't block the render loop waiting for the first NTP sync.
  return getLocalTime(now, 0);
}

void formatClockTime(const struct tm& now, char* out, size_t out_len) {
  if (config::kClock24Hour) {
    snprintf(out, out_len, "%02d:%02d", now.tm_hour, now.tm_min);
    return;
  }
  const int hour12 = now.tm_hour % 12 == 0 ? 12 : now.tm_hour % 12;
  snprintf(out, out_len, "%d:%02d", hour12, now.tm_min);
}

uint16_t clockColor(uint8_t r, uint8_t g, uint8_t b) {
  if (config::kDisplayRgbOrder) {
    return tft.color565(b, g, r);
  }
  return tft.color565(r, g, b);
}

template <typename Gfx>
void drawWatchBezel(Gfx& gfx) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const uint16_t blue = clockColor(20, 45, 210);
  const uint16_t cyan = clockColor(35, 175, 235);
  const uint16_t amber = clockColor(245, 165, 25);
  const uint16_t red = clockColor(130, 35, 20);
  const uint16_t muted_green = clockColor(0, 65, 14);

  gfx.drawCircle(cx, cy, 119, muted_green);
  gfx.drawCircle(cx, cy, 116, muted_green);
  for (int bearing = 0; bearing < 360; bearing += 6) {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    const int outer = 115;
    const bool major = bearing % 30 == 0;
    const int inner = outer - (major ? 12 : 5);
    pointOnBearing(static_cast<float>(bearing), outer, &x0, &y0);
    pointOnBearing(static_cast<float>(bearing), inner, &x1, &y1);

    uint16_t color = muted_green;
    if (bearing >= 315 || bearing < 45) color = bearing < 45 ? blue : red;
    else if (bearing < 135) color = cyan;
    else if (bearing < 180) color = cyan;
    else if (bearing < 225) color = amber;
    else color = amber;
    if (major) {
      color = clockColor(70, 235, 60);
    }
    gfx.drawWideLine(x0, y0, x1, y1, major ? 1.8f : 1.0f, color);
  }

  // A subdued radar instrument beneath the time readout.
  const uint16_t panel = clockColor(0, 14, 4);
  const uint16_t grid = clockColor(0, 48, 13);
  gfx.fillCircle(cx, cy, 86, panel);
  gfx.drawCircle(cx, cy, 82, grid);
  gfx.drawCircle(cx, cy, 61, grid);
  gfx.drawCircle(cx, cy, 39, grid);
  gfx.drawWideLine(cx - 77, cy, cx + 77, cy, 0.6f, grid);
  gfx.drawWideLine(cx, cy - 77, cx, cy + 77, 0.6f, grid);

  if (radar::sweepEnabled()) {
    drawSweepTrail(sweepAngleDeg());
    drawSweepBeam(sweepAngleDeg());
  }
  gfx.fillCircle(cx, cy, 2, clockColor(190, 115, 20));
}

template <typename Gfx>
void drawClockFace(Gfx& gfx, const AircraftFrame& frame, const struct tm& now) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;

  const uint16_t background = clockColor(0, 2, 3);
  const uint16_t panel = clockColor(0, 14, 4);
  const uint16_t green = clockColor(40, 255, 35);
  const uint16_t dim_green = clockColor(0, 100, 20);
  const uint16_t cyan = clockColor(25, 210, 180);
  gfx.fillScreen(background);
  drawWatchBezel(gfx);

  // The tiny system readouts and broad digital time echo a 1990s spy watch.
  applyCardinalStyle();
  gfx.setTextDatum(textdatum_t::top_center);
  gfx.setTextColor(dim_green, background);
  gfx.drawString("Q WATCH  //  007", cx, 31);
  gfx.drawString("MISSION STATUS: STANDBY", cx, 47);

  // Date and contact status sit below the main time.
  char date[16];
  strftime(date, sizeof(date), "%a %d %b", &now);
  applyCardinalStyle();
  gfx.setTextDatum(textdatum_t::top_center);
  gfx.setTextColor(cyan, panel);
  gfx.drawString(date, cx, cy + 28);
  gfx.setTextColor(dim_green, panel);
  gfx.drawString("RADAR SYSTEM // ACTIVE", cx, cy + 48);

  // Aircraft beyond the range ring remain visible as tiny bezel contacts.
  drawBeyondRingDots(frame, false);

  // Font7 is LovyanGFX's 48 px seven-segment font (digits and ':' only). It
  // replaces the VLW font on gfx; drawStaticGrid()/this function reload it.
  char time_text[8];
  formatClockTime(now, time_text, sizeof(time_text));
  gfx.setFont(&fonts::Font7);
  gfx.setTextSize(1);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(dim_green, panel);
  gfx.drawString(time_text, cx + 1, cy - 5);
  gfx.setTextColor(green, panel);
  gfx.drawString(time_text, cx, cy - 6);

  applyCardinalStyle();
  gfx.setTextDatum(textdatum_t::top_center);
  gfx.setTextColor(green, panel);
  gfx.drawString("INNER RING: CLEAR", cx, cy + 69);
  for (int i = 0; i < 5; ++i) {
    const int x = 67 + i * 23;
    gfx.fillRect(x, 204, 17, 4, dim_green);
    gfx.fillRect(x, 204, 14, 4, green);
  }
  gfx.setTextDatum(textdatum_t::top_left);
}

/** Either the idle clock or the radar grid with aircraft, into gfx. */
template <typename Gfx>
void composeFrame(Gfx& gfx) {
  collectAircraft(&s_aircraft_frame);

  struct tm now = {};
  if (shouldShowClock(s_aircraft_frame, &now)) {
    drawClockFace(gfx, s_aircraft_frame, now);
    return;
  }

  // opens its own DrawScope(gfx)
  drawStaticGrid(gfx, s_aircraft_frame.sweep_deg);
  const DrawScope scope(gfx);
  drawBeyondRingDots(s_aircraft_frame, true);
  drawAircraftSymbols(s_aircraft_frame);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  s_frame.setColorDepth(lgfx::color_depth_t::rgb332_1Byte);
  if (!s_frame.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: frame sprite alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

// Double-buffered frame: composite the grid AND aircraft into the off-screen
// sprite, then blit it to the panel in a single pushSprite. Because the panel
// is updated in one pass, labels never show an erase/redraw gap — no flicker.
void renderFrame() {
  composeFrame(s_frame);
  s_frame.pushSprite(0, 0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  composeFrame(tft);
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  radarDisplayDraw();
}

}  // namespace ui
