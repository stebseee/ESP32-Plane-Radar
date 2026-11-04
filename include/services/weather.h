#pragma once

namespace services::weather {

struct CurrentWeather {
  bool available = false;
  float temperature_c = 0.0f;
  int weather_code = -1;
};

/** Fetch current conditions and publish a snapshot. Runs in its own task. */
bool fetchCurrent(double latitude, double longitude);

/** Copy the latest successful weather result; false when unavailable. */
bool current(CurrentWeather* out);

/** Hide cached weather, for example when Wi-Fi disconnects. */
void clear();

}  // namespace services::weather
