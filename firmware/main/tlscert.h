#pragma once
// The device's own HTTPS certificate: an EC P-256 key and a self-signed certificate, made on the device at first
// start and kept in NVS ("tls"). Nothing secret is in the firmware image or the repository, and every device has
// its own key. Browsers warn once (self-signed); tools pin the public key (tools/ota.sh).
#include <stdbool.h>
#include <stddef.h>

// Loads the stored key and certificate, or makes new ones. False when neither works (then there is no web server).
bool tlscert_init(void);
const char *tlscert_cert_pem(void);  // NUL-terminated PEM, valid after tlscert_init()
const char *tlscert_key_pem(void);
// The public-key pin as curl takes it: "sha256//<base64>" (curl --pinnedpubkey). "" before tlscert_init().
const char *tlscert_pin(void);
// Throws the stored certificate away; a new one is made at the next start.
void tlscert_forget(void);
