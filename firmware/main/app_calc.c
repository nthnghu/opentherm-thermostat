// Calculator: + - x / with precedence, %, ±, backspace. Expression line + result, double precision.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "ui.h"
#include "ui_theme.h"

static lv_obj_t *s_expr_lbl, *s_res_lbl;
static char s_expr[64];
static bool s_done;  // last key was "=": a digit starts a new expression

/* --------------------------------------------------------------- evaluate */

typedef struct {
    const char *p;
    bool err;
} parser_t;

static double parse_sum(parser_t *ps);

// number [%]  |  -factor
static double parse_factor(parser_t *ps)
{
    if (*ps->p == '-') {
        ps->p++;
        return -parse_factor(ps);
    }
    char *end;
    double v = strtod(ps->p, &end);
    if (end == ps->p) {
        ps->err = true;
        return 0;
    }
    ps->p = end;
    if (*ps->p == '%') {
        ps->p++;
        v /= 100.0;
    }
    return v;
}

static double parse_product(parser_t *ps)
{
    double v = parse_factor(ps);
    while (!ps->err && (*ps->p == '*' || *ps->p == '/')) {
        char op = *ps->p++;
        double r = parse_factor(ps);
        if (op == '*') {
            v *= r;
        } else if (r == 0) {
            ps->err = true;
        } else {
            v /= r;
        }
    }
    return v;
}

static double parse_sum(parser_t *ps)
{
    double v = parse_product(ps);
    while (!ps->err && (*ps->p == '+' || *ps->p == '-')) {
        char op = *ps->p++;
        double r = parse_product(ps);
        v = op == '+' ? v + r : v - r;
    }
    return v;
}

static bool evaluate(const char *expr, double *out)
{
    parser_t ps = {.p = expr};
    double v = parse_sum(&ps);
    if (ps.err || *ps.p || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static void format_num(double v, char *buf, size_t n)
{
    if (fabs(v) < 1e-12) {
        v = 0;
    }
    snprintf(buf, n, "%.10g", v);
}

/* --------------------------------------------------------------------- UI */

static void show(void)
{
    // Show * as x (the built-in fonts have no multiplication sign)
    char disp[64];
    strlcpy(disp, s_expr, sizeof(disp));
    for (char *p = disp; *p; p++) {
        if (*p == '*') {
            *p = 'x';
        }
    }
    lv_label_set_text(s_expr_lbl, disp[0] ? disp : "0");
    double v;
    if (s_expr[0] && evaluate(s_expr, &v)) {
        char r[40];
        format_num(v, r, sizeof(r));
        lv_label_set_text_fmt(s_res_lbl, "= %s", r);
    } else {
        lv_label_set_text(s_res_lbl, "");
    }
}

static bool is_op(char c)
{
    return c == '+' || c == '-' || c == '*' || c == '/';
}

// Start of the number the cursor is in (after the last operator, keeping a unary minus).
static char *last_number(void)
{
    char *p = s_expr + strlen(s_expr);
    while (p > s_expr && !is_op(p[-1])) {
        p--;
    }
    if (p > s_expr && p[-1] == '-' && (p - 1 == s_expr || is_op(p[-2]))) {
        p--;
    }
    return p;
}

static void append(const char *s)
{
    if (strlen(s_expr) + strlen(s) < sizeof(s_expr) - 1) {
        strcat(s_expr, s);
    }
}

static void key_cb(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target(e);
    const char *k = lv_buttonmatrix_get_button_text(m, lv_buttonmatrix_get_selected_button(m));
    if (!k) {
        return;
    }
    size_t n = strlen(s_expr);
    char last = n ? s_expr[n - 1] : 0;

    if (strcmp(k, "C") == 0) {
        s_expr[0] = 0;
    } else if (strcmp(k, LV_SYMBOL_BACKSPACE) == 0) {
        if (n) {
            s_expr[n - 1] = 0;
        }
    } else if (strcmp(k, "=") == 0) {
        double v;
        if (!n) {
            return;
        }
        if (evaluate(s_expr, &v)) {
            format_num(v, s_expr, sizeof(s_expr));
            s_done = true;
            show();
            lv_label_set_text(s_res_lbl, "");
        } else {
            lv_label_set_text_fmt(s_res_lbl, "#%06lX Error#", (unsigned long)ui_hex(UI_ERR));
        }
        return;
    } else if (strcmp(k, "+/-") == 0) {
        char *num = last_number();
        if (*num == '-') {
            memmove(num, num + 1, strlen(num));
        } else if (*num && strlen(s_expr) + 1 < sizeof(s_expr) - 1) {  // same limit as append()
            memmove(num + 1, num, strlen(num) + 1);
            *num = '-';
        }
    } else if (strcmp(k, "%") == 0) {
        if (n && last >= '0' && last <= '9') {
            append("%");
        }
    } else if (strcmp(k, ".") == 0) {
        if (s_done) {
            s_expr[0] = 0;
        }
        char *num = last_number();
        if (!strchr(num, '.') && !strchr(num, '%')) {
            append(num[0] == 0 || num[strlen(num) - 1] == '-' ? "0." : ".");
        }
    } else {
        const char *op = strcmp(k, "x") == 0 ? "*" : k;
        if (is_op(op[0])) {
            if (!n) {
                if (op[0] == '-') {
                    append("-");
                }
            } else if (is_op(last)) {
                s_expr[n - 1] = op[0];  // replace the previous operator
            } else {
                append(op);
            }
        } else {  // digit
            if (s_done) {
                s_expr[0] = 0;
            }
            if (last != '%') {
                append(op);
            }
        }
    }
    s_done = false;
    show();
}

lv_obj_t *app_calc_create(void)
{
    lv_obj_t *content;
    lv_obj_t *scr = ui_app_screen("Calculator", &content);

    lv_obj_t *disp = lv_obj_create(content);
    lv_obj_set_size(disp, 448, 150);
    lv_obj_set_pos(disp, 16, 12);
    lv_obj_set_style_bg_color(disp, ui_c(0x0D141B), 0);
    lv_obj_set_scrollable(disp, false);
    s_expr_lbl = lv_label_create(disp);
    lv_obj_set_style_text_font(s_expr_lbl, &lv_font_montserrat_40, 0);
    lv_obj_set_width(s_expr_lbl, 420);
    lv_label_set_long_mode(s_expr_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_expr_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_expr_lbl, LV_ALIGN_TOP_RIGHT, 0, 4);
    s_res_lbl = lv_label_create(disp);
    lv_label_set_recolor(s_res_lbl, true);
    lv_obj_set_style_text_font(s_res_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_res_lbl, ui_c(0x90A4AE), 0);
    lv_obj_align(s_res_lbl, LV_ALIGN_BOTTOM_RIGHT, 0, -4);

    static const char *map[] = {
        "C", LV_SYMBOL_BACKSPACE, "%", "/", "\n",
        "7", "8", "9", "x", "\n",
        "4", "5", "6", "-", "\n",
        "1", "2", "3", "+", "\n",
        "+/-", "0", ".", "=", "",
    };
    lv_obj_t *m = lv_buttonmatrix_create(content);
    lv_buttonmatrix_set_map(m, map);
    lv_obj_set_size(m, 464, 520);
    lv_obj_set_pos(m, 8, 172);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(m, key_cb, LV_EVENT_VALUE_CHANGED, NULL);
    show();
    return scr;
}
