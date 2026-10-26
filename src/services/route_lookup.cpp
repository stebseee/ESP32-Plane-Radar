#include "services/route_lookup.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cctype>
#include <cstring>

#include "config.h"

namespace services::route {

namespace {

constexpr char kApiBase[] = "https://api.adsbdb.com/v0/callsign/";
constexpr int kConnectTimeoutMs = 5000;
constexpr uint16_t kRequestTimeoutMs = 6000;

/** Room for "BAW117" style ICAO callsigns (8 chars) plus the terminator. */
constexpr size_t kCallsignLen = 9;
constexpr size_t kCacheSize = 48;

enum class State : uint8_t {
  kEmpty,
  kPending,   // seen, not looked up yet
  kResolved,  // route known (may be empty if adsbdb has no route)
};

struct Entry {
  char callsign[kCallsignLen];
  char route[kRouteLen];
  State state;
  /** millis() of the last request()/lookup; oldest is evicted first. */
  unsigned long last_used_ms;
  /** millis() a failed lookup may be retried after (0 = no wait). */
  unsigned long retry_after_ms;
};

Entry s_cache[kCacheSize];

Entry* find(const char* callsign) {
  for (Entry& e : s_cache) {
    if (e.state != State::kEmpty && strcmp(e.callsign, callsign) == 0) {
      return &e;
    }
  }
  return nullptr;
}

Entry* allocate() {
  Entry* oldest = &s_cache[0];
  for (Entry& e : s_cache) {
    if (e.state == State::kEmpty) {
      return &e;
    }
    if (static_cast<long>(e.last_used_ms - oldest->last_used_ms) < 0) {
      oldest = &e;
    }
  }
  return oldest;
}

/** Hex-only fallbacks (no "flight" in the feed) have no route to look up. */
bool looksLikeFlightCallsign(const char* callsign) {
  const size_t n = strlen(callsign);
  if (n < 3 || n >= kCallsignLen) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (!isalnum(static_cast<unsigned char>(callsign[i]))) {
      return false;
    }
  }
  return true;
}

/** IATA code ("LHR") when present, else ICAO ("EGLL"). */
const char* airportCode(JsonObject airport) {
  const char* iata = airport["iata_code"] | "";
  if (iata[0] != '\0') {
    return iata;
  }
  return airport["icao_code"] | "";
}

/**
 * Look up one callsign. Returns true when adsbdb gave a definite answer
 * (route found, or unknown callsign); false on network/server errors.
 */
bool fetchRoute(const char* callsign, char* out, size_t out_len) {
  out[0] = '\0';

  String url = kApiBase;
  url += callsign;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kRequestTimeoutMs);

  const int code = http.GET();
  if (code == HTTP_CODE_NOT_FOUND) {
    http.end();
    return true;  // unknown callsign: cache the empty route
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("route: %s HTTP %d\n", callsign, code);
    http.end();
    return false;
  }

  // Only the airport codes are kept; names, coordinates etc. are dropped.
  JsonDocument filter;
  JsonObject fr = filter["response"]["flightroute"].to<JsonObject>();
  for (const char* side : {"origin", "destination"}) {
    fr[side]["iata_code"] = true;
    fr[side]["icao_code"] = true;
  }

  JsonDocument doc;
  const String body = http.getString();
  http.end();
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("route: %s JSON parse error: %s\n", callsign, err.c_str());
    return false;
  }

  JsonObject route = doc["response"]["flightroute"];
  if (route.isNull()) {
    return true;
  }
  const char* from = airportCode(route["origin"]);
  const char* to = airportCode(route["destination"]);
  if (from[0] != '\0' && to[0] != '\0') {
    snprintf(out, out_len, "%s-%s", from, to);
  }
  return true;
}

}  // namespace

void lookupCached(const char* callsign, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  Entry* e = find(callsign);
  if (e == nullptr || e->state != State::kResolved) {
    return;
  }
  e->last_used_ms = millis();
  strncpy(out, e->route, out_len - 1);
  out[out_len - 1] = '\0';
}

void request(const char* callsign) {
  if (!looksLikeFlightCallsign(callsign)) {
    return;
  }
  Entry* e = find(callsign);
  if (e != nullptr) {
    e->last_used_ms = millis();
    return;
  }
  e = allocate();
  strncpy(e->callsign, callsign, kCallsignLen - 1);
  e->callsign[kCallsignLen - 1] = '\0';
  e->route[0] = '\0';
  e->state = State::kPending;
  e->last_used_ms = millis();
  e->retry_after_ms = 0;
}

void resolvePending(size_t max_lookups) {
  size_t done = 0;
  for (Entry& e : s_cache) {
    if (done >= max_lookups) {
      break;
    }
    if (e.state != State::kPending) {
      continue;
    }
    if (e.retry_after_ms != 0 &&
        static_cast<long>(millis() - e.retry_after_ms) < 0) {
      continue;
    }
    ++done;
    char route[kRouteLen];
    if (fetchRoute(e.callsign, route, sizeof(route))) {
      memcpy(e.route, route, sizeof(route));
      e.state = State::kResolved;
      Serial.printf("route: %s -> %s\n", e.callsign,
                    route[0] != '\0' ? route : "(unknown)");
    } else {
      e.retry_after_ms = millis() + config::kRouteRetryAfterErrorMs;
    }
  }
}

}  // namespace services::route
