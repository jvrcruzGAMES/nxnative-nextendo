// Nextendo Network integration implementation for nxnative
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <Security/Security.h>
#endif
#import <pthread.h>
#import <sys/socket.h>
#import <netinet/in.h>
#import <arpa/inet.h>

#include "hle_nextendo.h"
#include "dm.h"

static pthread_mutex_t s_nextendo_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_initialized = 0;
static int s_enabled = 1;
static char s_client_id[64] = NEXTENDO_CLIENT_ID_DEFAULT;
static char s_server_ip[64] = NEXTENDO_SERVER_IP_DEFAULT;
static char s_nat_ip[64] = NEXTENDO_NAT_IP_DEFAULT;
static char s_api_url[256] = NEXTENDO_API_URL_DEFAULT;

static uint64_t s_pid = 0;
static char s_username[64] = "Player";
static char s_token[512] = "";
static char s_friend_code[64] = "";

static NSMutableDictionary<NSString *, NSString *> *s_last_host_for_ip = nil;
static NSMutableDictionary<NSNumber *, NSString *> *s_last_ip_for_port = nil;
static SecKeyRef s_baas_rsa_key = NULL;

static NSString *base64url_encode(NSData *data) {
    NSString *base64 = [data base64EncodedStringWithOptions:0];
    base64 = [base64 stringByReplacingOccurrencesOfString:@"+" withString:@"-"];
    base64 = [base64 stringByReplacingOccurrencesOfString:@"/" withString:@"_"];
    base64 = [base64 stringByReplacingOccurrencesOfString:@"=" withString:@""];
    return base64;
}

static BOOL wildcard_match(const char *pattern, const char *str) {
    if (!pattern || !str) return NO;
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return YES;
            while (*str) {
                if (wildcard_match(pattern, str)) return YES;
                str++;
            }
            return NO;
        } else if (*pattern == '?' || tolower((unsigned char)*pattern) == tolower((unsigned char)*str)) {
            pattern++;
            str++;
        } else {
            return NO;
        }
    }
    while (*pattern == '*') pattern++;
    return (!*pattern && !*str);
}

#if !TARGET_OS_IPHONE
static NSString *mac_auth_path(void) {
    const char *env = getenv("DM_NEXTENDO_AUTH");
    if (env && env[0]) return [NSString stringWithUTF8String:env];
    NSArray<NSString *> *dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *base = dirs.count ? [dirs[0] stringByAppendingPathComponent:@"nxnative"] : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/nxnative"];
    return [base stringByAppendingPathComponent:@"nextendo_auth.json"];
}
#endif

static int write_auth_dict(NSDictionary *dict) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:dict options:0 error:nil];
    if (!data) return 0;

#if TARGET_OS_IPHONE
    NSDictionary *query = @{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: @"network.nextendo.auth",
        (__bridge id)kSecAttrAccount: @"session"
    };
    SecItemDelete((__bridge CFDictionaryRef)query);

    NSMutableDictionary *attrs = [query mutableCopy];
    attrs[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlock;
    attrs[(__bridge id)kSecValueData] = data;

    OSStatus st = SecItemAdd((__bridge CFDictionaryRef)attrs, NULL);
    return (st == errSecSuccess) ? 1 : 0;
#else
    NSString *path = mac_auth_path();
    NSString *dir = [path stringByDeletingLastPathComponent];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    if (![data writeToFile:path options:NSDataWritingAtomic error:nil]) return 0;
    [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions: @0600} ofItemAtPath:path error:nil];
    return 1;
#endif
}

static NSDictionary *sync_http_json(NSURLRequest *req) {
    __block NSData *respData = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
        if (!err && data) respData = data;
        dispatch_semaphore_signal(sem);
    }];
    [task resume];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
    if (!respData) return nil;
    id obj = [NSJSONSerialization JSONObjectWithData:respData options:0 error:nil];
    return [obj isKindOfClass:[NSDictionary class]] ? (NSDictionary *)obj : nil;
}

