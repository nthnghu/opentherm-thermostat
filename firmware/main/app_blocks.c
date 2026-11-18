// Blocks: a simple falling-blocks puzzle game (10 x 20 board, touch controls).
#include <string.h>
#include "esp_random.h"
#include "board.h"
#include "ui.h"

#define COLS     10
#define ROWS     20
#define CELL     28
#define BOARD_X  14
#define BOARD_Y  10
#define PREVIEW_CELL 22

// 7 pieces x 4 rotations, each a 4x4 bitmask (bit 15 = row 0, col 0).
static const uint16_t PIECES[7][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444},  // I
    {0x8E00, 0x6440, 0x0E20, 0x44C0},  // J
    {0x2E00, 0x4460, 0x0E80, 0xC440},  // L
    {0x6600, 0x6600, 0x6600, 0x6600},  // O
    {0x6C00, 0x4620, 0x06C0, 0x8C40},  // S
    {0x4E00, 0x4640, 0x0E40, 0x4C40},  // T
    {0xC600, 0x2640, 0x0C60, 0x4C80},  // Z
};
static const uint32_t COLORS[8] = {
    0x263238,                                                      // empty
    0x00BCD4, 0x3F51B5, 0xFF9800, 0xFFEB3B, 0x4CAF50, 0x9C27B0, 0xF44336,
};

static uint8_t s_grid[ROWS][COLS];  // 0 = empty, 1..7 = piece colour
static int s_piece, s_rot, s_px, s_py, s_next;
static int s_score, s_lines, s_level;
static bool s_over;

static lv_obj_t *s_cells[ROWS][COLS];
static uint8_t s_drawn[ROWS][COLS];
static lv_obj_t *s_preview[4][4];
static lv_obj_t *s_score_lbl, *s_over_box;
static lv_timer_t *s_timer;

static inline bool cell_on(uint16_t mask, int r, int c)
{
    return mask & (0x8000 >> (r * 4 + c));
}

static bool fits(int piece, int rot, int x, int y)
{
    uint16_t m = PIECES[piece][rot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!cell_on(m, r, c)) {
                continue;
            }
            int gx = x + c, gy = y + r;
            if (gx < 0 || gx >= COLS || gy >= ROWS) {
                return false;
            }
            if (gy >= 0 && s_grid[gy][gx]) {
                return false;
            }
        }
    }
    return true;
}

static void render(void)
{
    uint8_t frame[ROWS][COLS];
    memcpy(frame, s_grid, sizeof(frame));
    if (!s_over) {
        uint16_t m = PIECES[s_piece][s_rot];
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                int gy = s_py + r, gx = s_px + c;
                if (cell_on(m, r, c) && gy >= 0 && gy < ROWS && gx >= 0 && gx < COLS) {
                    frame[gy][gx] = s_piece + 1;
                }
            }
        }
    }
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (frame[r][c] != s_drawn[r][c]) {
                s_drawn[r][c] = frame[r][c];
                lv_obj_set_style_bg_color(s_cells[r][c], lv_color_hex(COLORS[frame[r][c]]), 0);
            }
        }
    }
    uint16_t nm = PIECES[s_next][0];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            lv_obj_set_style_bg_color(s_preview[r][c], lv_color_hex(cell_on(nm, r, c) ? COLORS[s_next + 1] : COLORS[0]), 0);
        }
    }
    lv_label_set_text_fmt(s_score_lbl, "Score\n%d\n\nLines\n%d\n\nLevel\n%d", s_score, s_lines, s_level + 1);
}

static void set_speed(void)
{
    int ms = 800 - s_level * 70;
    lv_timer_set_period(s_timer, ms < 100 ? 100 : ms);
}

static void spawn(void)
{
    s_piece = s_next;
    s_next = esp_random() % 7;
    s_rot = 0;
    s_px = 3;
    s_py = -1;
    if (!fits(s_piece, s_rot, s_px, s_py)) {
        s_over = true;
        lv_timer_pause(s_timer);
        lv_obj_set_hidden(s_over_box, false);
    }
}

static void lock_piece(void)
{
    uint16_t m = PIECES[s_piece][s_rot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int gy = s_py + r, gx = s_px + c;
            if (cell_on(m, r, c) && gy >= 0) {
                s_grid[gy][gx] = s_piece + 1;
            }
        }
    }
    int cleared = 0;
    for (int r = ROWS - 1; r >= 0; r--) {
        bool full = true;
        for (int c = 0; c < COLS; c++) {
            full &= s_grid[r][c] != 0;
        }
        if (full) {
            memmove(&s_grid[1], &s_grid[0], r * COLS);
            memset(s_grid[0], 0, COLS);
            cleared++;
            r++;  // re-check the same row index
        }
    }
    static const int points[5] = {0, 100, 300, 500, 800};
    s_score += points[cleared] * (s_level + 1);
    s_lines += cleared;
    if (s_lines / 10 != s_level) {
        s_level = s_lines / 10;
        set_speed();
    }
    spawn();
}

static bool step_down(void)
{
    if (fits(s_piece, s_rot, s_px, s_py + 1)) {
        s_py++;
        return true;
    }
    lock_piece();
    return false;
}

static void tick_cb(lv_timer_t *t)
{
    if (!s_over) {
        step_down();
        render();
    }
}

typedef enum { ACT_LEFT, ACT_RIGHT, ACT_ROTATE, ACT_DOWN, ACT_DROP } action_t;

