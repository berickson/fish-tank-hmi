#pragma once

#include "model.h"

// Fills the 24-hour Home sparklines from the Apex's own datalog, so a freshly
// booted panel shows a day of history instead of taking a day to draw one.
//
// /cgi-bin/datalog.json?sdate=YYMMDDHHMM returns everything from that local
// timestamp to now. A full day is ~725 KB, which is far too much to hold in RAM
// and far too slow to sit through, so it is read a slice at a time from loop()
// and scanned as it arrives.

// Ask for a full 24-hour backfill. Ignored if one is already running.
void history_request_backfill();

// Ask for the last half hour only, to heal a gap after a dropped link. A few KB.
void history_request_topup();

// Do one slice of whatever was asked for; returns immediately when idle. Call
// once per loop(), after lv_timer_handler().
void history_pump(ReefState &state);

// True while a fetch is in flight.
bool history_busy();
