#include "hadash.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "nvs.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "hadash";

// Text format: one line per page, "Name|id1,id2,...".
static void parse(hadash_t *d, char *text)
{
    memset(d, 0, sizeof(*d));
    for (char *save, *line = strtok_r(text, "\n", &save); line && d->page_count < HADASH_MAX_PAGES;
         line = strtok_r(NULL, "\n", &save)) {
        hadash_page_t *p = &d->pages[d->page_count++];
        char *bar = strchr(line, '|');
        if (bar) {
            *bar = 0;
        }
        strlcpy(p->name, line, sizeof(p->name));
        if (!bar) {
            continue;
        }
        for (char *s2, *id = strtok_r(bar + 1, ",", &s2); id && p->count < HADASH_MAX_PER_PAGE; id = strtok_r(NULL, ",", &s2)) {
            strlcpy(p->ids[p->count++], id, HADASH_ID_LEN);
        }
    }
}

void hadash_load(hadash_t *d)
{
    char *text = calloc(1, 4096);
    size_t len = 4096;
    nvs_handle_t h;
    bool ok = false;
    if (text && nvs_open("hadash", NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_str(h, "layout", text, &len) == ESP_OK;
        nvs_close(h);
    }
    if (ok) {
        parse(d, text);
    } else {
        // First run: take over the flat list from the earlier version.
        memset(d, 0, sizeof(*d));
        d->page_count = 1;
        strlcpy(d->pages[0].name, "Home", HADASH_NAME_LEN);
        if (text) {
            strlcpy(text, settings_get()->ha_entities, 4096);
            for (char *s2, *id = strtok_r(text, ",", &s2); id; id = strtok_r(NULL, ",", &s2)) {
                hadash_add(d, 0, id);
            }
        }
    }
    if (d->page_count == 0) {
        d->page_count = 1;
        strlcpy(d->pages[0].name, "Home", HADASH_NAME_LEN);
    }
    free(text);
}

void hadash_save(const hadash_t *d)
{
    char *text = calloc(1, 4096);
    if (!text) {
        return;
    }
    for (int i = 0; i < d->page_count; i++) {
        const hadash_page_t *p = &d->pages[i];
        strlcat(text, p->name, 4096);
        strlcat(text, "|", 4096);
        for (int k = 0; k < p->count; k++) {
            if (k) {
                strlcat(text, ",", 4096);
            }
            strlcat(text, p->ids[k], 4096);
        }
        strlcat(text, "\n", 4096);
    }
    nvs_handle_t h;
    if (nvs_open("hadash", NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_set_str(h, "layout", text);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        }
    }
    free(text);
}

bool hadash_contains(const hadash_t *d, const char *id)
{
    for (int i = 0; i < d->page_count; i++) {
        for (int k = 0; k < d->pages[i].count; k++) {
            if (strcmp(d->pages[i].ids[k], id) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool hadash_add(hadash_t *d, int page, const char *id)
{
    if (page < 0 || page >= d->page_count || !id[0] || strlen(id) >= HADASH_ID_LEN) {
        return false;
    }
    hadash_page_t *p = &d->pages[page];
    if (p->count >= HADASH_MAX_PER_PAGE || hadash_contains(d, id)) {
        return false;
    }
    strlcpy(p->ids[p->count++], id, HADASH_ID_LEN);
    return true;
}

bool hadash_remove(hadash_t *d, int page, int idx)
{
    if (page < 0 || page >= d->page_count || idx < 0 || idx >= d->pages[page].count) {
        return false;
    }
    hadash_page_t *p = &d->pages[page];
    memmove(p->ids[idx], p->ids[idx + 1], (size_t)(p->count - idx - 1) * HADASH_ID_LEN);
    p->count--;
    return true;
}

bool hadash_move(hadash_t *d, int page, int idx, int delta)
{
    if (page < 0 || page >= d->page_count) {
        return false;
    }
    hadash_page_t *p = &d->pages[page];
    int to = idx + delta;
    if (idx < 0 || idx >= p->count || to < 0 || to >= p->count) {
        return false;
    }
    char tmp[HADASH_ID_LEN];
    memcpy(tmp, p->ids[idx], HADASH_ID_LEN);
    memcpy(p->ids[idx], p->ids[to], HADASH_ID_LEN);
    memcpy(p->ids[to], tmp, HADASH_ID_LEN);
    return true;
}

bool hadash_move_to_page(hadash_t *d, int page, int idx, int to_page)
{
    if (page < 0 || page >= d->page_count || to_page < 0 || to_page >= d->page_count || to_page == page ||
        idx < 0 || idx >= d->pages[page].count || d->pages[to_page].count >= HADASH_MAX_PER_PAGE) {
        return false;
    }
    char id[HADASH_ID_LEN];
    strlcpy(id, d->pages[page].ids[idx], sizeof(id));
    hadash_remove(d, page, idx);
    strlcpy(d->pages[to_page].ids[d->pages[to_page].count++], id, HADASH_ID_LEN);
    return true;
}

bool hadash_add_page(hadash_t *d, const char *name)
{
    if (d->page_count >= HADASH_MAX_PAGES) {
        return false;
    }
    hadash_page_t *p = &d->pages[d->page_count++];
    memset(p, 0, sizeof(*p));
    strlcpy(p->name, name && name[0] ? name : "Page", sizeof(p->name));
    return true;
}

bool hadash_delete_page(hadash_t *d, int page)
{
    if (d->page_count <= 1 || page < 0 || page >= d->page_count) {
        return false;
    }
    int to = page > 0 ? page - 1 : 1;
    hadash_page_t *src = &d->pages[page];
    for (int k = 0; k < src->count && d->pages[to].count < HADASH_MAX_PER_PAGE; k++) {
        strlcpy(d->pages[to].ids[d->pages[to].count++], src->ids[k], HADASH_ID_LEN);
    }
    memmove(&d->pages[page], &d->pages[page + 1], sizeof(hadash_page_t) * (d->page_count - page - 1));
    d->page_count--;
    return true;
}
