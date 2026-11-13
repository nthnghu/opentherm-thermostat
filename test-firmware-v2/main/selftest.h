#pragma once
// Boot hardware test with a splash screen. Results stay available for Diagnostics and the web page.
#include <stdbool.h>

typedef enum { ST_PENDING, ST_PASS, ST_WARN, ST_FAIL } selftest_result_t;

typedef struct {
    const char *name;
    bool required;          // a failure stops at the boot screen ("Continue anyway")
    selftest_result_t result;
    char detail[96];
} selftest_item_t;

// Runs every test (also starts audio, Wi-Fi and the camera). Call with LVGL running, not locked.
// Returns true when all required parts passed.
bool selftest_run(void);
int selftest_items(const selftest_item_t **items);
bool selftest_required_ok(void);
