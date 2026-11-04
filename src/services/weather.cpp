#include "services/weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cmath>
#include <cstdio>

#include <freertos/FreeRTOS.h>

namespace services::weather {

namespace {

constexpr int kConnectTimeoutMs = 3500;
constexpr int kRequestTimeoutMs = 3500;
portMUX_TYPE s_weather_mux = portMUX_INITIALIZER_UNLOCKED;
CurrentWeather s_current;

void publishUnavailable() {
  portENTER_CRITICAL(&s_weather_mux);
  s_current.available = false;
  s_current.weather_code = -1;
  portEXIT_CRITICAL(&s_weather_mux);
}

}  // namespace

bool fetchCurrent(double latitude, double longitude) {
  if (WiFi.status() != WL_CONNECTED || latitude < -90.0 || latitude > 90.0 ||
      longitude < -180.0 || longitude > 180.0) {
    publishUnavailable();
    return false;
  }

  char url[220];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f"
           "&current=temperature_2m,weather_code&temperature_unit=celsius&timezone=auto",
           latitude, longitude);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    publishUnavailable();
    return false;
  }
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kRequestTimeoutMs);
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    publishUnavailable();
    return false;
  }

  JsonDocument filter;
  JsonObject current_filter = filter["current"].to<JsonObject>();
  current_filter["temperature_2m"] = true;
  current_filter["weather_code"] = true;
  JsonDocument doc;
  const String body = http.getString();
  const DeserializationError error = deserializeJson(
      doc, body.c_str(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    publishUnavailable();
    return false;
  }

  const JsonVariantConst temp_value = doc["current"]["temperature_2m"];
  const JsonVariantConst code_value = doc["current"]["weather_code"];
  if ((!temp_value.is<float>() && !temp_value.is<double>() &&
       !temp_value.is<int>()) ||
      !code_value.is<int>()) {
    publishUnavailable();
    return false;
  }

  const float temperature_c = temp_value.as<float>();
  const int weather_code = code_value.as<int>();
  if (!std::isfinite(temperature_c) || weather_code < 0 || weather_code > 99) {
    publishUnavailable();
    return false;
  }

  portENTER_CRITICAL(&s_weather_mux);
  s_current.available = true;
  s_current.temperature_c = temperature_c;
  s_current.weather_code = weather_code;
  portEXIT_CRITICAL(&s_weather_mux);
  return true;
}

bool current(CurrentWeather* out) {
  if (out == nullptr) return false;
  portENTER_CRITICAL(&s_weather_mux);
  *out = s_current;
  portEXIT_CRITICAL(&s_weather_mux);
  return out->available;
}

void clear() { publishUnavailable(); }

}  // namespace services::weather
