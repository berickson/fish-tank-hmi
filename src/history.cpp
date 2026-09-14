#include "history.h"

#include <HTTPClient.h>
#include <time.h>

#include "apex.h"

namespace {

// A little over the window, so the oldest visible column is fully covered even
// though the Apex logs on its own 10-minute schedule.
constexpr uint32_t backfill_span_s = 25UL * 3600;
// Enough to cover a dropped link plus the poll that noticed it. A few KB.
constexpr uint32_t topup_span_s = 30UL * 60;

// How long a single pump may spend draining the socket. The whole point of this
// module is that the panel keeps painting and responding while it runs, so this
// is a frame budget, not a throughput target -- but it has to be large enough
// that the socket is drained faster than the Apex fills it, or the TCP window
// closes and the transfer crawls. 10 ms is still far quicker than one full-frame
// flush on this panel.
constexpr uint32_t pump_budget_ms = 20;
constexpr size_t read_chunk = 1024;

// A stalled transfer must not hold the connection open forever.
constexpr uint32_t transfer_timeout_ms = 40000;
// The request itself has to answer promptly even though the body that follows
// takes seconds. Measured time-to-first-byte on this Apex is under 100 ms.
constexpr uint16_t http_fetch_timeout_ms = 8000;
// The chart is redrawn as history lands, but every redraw costs a full-frame
// flush on this panel, so a day of backfill is shown filling in at a handful of
// frames a second rather than once per slice.
constexpr uint32_t redraw_interval_ms = 750;

enum class Phase : uint8_t { idle, wanted, streaming };

// The datalog is much too big to hold in RAM, so it is scanned as a byte stream
// rather than parsed. The shape we need out of it never varies:
//
//   {"date":1789369740,"data":[{"name":"Tmp","did":"base_Temp","type":"Temp",
//                               "value":"80.8"}, ...]}
//
// so remembering the latest "date", the latest "name", and applying each
// "value" that follows a name we care about is enough. Every other key in the
// response streams past unread. The Apex repeats timestamps and interleaves
// twenty other channels; none of that needs handling, because a sample is
// placed by its own timestamp and merged into whatever column it lands in.
const char pattern_date[] = "\"date\":";
const char pattern_name[] = "\"name\":\"";
const char pattern_value[] = "\"value\":\"";
const char *const patterns[3] = {pattern_date, pattern_name, pattern_value};

enum class Capture : uint8_t { none, date, name, value };
const Capture capture_for[3] = {Capture::date, Capture::name, Capture::value};

Phase phase = Phase::idle;
uint32_t requested_span_s = 0;
HTTPClient http;
WiFiClient *stream = nullptr;
uint32_t started_ms = 0;
uint32_t last_redraw_ms = 0;
uint32_t bytes_read = 0;
uint32_t samples_applied = 0;

uint8_t progress[3];
Capture capturing = Capture::none;
char token[24];
uint8_t token_len = 0;
uint32_t record_epoch = 0;
int8_t pending_probe = -1;

Reading *reading_for(ReefState &state, int8_t probe) {
  switch (probe) {
    case 0:
      return &state.temperature;
    case 1:
      return &state.ph;
    case 2:
      return &state.salinity;
    default:
      return nullptr;
  }
}

int8_t probe_index(const char *name) {
  if (strcmp(name, "Tmp") == 0) {
    return 0;
  }
  if (strcmp(name, "pH") == 0) {
    return 1;
  }
  if (strcmp(name, "Salt") == 0) {
    return 2;
  }
  return -1;
}

void reset_scanner() {
  for (uint8_t i = 0; i < 3; ++i) {
    progress[i] = 0;
  }
  capturing = Capture::none;
  token_len = 0;
  record_epoch = 0;
  pending_probe = -1;
}

void finish_token(ReefState &state) {
  token[token_len] = '\0';
  switch (capturing) {
    case Capture::date:
      record_epoch = strtoul(token, nullptr, 10);
      break;
    case Capture::name:
      pending_probe = probe_index(token);
      break;
    case Capture::value: {
      Reading *reading = reading_for(state, pending_probe);
      if (reading != nullptr) {
        reading->trend.add(record_epoch, atof(token));
        ++samples_applied;
      }
      pending_probe = -1;
      break;
    }
    default:
      break;
  }
  capturing = Capture::none;
}

void scan_byte(ReefState &state, char byte) {
  if (capturing == Capture::date) {
    if (byte >= '0' && byte <= '9') {
      if (token_len < sizeof(token) - 1) {
        token[token_len++] = byte;
      }
      return;
    }
    finish_token(state);
    // The digits end on a comma, which cannot start a pattern, but falling
    // through costs nothing and keeps the matcher honest.
  } else if (capturing != Capture::none) {
    if (byte != '"') {
      if (token_len < sizeof(token) - 1) {
        token[token_len++] = byte;
      }
      return;
    }
    finish_token(state);
    return;
  }

  for (uint8_t i = 0; i < 3; ++i) {
    if (byte == patterns[i][progress[i]]) {
      if (patterns[i][++progress[i]] == '\0') {
        capturing = capture_for[i];
        token_len = 0;
        for (uint8_t j = 0; j < 3; ++j) {
          progress[j] = 0;
        }
        return;
      }
      continue;
    }
    // A failed match can still be the start of the next one.
    progress[i] = (byte == patterns[i][0]) ? 1 : 0;
  }
}

void stop(ReefState &state, const char *why) {
  if (phase == Phase::streaming) {
    http.end();
    const uint32_t elapsed_ms = max(millis() - started_ms, 1UL);
    Serial.printf("Datalog %s: %u B, %u samples, %u ms (%u B/s)\n", why,
                  static_cast<unsigned>(bytes_read), static_cast<unsigned>(samples_applied),
                  static_cast<unsigned>(elapsed_ms),
                  static_cast<unsigned>((bytes_read * 1000UL) / elapsed_ms));
  }
  stream = nullptr;
  phase = Phase::idle;
  requested_span_s = 0;
  if (samples_applied > 0) {
    ++state.revision;
  }
}

// `sdate` is the Apex's own local time, whereas the timestamps inside the
// response are true Unix epochs. Mixing those up costs you a whole timezone's
// worth of history, so the offset is applied here and nowhere else.
void format_sdate(char *out, size_t len, uint32_t epoch, int32_t tz_offset_s) {
  const time_t local = static_cast<time_t>(epoch) + tz_offset_s;
  struct tm parts;
  gmtime_r(&local, &parts);
  snprintf(out, len, "%02d%02d%02d%02d%02d", parts.tm_year % 100, parts.tm_mon + 1, parts.tm_mday,
           parts.tm_hour, parts.tm_min);
}

bool start_fetch(ReefState &state) {
  const uint32_t now = state.now_epoch();
  if (now == 0) {
    return false;  // no Apex clock yet; try again after the next poll
  }

  char sdate[12];
  format_sdate(sdate, sizeof(sdate), now - requested_span_s, state.tz_offset_s);

  // No `days` parameter: the Apex sends everything from sdate to now, which is
  // exactly what both the backfill and the top-up want.
  if (!apex_begin_request(http, String("/cgi-bin/datalog.json?sdate=") + sdate)) {
    return false;
  }
  // The transfer is long by design, but it is drained a slice at a time; only
  // the request itself needs to answer promptly.
  http.setTimeout(http_fetch_timeout_ms);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Datalog request failed, code: %d\n", code);
    http.end();
    return false;
  }

