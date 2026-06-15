// claude.h — auth-blob storage + claude.ai usage/limits fetching.
#pragma once
#include <Arduino.h>

// One utilisation window (5h, 7d, per-model, ...).
struct UsageWindow {
  bool present = false;
  float util = 0.0f;     // 0..100
  long resets_in = -1;   // seconds until reset, -1 = unknown
};

struct ExtraUsage {
  bool enabled = false;
  float util = -1.0f;    // percent, -1 = unknown
  float used = -1.0f;    // currency units (e.g. EUR)
  float total = -1.0f;
  char currency[8] = "";
};

struct RateLimit {
  char model_group[40] = "";
  char limiter[24] = "";
  long value = 0;
};

#define MAX_LIMITS 16

struct UsageData {
  bool valid = false;
  char name[40] = "";
  char org_name[48] = "";
  UsageWindow five_hour, seven_day, opus, sonnet;
  ExtraUsage extra;
  RateLimit limits[MAX_LIMITS];
  int n_limits = 0;
  long fetched_at = 0;       // epoch seconds, 0 = never
  int http_status = 0;       // last HTTP code (0 = none)
  char error[96] = "";       // human-readable error, "" = ok
};

// ---- auth blob (pushed from the PC over serial, stored in NVS) --------------
bool claudeLoadToken();                 // load NVS -> in-memory config; false if none
bool claudeSaveBlob(const String &json);// validate + persist a compact-JSON blob
bool claudeHasToken();
const char *claudeOrgName();            // "" until a token is loaded
const char *claudeUserName();

// ---- fetching ---------------------------------------------------------------
// Performs the /usage (+ /rate_limits) calls and fills `out`. Returns true on a
// fully successful fetch; on failure out.error/out.http_status describe why.
bool claudeFetch(UsageData &out);
