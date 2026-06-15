// ui.h — LVGL UI: usage bars, rate limits, info/status + touch controls.
#pragma once
#include <Arduino.h>
#include "claude.h"

void uiInit();

// Push a fresh fetch result into the widgets.
void uiUpdate(const UsageData &d);

// Network status line on the Info tab.
void uiSetNet(bool connected, const String &ip);

// Called ~once a second: seconds until the next auto-refresh (<0 = fetching now).
void uiTick(long secs_to_refresh);

// Show a transient state message (e.g. "Updating…", "Token updated").
void uiSetState(const char *msg);

// True (once) if the user tapped "Refresh now" since the last call.
bool uiTakeRefreshRequest();
