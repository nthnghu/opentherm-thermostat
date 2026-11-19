// Text input (textparse.c): REVIEW.md T4 (MQTT setpoint) and T14 (web form fields).
#include <string.h>
#include "test.h"
#include "textparse.h"

static void test_t4_setpoint(void)
{
    struct {
        const char *in;
        bool ok;
        float v;
    } cases[] = {
        {"21.5", true, 21.5f}, {" 21.5 ", true, 21.5f}, {"5", true, 5}, {"30", true, 30}, {"4.9", false, 0},
        {"30.1", false, 0},    {"abc", false, 0},       {"", false, 0}, {"21.5xyz", false, 0}, {"nan", false, 0},
        {"inf", false, 0},     {"1e9", false, 0},       {"2e1", false, 0}, {"0x15", false, 0}, {"21,5", false, 0},
        {"-", false, 0},       {"21.5 22", false, 0},
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float v = -99;
        bool ok = tp_parse_float(cases[i].in, 5, 30, &v);
        if (ok != cases[i].ok || (ok && v != cases[i].v) || (!ok && v != -99)) {
            printf("   setpoint \"%s\": %s %.2f\n", cases[i].in, ok ? "accepted" : "refused", v);
            bad++;
        }
    }
    CHECK(bad == 0, "setpoint texts: in-range numbers only, nothing after them, value untouched on refusal");
    CHECK(!tp_parse_float(NULL, 5, 30, &(float){0}), "NULL refused");
}

static void test_url_decode(void)
{
    char s[64];
    strcpy(s, "a+b%20c%2Bd%3d");
    tp_url_decode(s);
    CHECK(strcmp(s, "a b c+d=") == 0, "decode: '%s'", s);
    strcpy(s, "100%");
    tp_url_decode(s);
    CHECK(strcmp(s, "100%") == 0, "'%%' at the end stays");
    strcpy(s, "50%2");
    tp_url_decode(s);
    CHECK(strcmp(s, "50%2") == 0, "'%%' with one hex digit stays");
    strcpy(s, "%zz%41");
    tp_url_decode(s);
    CHECK(strcmp(s, "%zzA") == 0, "bad escape kept, the next one decoded: '%s'", s);
}

static void test_t14_form(void)
{
    char out[33];
    const char *body = "ssid=My+Home&pass=p%40ss+word&empty=&flag&loc=%20bed%20";
    CHECK(tp_form_get(body, "ssid", out, sizeof(out)) == TP_OK && strcmp(out, "My Home") == 0, "ssid");
    CHECK(tp_form_get(body, "pass", out, sizeof(out)) == TP_OK && strcmp(out, "p@ss word") == 0, "decoded value");
    CHECK(tp_form_get(body, "empty", out, sizeof(out)) == TP_OK && out[0] == 0, "empty value");
    CHECK(tp_form_get(body, "flag", out, sizeof(out)) == TP_OK && out[0] == 0, "key without '='");
    CHECK(tp_form_get(body, "loc", out, sizeof(out)) == TP_OK && strcmp(out, "bed") == 0, "trimmed");
    CHECK(tp_form_get(body, "missing", out, sizeof(out)) == TP_MISSING, "missing field");
    CHECK(tp_form_get(body, "ss", out, sizeof(out)) == TP_MISSING, "a key prefix is not the key");
    CHECK(tp_form_get("xssid=1&ssid=2", "ssid", out, sizeof(out)) == TP_OK && strcmp(out, "2") == 0,
          "a key ending in the name is not the key");

    // A 600-character SSID: refused whole, never cut to 32
    static char big[700];
    strcpy(big, "ssid=");
    memset(big + 5, 'A', 600);
    big[605] = 0;
    strcat(big, "&x=1");
    CHECK(tp_form_get(big, "ssid", out, sizeof(out)) == TP_TOO_LONG && out[0] == 0, "600-character value: too long");
    CHECK(tp_form_get(big, "x", out, sizeof(out)) == TP_OK && strcmp(out, "1") == 0, "the next field still read");

    // The limit counts the decoded text: 32 characters fit a 33-byte field, 33 do not
    char b32[128] = "k=", b33[128] = "k=";
    for (int i = 0; i < 32; i++) {
        strcat(b32, "%41");
        strcat(b33, "%41");
    }
    strcat(b33, "B");
    CHECK(tp_form_get(b32, "k", out, sizeof(out)) == TP_OK && strlen(out) == 32, "32 decoded characters fit");
    CHECK(tp_form_get(b33, "k", out, sizeof(out)) == TP_TOO_LONG, "33 do not");

    char field[8] = "old";
    CHECK(!tp_copy_fits(field, sizeof(field), "12345678") && strcmp(field, "old") == 0, "copy: too long, unchanged");
    CHECK(tp_copy_fits(field, sizeof(field), "1234567") && strcmp(field, "1234567") == 0, "copy: exactly fits");
}

int main(void)
{
    test_t4_setpoint();
    test_url_decode();
    test_t14_form();
    TEST_DONE();
}
