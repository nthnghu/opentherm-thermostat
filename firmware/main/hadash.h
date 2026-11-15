#pragma once
// Home Assistant dashboard layout: pages of entity cards, stored in NVS ("hadash"/"layout").
#include <stdbool.h>

#define HADASH_MAX_PAGES     6
#define HADASH_MAX_PER_PAGE  12
#define HADASH_ID_LEN        64
#define HADASH_NAME_LEN      24

typedef struct {
    char name[HADASH_NAME_LEN];
    int count;
    char ids[HADASH_MAX_PER_PAGE][HADASH_ID_LEN];
} hadash_page_t;

typedef struct {
    int page_count;
    hadash_page_t pages[HADASH_MAX_PAGES];
} hadash_t;

// Loads the layout (migrates the old flat entity list into page "Home"). Always at least one page.
void hadash_load(hadash_t *d);
void hadash_save(const hadash_t *d);

// Edit helpers; each returns false when the change is not possible.
bool hadash_add(hadash_t *d, int page, const char *entity_id);
bool hadash_remove(hadash_t *d, int page, int idx);
bool hadash_move(hadash_t *d, int page, int idx, int delta);          // within the page
bool hadash_move_to_page(hadash_t *d, int page, int idx, int to_page); // appended there
bool hadash_add_page(hadash_t *d, const char *name);
bool hadash_delete_page(hadash_t *d, int page);                       // cards move to the previous page
bool hadash_contains(const hadash_t *d, const char *entity_id);