static void action_cb(lv_event_t *e)
{
    if (s_over) {
        return;
    }
    action_t a = (action_t)(intptr_t)lv_event_get_user_data(e);
    switch (a) {
    case ACT_LEFT:
        if (fits(s_piece, s_rot, s_px - 1, s_py)) {
            s_px--;
        }
        break;
    case ACT_RIGHT:
        if (fits(s_piece, s_rot, s_px + 1, s_py)) {
            s_px++;
        }
        break;
    case ACT_ROTATE: {
        int nr = (s_rot + 1) % 4;
        static const int kicks[] = {0, -1, 1, -2, 2};
        for (int i = 0; i < 5; i++) {
            if (fits(s_piece, nr, s_px + kicks[i], s_py)) {
                s_rot = nr;
                s_px += kicks[i];
                break;
            }
        }
        break;
    }
    case ACT_DOWN:
        if (step_down()) {
            s_score += 1;
        }
        break;
    case ACT_DROP:
        while (fits(s_piece, s_rot, s_px, s_py + 1)) {
            s_py++;
            s_score += 2;
        }
        lock_piece();
        break;
    }
    render();
}

static void new_game(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    s_score = s_lines = s_level = 0;
    s_over = false;
    s_next = esp_random() % 7;
    lv_obj_set_hidden(s_over_box, true);
    set_speed();
    spawn();
    lv_timer_resume(s_timer);
    render();
}

static void new_game_cb(lv_event_t *e)
{
    new_game();
}

static void screen_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        if (!s_over) {
            lv_timer_resume(s_timer);
        }
    } else {
        lv_timer_pause(s_timer);
    }
}

static lv_obj_t *make_cell(lv_obj_t *parent, int x, int y, int size)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, size - 2, size - 2);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COLORS[0]), 0);
    lv_obj_set_style_radius(o, 3, 0);
    lv_obj_set_clickable(o, false);
    return o;
}

static void add_control(lv_obj_t *parent, const char *text, action_t a, int x, int y, int w, bool repeat)
{
    lv_obj_t *b = ui_button(parent, text, lv_color_hex(0x455A64), NULL, NULL);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 100);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_28, 0);
    lv_obj_add_event_cb(b, action_cb, LV_EVENT_PRESSED, (void *)(intptr_t)a);
    if (repeat) {
        lv_obj_add_event_cb(b, action_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)a);
    }
}

lv_obj_t *app_blocks_create(void)
{
    lv_obj_t *content;
    lv_obj_t *scr = ui_app_screen("Blocks", &content);
    lv_obj_add_event_cb(scr, screen_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr, screen_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);

    // Board frame + cells
    lv_obj_t *frame = lv_obj_create(content);
    lv_obj_remove_style_all(frame);
    lv_obj_set_pos(frame, BOARD_X - 4, BOARD_Y - 4);
    lv_obj_set_size(frame, COLS * CELL + 6, ROWS * CELL + 6);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(frame, 6, 0);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            s_cells[r][c] = make_cell(frame, 4 + c * CELL, 4 + r * CELL, CELL);
        }
    }

    // Side panel: next piece + score
    const int side_x = BOARD_X + COLS * CELL + 18;
    lv_obj_t *nl = lv_label_create(content);
    lv_label_set_text(nl, "Next");
    lv_obj_set_pos(nl, side_x, BOARD_Y);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            s_preview[r][c] = make_cell(content, side_x + c * PREVIEW_CELL, BOARD_Y + 34 + r * PREVIEW_CELL, PREVIEW_CELL);
        }
    }
    s_score_lbl = lv_label_create(content);
    lv_obj_set_pos(s_score_lbl, side_x, BOARD_Y + 150);

    lv_obj_t *ng = ui_button(content, LV_SYMBOL_REFRESH " New", lv_color_hex(0x6A1B9A), new_game_cb, NULL);
    lv_obj_set_size(ng, 140, 60);
    lv_obj_set_pos(ng, side_x - 4, BOARD_Y + ROWS * CELL - 60);

    // Controls
    const int cy = BOARD_Y + ROWS * CELL + 14;
    add_control(content, LV_SYMBOL_LEFT, ACT_LEFT, 8, cy, 84, true);
    add_control(content, LV_SYMBOL_RIGHT, ACT_RIGHT, 100, cy, 84, true);
    add_control(content, LV_SYMBOL_REFRESH, ACT_ROTATE, 192, cy, 92, false);
    add_control(content, LV_SYMBOL_DOWN, ACT_DOWN, 292, cy, 84, true);
    add_control(content, LV_SYMBOL_DOWNLOAD, ACT_DROP, 384, cy, 88, false);

    // Game-over overlay
    s_over_box = lv_obj_create(content);
    lv_obj_set_size(s_over_box, 300, 220);
    lv_obj_align(s_over_box, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_scrollable(s_over_box, false);
    lv_obj_t *gl = lv_label_create(s_over_box);
    lv_obj_set_style_text_font(gl, &lv_font_montserrat_28, 0);
    lv_label_set_text(gl, "Game over");
    lv_obj_align(gl, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t *again = ui_button(s_over_box, "Play again", lv_color_hex(0x6A1B9A), new_game_cb, NULL);
    lv_obj_set_size(again, 220, 70);
    lv_obj_align(again, LV_ALIGN_BOTTOM_MID, 0, -10);

    memset(s_drawn, 0, sizeof(s_drawn));
    s_timer = lv_timer_create(tick_cb, 800, NULL);
    lv_timer_pause(s_timer);
    new_game();
    lv_timer_pause(s_timer);  // starts when the screen opens
    return scr;
}