  // A full backfill arrives oldest-first, so pin the right edge to now before
  // any of it lands. Otherwise the window would open a day in the past and
  // scroll forward as the response streams in, which looks like the chart is
  // running away.
  state.temperature.trend.anchor(now);
  state.ph.trend.anchor(now);
  state.salinity.trend.anchor(now);

  stream = http.getStreamPtr();
  phase = Phase::streaming;
  started_ms = millis();
  last_redraw_ms = started_ms;
  bytes_read = 0;
  samples_applied = 0;
  reset_scanner();
  Serial.printf("Datalog fetch started from %s\n", sdate);
  return true;
}

}  // namespace

void history_request_backfill() {
  if (phase != Phase::idle) {
    return;
  }
  phase = Phase::wanted;
  requested_span_s = backfill_span_s;
}

void history_request_topup() {
  if (phase != Phase::idle) {
    return;
  }
  phase = Phase::wanted;
  requested_span_s = topup_span_s;
}

bool history_busy() { return phase != Phase::idle; }

void history_pump(ReefState &state) {
  if (phase == Phase::wanted) {
    if (!start_fetch(state)) {
      phase = Phase::idle;
      requested_span_s = 0;
    }
    return;  // the request itself was this pump's work
  }

  if (phase != Phase::streaming) {
    return;
  }

  if (millis() - started_ms > transfer_timeout_ms) {
    stop(state, "timed out");
    return;
  }

  char buffer[read_chunk];
  const uint32_t deadline = millis() + pump_budget_ms;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    const int available = stream->available();
    if (available <= 0) {
      // No more bytes and the far end has hung up: that is the end of the body.
      if (!stream->connected()) {
        stop(state, "complete");
        return;
      }
      // Still connected, just nothing buffered this instant. Giving the pump up
      // here is what made the transfer crawl: the socket would only be read once
      // per loop pass, which is slower than the Apex sends. Spend the rest of the
      // budget waiting for the next segment instead.
      delay(1);
      continue;
    }

    const size_t want = min(static_cast<size_t>(available), sizeof(buffer));
    const int got = stream->readBytes(buffer, want);
    if (got <= 0) {
      return;
    }
    bytes_read += got;
    for (int i = 0; i < got; ++i) {
      scan_byte(state, buffer[i]);
    }
  }

  // Ran out of frame budget with bytes still waiting; the rest lands next loop.
  const uint32_t now_ms = millis();
  if (now_ms - last_redraw_ms >= redraw_interval_ms) {
    last_redraw_ms = now_ms;
    ++state.revision;
  }
}
