#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** BOOT hold duration to toggle the radar sweep (release before reset hold). */
constexpr unsigned long kBootSweepToggleMinMs = 800UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 50.743308;
constexpr double kDefaultRadarLon = -1.983437;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 5000;
/** Redraw cadence; aircraft are dead-reckoned along their track/speed so motion
 *  is smooth. 10 Hz = 100 ms keeps the sweep beam fluid (panel scanout caps at
 *  ~24 Hz; the full-frame recompose+present is the practical limit ~10-15 Hz). */
constexpr unsigned long kRadarRedrawIntervalMs = 100;

// --- Radar sweep (rotating beam; aircraft brighten as it passes them) ---
constexpr bool kRadarSweepEnabled = true;
/** One full clockwise rotation of the beam. */
constexpr unsigned long kRadarSweepPeriodMs = 4000;
/** Width of the fading trail behind the beam (degrees). */
constexpr float kRadarSweepTrailDeg = 60.0f;
/** Aircraft symbol brightness just before the beam returns (1 = no fade). */
constexpr float kRadarSweepMinBrightness = 0.25f;
/** Same for the text tags; kept higher so they stay readable. */
constexpr float kRadarSweepTagMinBrightness = 0.55f;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

/** Show origin-destination airport codes (e.g. "LHR-JFK") on aircraft tags,
 *  looked up per callsign from adsbdb.com and cached. */
constexpr bool kShowRoutes = true;
/** Route lookups per ADS-B fetch cycle (each is one HTTPS request). */
constexpr size_t kRouteLookupsPerCycle = 3;
/** Wait before retrying a callsign whose lookup hit a network/server error. */
constexpr unsigned long kRouteRetryAfterErrorMs = 60000;

// --- Idle clock (shown while no aircraft are inside the radar ring) ---
constexpr bool kIdleClockEnabled = true;
/** POSIX TZ string; default is UK time (GMT, BST in summer). Examples:
 *  "CET-1CEST,M3.5.0,M10.5.0/3" (central Europe),
 *  "EST5EDT,M3.2.0,M11.1.0" (US eastern). */
constexpr char kClockTimeZone[] = "GMT0BST,M3.5.0/1,M10.5.0";
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.google.com";
/** true = 24-hour clock ("18:05"); false = 12-hour ("6:05"). */
constexpr bool kClock24Hour = true;
/** Stay on the radar this long after the last aircraft leaves the ring, so a
 *  plane hovering at the edge doesn't flip the screen back and forth. */
constexpr unsigned long kClockIdleDelayMs = 10000;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
