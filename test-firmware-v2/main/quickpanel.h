#pragma once
// Pull-down quick panel from the status bar.
#include <stdbool.h>

void quickpanel_create(void);  // before the status bar, so the bar stays on top
void quickpanel_open(void);
void quickpanel_close(void);
bool quickpanel_is_open(void);
