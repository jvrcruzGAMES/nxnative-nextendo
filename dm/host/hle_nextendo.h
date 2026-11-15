// Nextendo Network integration for nxnative
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NEXTENDO_CLIENT_ID_DEFAULT "nxc_cUHxqMhkXXSA"
#define NEXTENDO_SERVER_IP_DEFAULT "51.178.29.194"
#define NEXTENDO_NAT_IP_DEFAULT    "164.132.111.120"
#define NEXTENDO_API_URL_DEFAULT   "https://nextendo.network"

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 if Nextendo integration is enabled (user logged in and token available), 0 otherwise.
int nextendo_enabled(void);

// Returns 1 if user credentials exist in Keychain or environment, 0 otherwise.
int nextendo_is_logged_in(void);

// Keychain credentials management
int nextendo_keychain_save(const char *token, const char *username, uint64_t pid);
int nextendo_keychain_load(char *token_out, size_t token_cap, char *user_out, size_t user_cap, uint64_t *pid_out);
int nextendo_keychain_delete(void);

// Initialize Nextendo configuration and load stored credentials.
void nextendo_init(void);

// Player PID (returns 0 or 0xcafe if not logged in).
uint64_t nextendo_get_pid(void);

// Player username (e.g. from Nextendo auth or DM_NICK).
const char *nextendo_get_username(void);

// Nextendo game token (nx2...).
const char *nextendo_get_token(void);

// Client ID configured or default.
const char *nextendo_get_client_id(void);

// Server IP configured or default.
const char *nextendo_get_server_ip(void);

// NAT IP configured or default.
const char *nextendo_get_nat_ip(void);

// Generate signed RS256 BAAS id_token for the active Nextendo account and game.
int nextendo_generate_id_token(const char *installed_version, uint64_t program_id, char *out_buf, size_t out_max);

// Redirects a hostname per Nextendo rules (Atmosphere DNS-MITM semantics).
// Returns 1 if redirected (writing destination IP to out_ip and canonical name to out_canon),
// or 0 if normal DNS resolution should be performed.
int nextendo_redirect_host(const char *host, char *out_ip, size_t out_sz, char *out_canon, size_t canon_sz);

// Reverse map: returns the original hostname that was redirected to the given IP (for SNI restoration).
const char *nextendo_last_host_for_ip(const char *ip);

// Records that an IP was resolved for a hostname.
void nextendo_set_last_host_for_ip(const char *ip, const char *host);

// Port-keyed fallback (for gRPC/NPLN connections that lose target IP).
void nextendo_set_last_ip_for_port(uint16_t port, const char *ip);
const char *nextendo_last_ip_for_port(uint16_t port);

#ifdef __cplusplus
}
#endif
