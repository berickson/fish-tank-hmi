#include "model.h"

void ReefState::format_now(char *out, size_t len) const {
  if (apex_epoch == 0) {
    snprintf(out, len, "now");
    return;
  }

  const uint32_t elapsed = (millis() - apex_epoch_ms) / 1000;
  // The Apex reports its clock already shifted into local time, so there is no
  // timezone maths to redo here -- just advance it by however long ago we asked.
  const uint32_t local = apex_epoch + elapsed;
  snprintf(out, len, "%02u:%02u", static_cast<unsigned>((local / 3600) % 24),
           static_cast<unsigned>((local / 60) % 60));
}

void ReefState::add_event(EventKind kind, const char *title, const char *detail) {
  // Collapse a repeat of the newest event rather than filling the ring with it.
  if (event_count > 0 && strcmp(events[0].title, title) == 0) {
    return;
  }

  if (event_count == max_events) {
    --event_count;
  }
  memmove(events + 1, events, sizeof(Event) * event_count);
  ++event_count;

  Event &event = events[0];
  event.kind = kind;
  snprintf(event.title, sizeof(event.title), "%s", title);
  snprintf(event.detail, sizeof(event.detail), "%s", detail);
  format_now(event.time, sizeof(event.time));
}
