#include "tlscert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "tls";
#define PEM_MAX 1536
static char *s_cert, *s_key;
static char s_pin[64];

static int rng(void *ctx, unsigned char *out, size_t len)
{
    esp_fill_random(out, len);  // hardware RNG (the radio co-processor is not needed for it on the P4)
    return 0;
}

static bool make(void)
{
    int64_t t0 = esp_timer_get_time();
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    unsigned char serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7F;  // positive
    serial[0] |= 0x01;
    int r = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    r = r ? r : mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), rng, NULL);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    r = r ? r : mbedtls_x509write_crt_set_subject_name(&crt, "CN=thermostat,O=OpenTherm thermostat");
    r = r ? r : mbedtls_x509write_crt_set_issuer_name(&crt, "CN=thermostat,O=OpenTherm thermostat");
    r = r ? r : mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    // Fixed dates: the clock may not be set yet, and the certificate is trusted by its key, not by its dates.
    r = r ? r : mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20491231235959");
    r = r ? r : mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    r = r ? r : mbedtls_x509write_crt_pem(&crt, (unsigned char *)s_cert, PEM_MAX, rng, NULL);
    r = r ? r : mbedtls_pk_write_key_pem(&key, (unsigned char *)s_key, PEM_MAX);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    if (r) {
        ESP_LOGE(TAG, "making the certificate failed: -0x%04x", -r);
        return false;
    }
    ESP_LOGW(TAG, "new HTTPS certificate made in %lld ms", (esp_timer_get_time() - t0) / 1000);
    return true;
}

// Checks that the PEMs parse and belong together, and computes the public-key pin.
static bool check(void)
{
    mbedtls_x509_crt crt;
    mbedtls_pk_context key;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&key);
    bool ok = mbedtls_x509_crt_parse(&crt, (const unsigned char *)s_cert, strlen(s_cert) + 1) == 0 &&
              mbedtls_pk_parse_key(&key, (const unsigned char *)s_key, strlen(s_key) + 1, NULL, 0, rng, NULL) == 0 &&
              mbedtls_pk_check_pair(&crt.pk, &key, rng, NULL) == 0;
    if (ok) {
        unsigned char der[160], hash[32];
        int n = mbedtls_pk_write_pubkey_der(&crt.pk, der, sizeof(der));  // written at the end of the buffer
        size_t b64 = 0;
        ok = n > 0 && mbedtls_sha256(der + sizeof(der) - n, n, hash, 0) == 0;
        strcpy(s_pin, "sha256//");
        ok = ok && mbedtls_base64_encode((unsigned char *)s_pin + 8, sizeof(s_pin) - 8, &b64, hash, sizeof(hash)) == 0;
    }
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&crt);
    if (!ok) {
        s_pin[0] = 0;
    }
    return ok;
}

bool tlscert_init(void)
{
    if (s_pin[0]) {
        return true;
    }
    s_cert = s_cert ? s_cert : calloc(1, PEM_MAX);
    s_key = s_key ? s_key : calloc(1, PEM_MAX);
    if (!s_cert || !s_key) {
        return false;
    }
    nvs_handle_t h;
    bool have = false;
    if (nvs_open("tls", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "no storage for the certificate: a new one is used until the next restart");
        return make() && check();
    }
    size_t lc = PEM_MAX, lk = PEM_MAX;
    have = nvs_get_str(h, "cert", s_cert, &lc) == ESP_OK && nvs_get_str(h, "key", s_key, &lk) == ESP_OK && check();
    if (!have) {
        have = make() && check();
        if (have && (nvs_set_str(h, "cert", s_cert) != ESP_OK || nvs_set_str(h, "key", s_key) != ESP_OK ||
                     nvs_commit(h) != ESP_OK)) {
            ESP_LOGE(TAG, "the new certificate could not be saved: it changes at the next restart");
        }
    }
    nvs_close(h);
    if (have) {
        ESP_LOGI(TAG, "HTTPS public-key pin: %s", s_pin);
    }
    return have;
}

const char *tlscert_cert_pem(void)
{
    return s_cert ? s_cert : "";
}

const char *tlscert_key_pem(void)
{
    return s_key ? s_key : "";
}

const char *tlscert_pin(void)
{
    return s_pin;
}

void tlscert_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("tls", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "cert");
        nvs_erase_key(h, "key");
        nvs_commit(h);
        nvs_close(h);
    }
}
