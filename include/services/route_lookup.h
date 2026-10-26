#pragma once

#include <cstddef>

namespace services::route {

/** Room for "EGLL-KJFK" (ICAO fallback) plus the terminator. */
constexpr size_t kRouteLen = 10;

/**
 * Copy the cached route for `callsign` (e.g. "LHR-JFK") into `out`. Leaves
 * `out` empty when the route is unknown or not looked up yet. Call only from
 * the ADS-B fetch task; the cache is not locked.
 */
void lookupCached(const char* callsign, char* out, size_t out_len);

/**
 * Queue `callsign` for a route lookup if it isn't cached yet. Cheap; no I/O.
 */
void request(const char* callsign);

/**
 * Resolve up to `max_lookups` queued callsigns against adsbdb.com. Blocks on
 * HTTPS, so run it on the fetch task, never the render loop.
 */
void resolvePending(size_t max_lookups);

}  // namespace services::route
