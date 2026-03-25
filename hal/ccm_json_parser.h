// SPDX-License-Identifier: GPL-2.0-only
// ccm_json_parser.h — Minimal parser for calibration.json CCM table
//
// Expected JSON schema (INTEGRATION_MAP.md CCM_JSON_SCHEMA):
// {
//   "calibration_date": "...",
//   "cell_id": "...",
//   "ccm": [
//     { "nd_stops": 1.10,
//       "matrix_3x3": [r0c0,r0c1,r0c2, r1c0,r1c1,r1c2, r2c0,r2c1,r2c2],
//       "gains_rggb":  [R, Gr, Gb, B] },
//     ...
//   ]
// }
//
// Uses a simple recursive-descent parser to avoid a heavy JSON dependency
// in the vendor HAL partition.  Replace with nlohmann/json or libchrome
// if already available in the build.

#pragma once

#include "nd_hal_wrapper.h"   // for CcmEntry
#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#define LOG_TAG "CcmJsonParser"
#include <log/log.h>

namespace oiw {

// ── Tiny token scanner ────────────────────────────────────────── //
namespace detail {

static void skipWs(const char *&p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
}

static bool consume(const char *&p, char c)
{
    skipWs(p);
    if (*p != c) return false;
    p++;
    return true;
}

static float parseFloat(const char *&p)
{
    skipWs(p);
    char *end;
    float v = strtof(p, &end);
    p = end;
    return v;
}

static std::string parseString(const char *&p)
{
    skipWs(p);
    if (*p != '"') return {};
    p++;   // skip opening "
    const char *start = p;
    while (*p && *p != '"') p++;
    std::string s(start, p);
    if (*p == '"') p++;
    return s;
}

// Parse a JSON array of N floats into out[].
static bool parseFloatArray(const char *&p, float *out, int n)
{
    if (!consume(p, '[')) return false;
    for (int i = 0; i < n; i++) {
        out[i] = parseFloat(p);
        if (i < n - 1 && !consume(p, ',')) return false;
    }
    return consume(p, ']');
}

} // namespace detail

// ─────────────────────────────────────────────────────────────── //

inline std::vector<CcmEntry> parseCcmJson(const char *path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        ALOGE("parseCcmJson: cannot open %s", path);
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string buf = ss.str();
    const char *p   = buf.c_str();

    std::vector<CcmEntry> entries;

    // Find the "ccm" array
    const char *ccm_pos = strstr(p, "\"ccm\"");
    if (!ccm_pos) {
        ALOGE("parseCcmJson: no \"ccm\" key in %s", path);
        return {};
    }
    p = ccm_pos + 5;
    if (!detail::consume(p, ':') || !detail::consume(p, '['))
        return {};

    while (true) {
        detail::skipWs(p);
        if (*p == ']') break;         // end of array
        if (!detail::consume(p, '{')) break;

        CcmEntry entry{};
        bool has_stops = false, has_matrix = false, has_gains = false;

        // Parse object fields in any order
        for (int field = 0; field < 3; field++) {
            detail::skipWs(p);
            if (*p == '}') break;
            if (field > 0 && !detail::consume(p, ',')) break;

            std::string key = detail::parseString(p);
            if (!detail::consume(p, ':')) break;

            if (key == "nd_stops") {
                entry.nd_stops = detail::parseFloat(p);
                has_stops = true;
            } else if (key == "matrix_3x3") {
                has_matrix = detail::parseFloatArray(p, entry.matrix3x3.data(), 9);
            } else if (key == "gains_rggb") {
                has_gains  = detail::parseFloatArray(p, entry.gains_rggb.data(), 4);
            } else {
                // Skip unknown string or number value
                detail::skipWs(p);
                if (*p == '"') {
                    detail::parseString(p);
                } else {
                    detail::parseFloat(p);
                }
            }
        }

        detail::consume(p, '}');
        detail::skipWs(p);
        detail::consume(p, ',');

        if (has_stops && has_matrix && has_gains)
            entries.push_back(entry);
    }

    return entries;
}

} // namespace oiw
