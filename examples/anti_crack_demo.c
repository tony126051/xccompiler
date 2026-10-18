#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ============================================================================
 * Anti-Reverse-Engineering Showcase for XCC Custom Hardened IR & Polymorphic VM
 * ============================================================================ */

/* Secret key transformation using non-linear rounds */
int verify_serial_code(const char *user_id, uint64_t serial_input) {
    uint64_t acc = 0x5A5A1337ULL;
    int i = 0;
    while (user_id[i] != '\0') {
        uint64_t ch = (uint64_t)(unsigned char)user_id[i];
        if ((i & 1) == 0) {
            acc = (acc ^ (ch * 0x109)) + 0x7F31ULL;
        } else {
            acc = (acc + (ch ^ 0x55)) ^ (acc >> 3);
        }
        i++;
    }
    uint64_t expected = (acc ^ 0xA5A5DEADULL) + 0x2026ULL;
    return (serial_input == expected) ? 1 : 0;
}

uint64_t derive_expected_serial(const char *user_id) {
    uint64_t acc = 0x5A5A1337ULL;
    for (int i = 0; user_id[i] != '\0'; i++) {
        uint64_t ch = (uint64_t)(unsigned char)user_id[i];
        if ((i & 1) == 0) {
            acc = (acc ^ (ch * 0x109)) + 0x7F31ULL;
        } else {
            acc = (acc + (ch ^ 0x55)) ^ (acc >> 3);
        }
    }
    return (acc ^ 0xA5A5DEADULL) + 0x2026ULL;
}

int main(void) {
    const char *secret_user = "XCC-ENTERPRISE-ADMIN";
    const char *vault_banner = "[VAULT] Protected by XCC Custom Hardened IR + Polymorphic VM";

    puts(vault_banner);

    uint64_t valid_serial = derive_expected_serial(secret_user);
    int ok_valid = verify_serial_code(secret_user, valid_serial);
    int ok_tampered = verify_serial_code(secret_user, valid_serial ^ 1ULL);

    printf("User Identity   : %s\n", secret_user);
    printf("Derived Serial  : 0x%llx\n", (unsigned long long)valid_serial);
    printf("Auth Check (OK) : %s\n", ok_valid ? "ACCESS GRANTED" : "ACCESS DENIED");
    printf("Auth Check (Bad): %s\n", ok_tampered ? "ACCESS GRANTED" : "ACCESS DENIED");

    return (ok_valid == 1 && ok_tampered == 0) ? 0 : 1;
}