static void nextendo_try_refresh_locked(NSString *refreshToken, NSDictionary *origDict) {
    if (!refreshToken.length) return;
    NSString *base = [NSString stringWithUTF8String:s_api_url[0] ? s_api_url : "https://nextendo.network"];
    if ([base hasSuffix:@"/"]) base = [base substringToIndex:base.length - 1];
    NSString *cid = [NSString stringWithUTF8String:s_client_id];

    NSURL *tokURL = [NSURL URLWithString:[base stringByAppendingString:@"/api/oauth/token"]];
    if (!tokURL) return;
    NSMutableURLRequest *tokReq = [NSMutableURLRequest requestWithURL:tokURL];
    tokReq.HTTPMethod = @"POST";
    [tokReq setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
    [tokReq setValue:cid forHTTPHeaderField:@"X-Nextendo-Client-Id"];
    [tokReq setValue:@"nxnative/1.0" forHTTPHeaderField:@"User-Agent"];

    NSCharacterSet *allowed = [NSCharacterSet URLQueryAllowedCharacterSet];
    NSString *body = [NSString stringWithFormat:@"grant_type=refresh_token&refresh_token=%@&client_id=%@",
                      [refreshToken stringByAddingPercentEncodingWithAllowedCharacters:allowed] ?: refreshToken,
                      [cid stringByAddingPercentEncodingWithAllowedCharacters:allowed] ?: cid];
    tokReq.HTTPBody = [body dataUsingEncoding:NSUTF8StringEncoding];

    NSDictionary *tokJson = sync_http_json(tokReq);
    NSString *accessToken = tokJson[@"access_token"] ?: tokJson[@"token"];
    if (![accessToken isKindOfClass:[NSString class]] || !accessToken.length) {
        LOG("[Nextendo] Refresh token exchange failed");
        return;
    }
    NSString *newRefresh = ([tokJson[@"refresh_token"] isKindOfClass:[NSString class]] && [tokJson[@"refresh_token"] length])
        ? tokJson[@"refresh_token"] : refreshToken;

    NSURL *nexURL = [NSURL URLWithString:[base stringByAppendingString:@"/api/nex-token"]];
    if (!nexURL) return;
    NSMutableURLRequest *nexReq = [NSMutableURLRequest requestWithURL:nexURL];
    [nexReq setValue:[NSString stringWithFormat:@"Bearer %@", accessToken] forHTTPHeaderField:@"Authorization"];
    [nexReq setValue:cid forHTTPHeaderField:@"X-Nextendo-Client-Id"];
    [nexReq setValue:@"nxnative/1.0" forHTTPHeaderField:@"User-Agent"];

    NSDictionary *nexJson = sync_http_json(nexReq);
    NSString *nexTok = nexJson[@"nex_token"];
    if (![nexTok isKindOfClass:[NSString class]] || !nexTok.length) {
        LOG("[Nextendo] /api/nex-token failed after refresh");
        return;
    }

    strncpy(s_token, [nexTok UTF8String], sizeof(s_token) - 1);
    s_token[sizeof(s_token) - 1] = '\0';

    if ([nexJson[@"username"] isKindOfClass:[NSString class]] && [nexJson[@"username"] length]) {
        strncpy(s_username, [nexJson[@"username"] UTF8String], sizeof(s_username) - 1);
        s_username[sizeof(s_username) - 1] = '\0';
    }
    if ([nexJson[@"friend_code"] isKindOfClass:[NSString class]] && [nexJson[@"friend_code"] length]) {
        strncpy(s_friend_code, [nexJson[@"friend_code"] UTF8String], sizeof(s_friend_code) - 1);
        s_friend_code[sizeof(s_friend_code) - 1] = '\0';
    }
    id pidVal = nexJson[@"pid"];
    if ([pidVal isKindOfClass:[NSNumber class]]) s_pid = [pidVal unsignedLongLongValue];
    else if ([pidVal isKindOfClass:[NSString class]]) s_pid = strtoull([pidVal UTF8String], NULL, 0);

    NSMutableDictionary *updated = origDict ? [origDict mutableCopy] : [NSMutableDictionary dictionary];
    updated[@"token"] = nexTok;
    updated[@"username"] = [NSString stringWithUTF8String:s_username];
    updated[@"pid"] = @(s_pid ? s_pid : 0xcafe);
    updated[@"refresh_token"] = newRefresh;
    updated[@"nex_token_at"] = @([[NSDate date] timeIntervalSince1970]);
    if (s_friend_code[0]) updated[@"friend_code"] = [NSString stringWithUTF8String:s_friend_code];
    write_auth_dict(updated);
    LOG("[Nextendo] Refreshed NEX token for user='%s' pid=%llu", s_username, s_pid);
}

int nextendo_keychain_save(const char *token, const char *username, uint64_t pid) {
    if (!token || !*token) return 0;
    @autoreleasepool {
        NSDictionary *dict = @{
            @"token": [NSString stringWithUTF8String:token],
            @"username": [NSString stringWithUTF8String:username ? username : "Player"],
            @"pid": @(pid ? pid : 0xcafe)
        };
        return write_auth_dict(dict);
    }
}

int nextendo_keychain_load(char *token_out, size_t token_cap, char *user_out, size_t user_cap, uint64_t *pid_out) {
    @autoreleasepool {
#if TARGET_OS_IPHONE
        NSDictionary *query = @{
            (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService: @"network.nextendo.auth",
            (__bridge id)kSecAttrAccount: @"session",
            (__bridge id)kSecReturnData: @YES,
            (__bridge id)kSecMatchLimit: (__bridge id)kSecMatchLimitOne
        };
        CFTypeRef result = NULL;
        OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
        if (status != errSecSuccess || !result) {
            return 0;
        }
        NSData *data = (__bridge NSData *)result;
        NSError *error = nil;
        NSDictionary *dict = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
        CFRelease(result);
#else
        NSString *path = mac_auth_path();
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (!data) return 0;
        NSError *error = nil;
        NSDictionary *dict = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
#endif
        if (error || ![dict isKindOfClass:[NSDictionary class]]) {
            return 0;
        }
        NSString *token = dict[@"token"];
        if (!token || ![token isKindOfClass:[NSString class]] || token.length == 0) {
            return 0;
        }
        NSString *user = dict[@"username"];
        id pidObj = dict[@"pid"];
        uint64_t pid = 0;
        if ([pidObj isKindOfClass:[NSNumber class]]) {
            pid = [pidObj unsignedLongLongValue];
        } else if ([pidObj isKindOfClass:[NSString class]]) {
            pid = strtoull([pidObj UTF8String], NULL, 0);
        }
        if (pid == 0) pid = 0xcafe;

        if (token_out && token_cap > 0) {
            snprintf(token_out, token_cap, "%s", [token UTF8String]);
        }
        if (user_out && user_cap > 0) {
            snprintf(user_out, user_cap, "%s", user ? [user UTF8String] : "Player");
        }
        if (pid_out) {
            *pid_out = pid;
        }
        return 1;
    }
}

int nextendo_keychain_delete(void) {
    @autoreleasepool {
#if TARGET_OS_IPHONE
        NSDictionary *query = @{
            (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService: @"network.nextendo.auth",
            (__bridge id)kSecAttrAccount: @"session"
        };
        OSStatus st = SecItemDelete((__bridge CFDictionaryRef)query);
        return (st == errSecSuccess || st == errSecItemNotFound) ? 1 : 0;
#else
        NSString *path = mac_auth_path();
        if (![[NSFileManager defaultManager] fileExistsAtPath:path]) return 1;
        return [[NSFileManager defaultManager] removeItemAtPath:path error:nil] ? 1 : 0;
#endif
    }
}

int nextendo_is_logged_in(void) {
    char token[512] = {0};
    if (nextendo_keychain_load(token, sizeof(token), NULL, 0, NULL) && token[0] != '\0') {
        return 1;
    }
    const char *env_tok = getenv("NEXTENDO_TOKEN");
    if (env_tok && env_tok[0]) return 1;
    return 0;
}

static void ensure_initialized_locked(void) {
    if (s_initialized) return;
    s_initialized = 1;
    s_last_host_for_ip = [NSMutableDictionary dictionary];
    s_last_ip_for_port = [NSMutableDictionary dictionary];

    const char *env_cid = getenv("NEXTENDO_CLIENT_ID");
    if (env_cid && env_cid[0]) strncpy(s_client_id, env_cid, sizeof(s_client_id) - 1);

    const char *env_srv = getenv("NEXTENDO_SERVER_IP");
    if (env_srv && env_srv[0]) strncpy(s_server_ip, env_srv, sizeof(s_server_ip) - 1);

    const char *env_nat = getenv("NEXTENDO_NAT_IP");
    if (env_nat && env_nat[0]) strncpy(s_nat_ip, env_nat, sizeof(s_nat_ip) - 1);

    const char *env_api = getenv("NEXTENDO_API");
    if (env_api && env_api[0]) strncpy(s_api_url, env_api, sizeof(s_api_url) - 1);

    // 1. Try loading credentials from Keychain
    char kc_tok[sizeof(s_token)] = {0};
    char kc_user[sizeof(s_username)] = {0};
    uint64_t kc_pid = 0;
    if (nextendo_keychain_load(kc_tok, sizeof(kc_tok), kc_user, sizeof(kc_user), &kc_pid) && kc_tok[0]) {
        strncpy(s_token, kc_tok, sizeof(s_token) - 1);
        strncpy(s_username, kc_user, sizeof(s_username) - 1);
        s_pid = kc_pid;
        LOG("[Nextendo] Loaded credentials from Keychain (user='%s', pid=%llu)", s_username, s_pid);
    }

    // 2. Look for nextendo_auth.json or nextendo.json in known paths
    NSMutableArray<NSString *> *searchPaths = [NSMutableArray array];

    const char *dm_save = getenv("DM_SAVE");
    if (dm_save && dm_save[0]) {
        NSString *saveDir = [NSString stringWithUTF8String:dm_save];
        [searchPaths addObject:[[saveDir stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"nextendo_auth.json"]];
        [searchPaths addObject:[saveDir stringByAppendingPathComponent:@"nextendo_auth.json"]];
        [searchPaths addObject:[saveDir stringByAppendingPathComponent:@"nextendo.json"]];
    }

    NSString *home = NSHomeDirectory();
    if (home) {
        [searchPaths addObject:[NSString stringWithFormat:@"%@/Library/Application Support/nxnative/nextendo_auth.json", home]];
        [searchPaths addObject:[NSString stringWithFormat:@"%@/Library/Application Support/nxnative/nextendo.json", home]];
        [searchPaths addObject:[NSString stringWithFormat:@"%@/Documents/nextendo_auth.json", home]];
        [searchPaths addObject:[NSString stringWithFormat:@"%@/Documents/nextendo.json", home]];
    }

    NSString *resPath = [[NSBundle mainBundle] resourcePath];
    if (resPath) {
        [searchPaths addObject:[resPath stringByAppendingPathComponent:@"dm/nextendo.json"]];
        [searchPaths addObject:[resPath stringByAppendingPathComponent:@"nextendo.json"]];
        [searchPaths addObject:[resPath stringByAppendingPathComponent:@"dm/nextendo_auth.json"]];
        [searchPaths addObject:[resPath stringByAppendingPathComponent:@"nextendo_auth.json"]];
    }
    [searchPaths addObject:@"nextendo.json"];
    [searchPaths addObject:@"nextendo_auth.json"];

    for (NSString *path in searchPaths) {
        if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
            NSData *data = [NSData dataWithContentsOfFile:path];
            if (!data) continue;
            NSError *err = nil;
            NSDictionary *dict = [NSJSONSerialization JSONObjectWithData:data options:0 error:&err];
            if ([dict isKindOfClass:[NSDictionary class]]) {
                if (s_pid == 0) {
                    id pidVal = dict[@"pid"];
                    if ([pidVal isKindOfClass:[NSNumber class]]) {
                        s_pid = [pidVal unsignedLongLongValue];
                    } else if ([pidVal isKindOfClass:[NSString class]]) {
                        s_pid = strtoull([pidVal UTF8String], NULL, 0);
                    }
                }

                if (!s_token[0]) {
                    id tokVal = dict[@"nex_token"];
                    if (!tokVal) tokVal = dict[@"token"];
                    if ([tokVal isKindOfClass:[NSString class]]) {
                        strncpy(s_token, [tokVal UTF8String], sizeof(s_token) - 1);
                    }
                }

                if (strcmp(s_username, "Player") == 0) {
                    id userVal = dict[@"username"];
                    if ([userVal isKindOfClass:[NSString class]]) {
                        strncpy(s_username, [userVal UTF8String], sizeof(s_username) - 1);
                    }
                }

                id fcVal = dict[@"friend_code"];
                if ([fcVal isKindOfClass:[NSString class]]) {
                    strncpy(s_friend_code, [fcVal UTF8String], sizeof(s_friend_code) - 1);
                }

                id cidVal = dict[@"client_id"];
                if ([cidVal isKindOfClass:[NSString class]]) {
                    strncpy(s_client_id, [cidVal UTF8String], sizeof(s_client_id) - 1);
                }

                id srvVal = dict[@"server_ip"];
                if ([srvVal isKindOfClass:[NSString class]]) {
                    strncpy(s_server_ip, [srvVal UTF8String], sizeof(s_server_ip) - 1);
                }

                id natVal = dict[@"nat_ip"];
                if ([natVal isKindOfClass:[NSString class]]) {
                    strncpy(s_nat_ip, [natVal UTF8String], sizeof(s_nat_ip) - 1);
                }

                id refreshVal = dict[@"refresh_token"];
                NSTimeInterval tokAt = [dict[@"nex_token_at"] isKindOfClass:[NSNumber class]] ? [dict[@"nex_token_at"] doubleValue] : 0;
                NSTimeInterval now = [[NSDate date] timeIntervalSince1970];
                BOOL tokenStale = (strncmp(s_token, "nx2", 3) != 0) || (tokAt > 0 && (now - tokAt) > 20 * 3600);
                if ([refreshVal isKindOfClass:[NSString class]] && [refreshVal length] && tokenStale) {
                    nextendo_try_refresh_locked(refreshVal, dict);
                }

                LOG("[Nextendo] Loaded config from %s (pid=%llu, user='%s')", [path UTF8String], s_pid, s_username);
                break;
            }
        }
    }

    // 3. Environment overrides
    const char *env_tok = getenv("NEXTENDO_TOKEN");
    if (env_tok && env_tok[0]) { strncpy(s_token, env_tok, sizeof(s_token) - 1); }

    const char *env_pid = getenv("NEXTENDO_PID");
    if (env_pid && env_pid[0]) { s_pid = strtoull(env_pid, NULL, 0); }

    const char *env_user = getenv("NEXTENDO_USER");
    if (env_user && env_user[0]) strncpy(s_username, env_user, sizeof(s_username) - 1);

    // Nextendo features are active if and only if the user is logged in (token available).
    const char *dm_nextendo = getenv("DM_NEXTENDO");
    if (dm_nextendo && (strcmp(dm_nextendo, "0") == 0 || strcasecmp(dm_nextendo, "off") == 0 || strcasecmp(dm_nextendo, "no") == 0)) {
        s_enabled = 0;
    } else {
        s_enabled = (s_token[0] != '\0') ? 1 : 0;
    }

    if (!s_enabled) return;

    LOG("[Nextendo] Online integration active (client=%s, server=%s, nat=%s)", s_client_id, s_server_ip, s_nat_ip);
}

int nextendo_enabled(void) {
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    int ret = s_enabled;
    pthread_mutex_unlock(&s_nextendo_lock);
    return ret;
}

void nextendo_init(void) {
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    pthread_mutex_unlock(&s_nextendo_lock);
}

uint64_t nextendo_get_pid(void) {
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    uint64_t pid = s_pid ? s_pid : 0xcafe;
    pthread_mutex_unlock(&s_nextendo_lock);
    return pid;
}

const char *nextendo_get_username(void) {
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    const char *u = s_username[0] ? s_username : "Player";
    pthread_mutex_unlock(&s_nextendo_lock);
    return u;
}

const char *nextendo_get_token(void) {
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    const char *t = s_token[0] ? s_token : NULL;
    pthread_mutex_unlock(&s_nextendo_lock);
    return t;
}

const char *nextendo_get_client_id(void) {
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    const char *cid = s_client_id;
    pthread_mutex_unlock(&s_nextendo_lock);
    return cid;
}

const char *nextendo_get_server_ip(void) {
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    const char *srv = s_server_ip;
    pthread_mutex_unlock(&s_nextendo_lock);
    return srv;
}

const char *nextendo_get_nat_ip(void) {
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    const char *nat = s_nat_ip;
    pthread_mutex_unlock(&s_nextendo_lock);
    return nat;
}

static BOOL is_nso_classics(uint64_t program_id) {
    return program_id == 0x0100D870045B6000ULL || // NES
           program_id == 0x01008D300C50C000ULL || // SNES
           program_id == 0x0100C62011050000ULL || // Game Boy
           program_id == 0x010012F017576000ULL || // Game Boy Advance
           program_id == 0x0100C9A00ECE6000ULL || // Nintendo 64
           program_id == 0x0100B3C014BDA000ULL;   // Mega Drive / Genesis
}

int nextendo_generate_id_token(const char *installed_version, uint64_t program_id, char *out_buf, size_t out_max) {
    if (!out_buf || out_max < 64) return -1;
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();

    // Ensure we have an RSA signing key
    if (!s_baas_rsa_key) {
        NSDictionary *attrs = @{
            (__bridge id)kSecAttrKeyType: (__bridge id)kSecAttrKeyTypeRSA,
            (__bridge id)kSecAttrKeySizeInBits: @2048
        };
        CFErrorRef err = NULL;
        s_baas_rsa_key = SecKeyCreateRandomKey((__bridge CFDictionaryRef)attrs, &err);
        if (!s_baas_rsa_key) {
            LOG("[Nextendo] Failed to create RSA signing key for BAAS token: %s",
                err ? [[(__bridge NSError *)err description] UTF8String] : "unknown");
            pthread_mutex_unlock(&s_nextendo_lock);
            return -1;
        }
    }

    uint8_t rawUserId[16], deviceId[16], deviceAccountId[16];
    arc4random_buf(rawUserId, sizeof(rawUserId));
    arc4random_buf(deviceId, sizeof(deviceId));
    arc4random_buf(deviceAccountId, sizeof(deviceAccountId));

    char sub_hex[33], di_hex[33], did_hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(sub_hex + i * 2, 3, "%02x", rawUserId[i]);
        snprintf(di_hex + i * 2, 3, "%02x", deviceId[i]);
        snprintf(did_hex + i * 2, 3, "%02x", deviceAccountId[i]);
    }

    NSString *jti = [[NSUUID UUID] UUIDString];
    long now = (long)[[NSDate date] timeIntervalSince1970];
    long exp = now + 3 * 3600;

    NSMutableDictionary *header = [NSMutableDictionary dictionaryWithDictionary:@{
        @"alg": @"RS256",
        @"typ": @"id_token",
        @"kid": @"nextendo-baas-key-1"
    }];

    NSMutableDictionary *claims = [NSMutableDictionary dictionaryWithDictionary:@{
        @"jku": @"https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com/1.0.0/certificates",
        @"jti": jti,
        @"di": [NSString stringWithUTF8String:di_hex],
        @"sn": @"XAW10000000000",
        @"bs:did": [NSString stringWithUTF8String:did_hex],
        @"hm": @YES,
        @"iss": @"https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com",
        @"aud": @"ed9e2f05d286f7b8",
        @"sub": [NSString stringWithUTF8String:sub_hex],
        @"iat": @(now),
        @"exp": @(exp)
    }];

    if (installed_version && installed_version[0]) {
        claims[@"tv"] = [NSString stringWithUTF8String:installed_version];
    }
    if (s_token[0]) {
        claims[@"nnex"] = [NSString stringWithUTF8String:s_token];
    }

    // App ID overrides for Classics / Scarlet / MH Rise
    if (is_nso_classics(program_id) || program_id == 0x0100A3D008C5C000ULL || program_id == 0x0100559011740000ULL) {
        claims[@"app_id"] = [NSString stringWithFormat:@"%016llX", (unsigned long long)program_id];
    }

    NSData *headerData = [NSJSONSerialization dataWithJSONObject:header options:0 error:nil];
    NSData *claimsData = [NSJSONSerialization dataWithJSONObject:claims options:0 error:nil];

    NSString *headerB64 = base64url_encode(headerData);
    NSString *claimsB64 = base64url_encode(claimsData);
    NSString *signingInput = [NSString stringWithFormat:@"%@.%@", headerB64, claimsB64];
    NSData *signingData = [signingInput dataUsingEncoding:NSUTF8StringEncoding];

    CFErrorRef signErr = NULL;
    CFDataRef sigData = SecKeyCreateSignature(s_baas_rsa_key,
                                              kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
                                              (__bridge CFDataRef)signingData,
                                              &signErr);
    if (!sigData) {
        LOG("[Nextendo] Failed to sign BAAS JWT: %s",
            signErr ? [[(__bridge NSError *)signErr description] UTF8String] : "unknown");
        pthread_mutex_unlock(&s_nextendo_lock);
        return -1;
    }

    NSString *sigB64 = base64url_encode((__bridge NSData *)sigData);
    CFRelease(sigData);

    NSString *jwt = [NSString stringWithFormat:@"%@.%@", signingInput, sigB64];
    const char *jwt_cstr = [jwt UTF8String];
    size_t jwt_len = strlen(jwt_cstr);

    if (jwt_len >= out_max) {
        pthread_mutex_unlock(&s_nextendo_lock);
        return -1;
    }

    memcpy(out_buf, jwt_cstr, jwt_len + 1);
    pthread_mutex_unlock(&s_nextendo_lock);
    return (int)jwt_len;
}

int nextendo_redirect_host(const char *host, char *out_ip, size_t out_sz, char *out_canon, size_t canon_sz) {
    if (!host || !out_ip || out_sz < 16) return 0;
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();

    if (!s_enabled) {
        pthread_mutex_unlock(&s_nextendo_lock);
        return 0;
    }

    const char *h = host;
    if (strncmp(h, "http://", 7) == 0) h += 7;
    else if (strncmp(h, "https://", 8) == 0) h += 8;
    else if (strncmp(h, "ws://", 5) == 0) h += 5;
    else if (strncmp(h, "wss://", 6) == 0) h += 6;

    char eff_host[256];
    strncpy(eff_host, h, sizeof(eff_host) - 1);
    eff_host[sizeof(eff_host) - 1] = '\0';
    char *slash = strchr(eff_host, '/');
    if (slash) *slash = '\0';
    char *colon = strchr(eff_host, ':');
    if (colon) *colon = '\0';

    // Replace % with lp1
    char *pct = strchr(eff_host, '%');
    if (pct) {
        char tmp[256];
        size_t prefix_len = (size_t)(pct - eff_host);
        snprintf(tmp, sizeof(tmp), "%.*slp1%s", (int)prefix_len, eff_host, pct + 1);
        strncpy(eff_host, tmp, sizeof(eff_host) - 1);
        eff_host[sizeof(eff_host) - 1] = '\0';
    }

    // Pre-DNS rewrite: accounts.nintendo.com -> BAAS host
    if (strcasecmp(eff_host, "accounts.nintendo.com") == 0 ||
        strcasecmp(eff_host, "api.accounts.nintendo.com") == 0) {
        strncpy(eff_host, "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com", sizeof(eff_host) - 1);
    }

    const char *target_ip = NULL;

    // NAT test responders (nncs1 -> server_ip, nncs2 -> nat_ip; two distinct IPs required for NAT type detection)
    // and NEX/Pia endpoints hosted alongside the NAT responder
    if (wildcard_match("nncs2-*.n.n.srv.nintendo.net", eff_host) ||
        strcasecmp(eff_host, "g2b309e01-lp1.s.n.srv.nintendo.net") == 0 ||
        strcasecmp(eff_host, "g2df33d01-lp1.p.srv.nintendo.net") == 0) {
        target_ip = s_nat_ip;
    }
    // Nintendo server domain wildcards
    else if (wildcard_match("*.nintendo.net", eff_host) ||
             wildcard_match("*.nintendo.com", eff_host) ||
             wildcard_match("*.nintendowifi.net", eff_host) ||
             wildcard_match("*.nintendo.co.jp", eff_host) ||
             wildcard_match("*.srv.nintendo.net", eff_host) ||
             wildcard_match("*srv.nintendo.net", eff_host) ||
             wildcard_match("*.npln.nintendo.net", eff_host) ||
             strcasecmp(eff_host, "gamesync.npln.nintendo.net") == 0) {
        target_ip = s_server_ip;
    }
    // Third-party titles handled by Nextendo
    else if (strcasecmp(eff_host, "demonware.net") == 0 ||
             wildcard_match("*.demonware.net", eff_host) ||
             strcasecmp(eff_host, "spring18.gosredirector.ea.com") == 0 ||
             wildcard_match("pvzgw3prodapp-*.ea.com", eff_host) ||
             strcasecmp(eff_host, "public-ubiservices.ubi.com") == 0 ||
             strcasecmp(eff_host, "public-ws-ubiservices.ubi.com") == 0 ||
             strcasecmp(eff_host, "jd.ubisoft.com") == 0 ||
             strcasecmp(eff_host, "phonescoring.jd.ubisoft.com") == 0 ||
             strcasecmp(eff_host, "v2.phonescoring.jd.ubisoft.com") == 0 ||
             strcasecmp(eff_host, "api.braincloudservers.com") == 0 ||
             wildcard_match("*.ultimatechickenhorseserver.com", eff_host) ||
             strcasecmp(eff_host, "nswitch.f13.illfonic.com") == 0) {
        target_ip = s_server_ip;
    }

    if (target_ip) {
        strncpy(out_ip, target_ip, out_sz - 1);
        out_ip[out_sz - 1] = '\0';
        if (out_canon && canon_sz > 0) {
            strncpy(out_canon, eff_host, canon_sz - 1);
            out_canon[canon_sz - 1] = '\0';
        }
        NSString *ipKey = [NSString stringWithUTF8String:target_ip];
        NSString *hostVal = [NSString stringWithUTF8String:eff_host];
        s_last_host_for_ip[ipKey] = hostVal;
        pthread_mutex_unlock(&s_nextendo_lock);
        return 1;
    }

    pthread_mutex_unlock(&s_nextendo_lock);
    return 0;
}

const char *nextendo_last_host_for_ip(const char *ip) {
    if (!ip) return NULL;
    pthread_mutex_lock(&s_nextendo_lock);
    NSString *ipKey = [NSString stringWithUTF8String:ip];
    NSString *host = s_last_host_for_ip[ipKey];
    pthread_mutex_unlock(&s_nextendo_lock);
    return host ? [host UTF8String] : NULL;
}

void nextendo_set_last_host_for_ip(const char *ip, const char *host) {
    if (!ip || !host) return;
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    s_last_host_for_ip[[NSString stringWithUTF8String:ip]] = [NSString stringWithUTF8String:host];
    pthread_mutex_unlock(&s_nextendo_lock);
}

void nextendo_set_last_ip_for_port(uint16_t port, const char *ip) {
    if (!ip || !port) return;
    pthread_mutex_lock(&s_nextendo_lock);
    ensure_initialized_locked();
    s_last_ip_for_port[@(port)] = [NSString stringWithUTF8String:ip];
    pthread_mutex_unlock(&s_nextendo_lock);
}

const char *nextendo_last_ip_for_port(uint16_t port) {
    if (!port) return NULL;
    pthread_mutex_lock(&s_nextendo_lock);
    NSString *ip = s_last_ip_for_port[@(port)];
    pthread_mutex_unlock(&s_nextendo_lock);
    return ip ? [ip UTF8String] : NULL;
}
