// SPDX-License-Identifier: GPL-2.0-only
// ccm_json_parser_test.cpp

#include "../ccm_json_parser.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <cmath>

using namespace oiw;

static bool approx(float a, float b, float tol = 0.001f)
{
    return std::fabs(a - b) < tol;
}

static void writeTestJson(const char *path)
{
    std::ofstream f(path);
    f << R"({
  "calibration_date": "2026-03-24",
  "cell_id": "CELL1-SN001",
  "ccm": [
    { "nd_stops": 1.10,
      "matrix_3x3": [1.0,0.0,0.0, 0.0,1.0,0.0, 0.0,0.0,1.0],
      "gains_rggb":  [1.0,1.0,1.0,1.0] },
    { "nd_stops": 2.00,
      "matrix_3x3": [1.1,-0.05,-0.05, -0.05,1.1,-0.05, -0.05,-0.05,1.1],
      "gains_rggb":  [1.05,1.0,1.0,0.98] }
  ]
})";
}

static void test_parse_basic()
{
    const char *path = "/tmp/nd_test_ccm.json";
    writeTestJson(path);

    auto entries = parseCcmJson(path);
    assert(entries.size() == 2);

    // First entry: identity matrix
    assert(approx(entries[0].nd_stops, 1.10f));
    assert(approx(entries[0].matrix3x3[0], 1.0f));
    assert(approx(entries[0].matrix3x3[4], 1.0f));
    assert(approx(entries[0].matrix3x3[8], 1.0f));
    assert(approx(entries[0].gains_rggb[0], 1.0f));

    // Second entry
    assert(approx(entries[1].nd_stops, 2.00f));
    assert(approx(entries[1].matrix3x3[0], 1.1f));
    assert(approx(entries[1].matrix3x3[1], -0.05f));
    assert(approx(entries[1].gains_rggb[0], 1.05f));
    assert(approx(entries[1].gains_rggb[3], 0.98f));

    printf("PASS: test_parse_basic\n");
}

static void test_missing_file()
{
    auto entries = parseCcmJson("/tmp/does_not_exist.json");
    assert(entries.empty());
    printf("PASS: test_missing_file\n");
}

int main()
{
    test_parse_basic();
    test_missing_file();
    printf("All CCM JSON parser tests passed.\n");
    return 0;
}
