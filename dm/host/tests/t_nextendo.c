// Test Nextendo online integration: token generation, DNS redirects, real sockets, and TLS handshake.
#include "../hle_nextendo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <Security/SecureTransport.h>

int main(void) {
    setenv("DM_NEXTENDO", "1", 1);
    setenv("NEXTENDO_CLIENT_ID", "nxc_cUHxqMhkXXSA", 1);
    setenv("NEXTENDO_PID", "1234567890", 1);
    setenv("NEXTENDO_USER", "NextendoTester", 1);
    setenv("NEXTENDO_TOKEN", "mock_nex_token_abc123", 1);

    nextendo_init();
    assert(nextendo_enabled() == 1);
    assert(nextendo_get_pid() == 1234567890ull);
    assert(!strcmp(nextendo_get_username(), "NextendoTester"));
    assert(!strcmp(nextendo_get_token(), "mock_nex_token_abc123"));
    assert(!strcmp(nextendo_get_client_id(), "nxc_cUHxqMhkXXSA"));

    // DNS MITM tests
    char ip[64] = {0};
    char canon[256] = {0};

    // Wildcard matching *.nintendo.net -> Server IP
    int r = nextendo_redirect_host("dauth-lp1.ndas.srv.nintendo.net", ip, sizeof(ip), canon, sizeof(canon));
    assert(r == 1);
    assert(!strcmp(ip, "51.178.29.194"));
    assert(!strcmp(canon, "dauth-lp1.ndas.srv.nintendo.net"));

    // % replacement with lp1
    r = nextendo_redirect_host("dauth-%.ndas.srv.nintendo.net", ip, sizeof(ip), canon, sizeof(canon));
    assert(r == 1);
    assert(!strcmp(canon, "dauth-lp1.ndas.srv.nintendo.net"));

    // accounts.nintendo.com rewrite
    r = nextendo_redirect_host("accounts.nintendo.com", ip, sizeof(ip), canon, sizeof(canon));
    assert(r == 1);
    assert(!strcmp(canon, "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com"));

    // NAT redirect
    r = nextendo_redirect_host("g2b309e01-lp1.s.n.srv.nintendo.net", ip, sizeof(ip), canon, sizeof(canon));
    assert(r == 1);
    assert(!strcmp(ip, "164.132.111.120"));

    // Reverse lookup
    const char *last_host = nextendo_last_host_for_ip("164.132.111.120");
    assert(last_host && !strcmp(last_host, "g2b309e01-lp1.s.n.srv.nintendo.net"));

    // JWT token generation
    char jwt[2048] = {0};
    int len = nextendo_generate_id_token("1.2.0", 0x010015100B514000ull, jwt, sizeof(jwt));
    assert(len > 100);
    assert(strchr(jwt, '.') != NULL);
    assert(strstr(jwt, "eyJ") == jwt); // Base64URL JSON header prefix

    // Keychain storage tests
    assert(nextendo_keychain_save("kc_test_token_xyz", "KeychainUser", 9876543210ull) == 1);
    char kc_tok[128] = {0};
    char kc_user[64] = {0};
    uint64_t kc_pid = 0;
    assert(nextendo_keychain_load(kc_tok, sizeof(kc_tok), kc_user, sizeof(kc_user), &kc_pid) == 1);
    assert(!strcmp(kc_tok, "kc_test_token_xyz"));
    assert(!strcmp(kc_user, "KeychainUser"));
    assert(kc_pid == 9876543210ull);
    assert(nextendo_is_logged_in() == 1);

    assert(nextendo_keychain_delete() == 1);
    assert(nextendo_keychain_load(kc_tok, sizeof(kc_tok), kc_user, sizeof(kc_user), &kc_pid) == 0);

    puts("ok t_nextendo");
    return 0;
}
