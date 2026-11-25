// Mii service layer (nn::mii::Database, the Mii editor applet). On hardware Database talks to the mii:u service (an IDatabaseService the SDK keeps at Database +0) and AppendMii /
// ShowMiiEdit start the MiiEdit library applet; the host has neither. It answers with the six default Miis, the player's console database when they supply it (mii_db_load: the
// mii service's system save, read-only), and Miis made in its stand-in for the editor (AppendMii: the default Mii, kept in a file of the host's own).
// Mario Kart 8 Deluxe (SDK 4.4.0) initializes a Database at boot (mii:e and mii:u were the first services it opened itself) and builds 12 random Miis for its title screen's demo race; their
// heads are drawn by the SDK's Mii rendering code linked into main, from the MiiModel system data (the host's stand-in below, mounted by hle_fs.c MountSystemData).
// The CharInfo the host builds (0x58 bytes) is the system's default Mii as the mii service's StoreData::BuildDefault makes it: the 3DS-era default parameters, with the colours converted to
// the Switch's common colour indices (hair 1, eyes 8, mouth 19, glasses 8), a create ID of UUID version 4 (fixed for each default Mii, fresh for a random one) and the nickname "no name";
// every field is within CharInfoRaw::Verify's ranges (SDK 4.4.0 sdk+0x1b68c8), so CharInfoAccessor::IsValid accepts it.
#include "hle.h"
#include "hle_mii.h"
#include "front.h"
#include <sys/stat.h>
#include <unistd.h>

uint64_t dm_entropy64(void);

__attribute__((weak)) int front_mii_editor(void *charinfo_out, int is_new) {
    (void)charinfo_out; (void)is_new; return -1;
}
__attribute__((weak)) int front_mii_editor_last_index(void) { return -1; }
__attribute__((weak)) uint32_t sf_session(const char *name, sf_answer answer, void *ctx, void (*close)(void *ctx)) {
    (void)name; (void)answer; (void)ctx; (void)close; return 0;
}

// index ≥ 0: default Mii <index> (a fixed create ID, so a game that saved one finds the same Mii again); < 0: a random Mii (a fresh one)
static void mii_default(mii_charinfo *c, int gender, int index) {
    memset(c, 0, sizeof *c);
    uint64_t a = index >= 0 ? 0x4d49494e44454641ull + (uint64_t)index : dm_entropy64(), b = index >= 0 ? 0x554c54444d484f53ull : dm_entropy64();
    memcpy(c->create_id, &a, 8); memcpy(c->create_id + 8, &b, 8);
    c->create_id[6] = (uint8_t)((c->create_id[6] & 0x0f) | 0x40); c->create_id[8] = (uint8_t)((c->create_id[8] & 0x3f) | 0x80);

    const mii_guest_preset *p = NULL;
    if (index >= 0 && index < MII_DEFAULTS) {
        p = &s_guest_miis[index];
    } else {
        int pick = (gender == 1) ? (3 + (int)(dm_entropy64() % 3)) : ((gender == 0) ? (int)(dm_entropy64() % 3) : (int)(dm_entropy64() % MII_DEFAULTS));
        p = &s_guest_miis[pick];
    }

    c->gender = p->gender;
    c->favorite_color = p->favorite_color;
    c->faceline_color = p->faceline_color;
    c->hair_type = (index < 0) ? (uint8_t)(dm_entropy64() % 132) : p->hair_type;
    c->hair_color = p->hair_color;
    c->eye_type = p->eye_type;
    c->eye_color = p->eye_color;
    c->eyebrow_type = p->eyebrow_type;
    c->eyebrow_color = p->eyebrow_color;
    c->nose_type = p->nose_type;
    c->mouth_type = p->mouth_type;

    const char *name = (index >= 0) ? p->name : "no name";
    for (size_t i = 0; i < 10 && name[i]; i++) c->nickname[i] = (uint16_t)name[i];

    c->height = 64; c->build = 64;
    c->eye_scale = 4; c->eye_aspect = 3; c->eye_rotate = 4; c->eye_x = 2; c->eye_y = 12;
    c->eyebrow_scale = 4; c->eyebrow_aspect = 3; c->eyebrow_rotate = 6; c->eyebrow_x = 2; c->eyebrow_y = 10;
    c->nose_scale = 4; c->nose_y = 9;
    c->mouth_color = 19; c->mouth_scale = 4; c->mouth_aspect = 3; c->mouth_y = 13;
    c->beard_color = 8; c->mustache_scale = 4; c->mustache_y = 10;
    c->glass_color = 8; c->glass_scale = 4; c->glass_y = 10;
    c->mole_scale = 4; c->mole_x = 2; c->mole_y = 20;
}

// The console's Mii database, from the player's own console: <DM_SYSTEM_DIR>/save/8000000000000030/MiiDatabase.dat, the mii service's system save as the Switch and the Switch 2 keep it
// (Switch 2 program 0400000000000039 mounts that save as "mii" and reads mii:/MiiDatabase.dat). The file is 0x1a98 bytes: "NFDB", 100 StoreData of 0x44 bytes, the version (1) at +0x1a94,
// the count at +0x1a95 and a CRC-16/CCITT (polynomial 0x1021, initial 0) of the bytes before it, big-endian, at +0x1a96. A StoreData is the CoreData (seven 32-bit words of bit fields and a
// 10-character nickname), the create ID and two CRCs; mii_from_store unpacks it into the CharInfo the SDK's Get returns, as the mii service does (its stored eyebrow y is 3 below the
// CharInfo's). Read once, never written: Miis a game adds go to the host's own file (mii_host_save)
enum { MII_DB_SIZE = 0x1a98, MII_DB_MAX = 100, MII_STORE = 0x44 };
static uint16_t mii_crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= (uint16_t)(p[i] << 8); for (int b = 0; b < 8; b++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1); }
    return c;
}
static unsigned mii_bits(const uint8_t *w, int word, int at, int len) { uint32_t x; memcpy(&x, w + 4 * word, 4); return (x >> at) & ((1u << len) - 1); }
static void mii_put_bits(uint8_t *w, int word, int at, int len, unsigned val) {
    uint32_t x; memcpy(&x, w + 4 * word, 4);
    uint32_t mask = ((1u << len) - 1u) << at;
    x = (x & ~mask) | ((val & ((1u << len) - 1u)) << at);
    memcpy(w + 4 * word, &x, 4);
}
static void mii_from_store(mii_charinfo *c, const uint8_t *sd) {
    memset(c, 0, sizeof *c);
    memcpy(c->create_id, sd + 0x30, 16); memcpy(c->nickname, sd + 0x1c, 20);
    c->hair_type = mii_bits(sd, 0, 0, 8); c->height = mii_bits(sd, 0, 8, 7); c->mole_type = mii_bits(sd, 0, 15, 1); c->build = mii_bits(sd, 0, 16, 7);
    c->hair_flip = mii_bits(sd, 0, 23, 1); c->hair_color = mii_bits(sd, 0, 24, 7); c->type = mii_bits(sd, 0, 31, 1);
    c->eye_color = mii_bits(sd, 1, 0, 7); c->gender = mii_bits(sd, 1, 7, 1); c->eyebrow_color = mii_bits(sd, 1, 8, 7); c->mouth_color = mii_bits(sd, 1, 16, 7); c->beard_color = mii_bits(sd, 1, 24, 7);
    c->glass_color = mii_bits(sd, 2, 0, 7); c->eye_type = mii_bits(sd, 2, 8, 6); c->region_move = mii_bits(sd, 2, 14, 2); c->mouth_type = mii_bits(sd, 2, 16, 6);
    c->font_region = mii_bits(sd, 2, 22, 2); c->eye_y = mii_bits(sd, 2, 24, 5); c->glass_scale = mii_bits(sd, 2, 29, 3);
    c->eyebrow_type = mii_bits(sd, 3, 0, 5); c->mustache_type = mii_bits(sd, 3, 5, 3); c->nose_type = mii_bits(sd, 3, 8, 5); c->beard_type = mii_bits(sd, 3, 13, 3);
    c->nose_y = mii_bits(sd, 3, 16, 5); c->mouth_aspect = mii_bits(sd, 3, 21, 3); c->mouth_y = mii_bits(sd, 3, 24, 5); c->eyebrow_aspect = mii_bits(sd, 3, 29, 3);
    c->mustache_y = mii_bits(sd, 4, 0, 5); c->eye_rotate = mii_bits(sd, 4, 5, 3); c->glass_y = mii_bits(sd, 4, 8, 5); c->eye_aspect = mii_bits(sd, 4, 13, 3);
    c->mole_x = mii_bits(sd, 4, 16, 5); c->eye_scale = mii_bits(sd, 4, 21, 3); c->mole_y = mii_bits(sd, 4, 24, 5);
    c->glass_type = mii_bits(sd, 5, 0, 5); c->favorite_color = mii_bits(sd, 5, 8, 4); c->faceline_type = mii_bits(sd, 5, 12, 4); c->faceline_color = mii_bits(sd, 5, 16, 4);
    c->faceline_wrinkle = mii_bits(sd, 5, 20, 4); c->faceline_make = mii_bits(sd, 5, 24, 4); c->eye_x = mii_bits(sd, 5, 28, 4);
    c->eyebrow_scale = mii_bits(sd, 6, 0, 4); c->eyebrow_rotate = mii_bits(sd, 6, 4, 4); c->eyebrow_x = mii_bits(sd, 6, 8, 4); c->eyebrow_y = (uint8_t)(mii_bits(sd, 6, 12, 4) + 3);
    c->nose_scale = mii_bits(sd, 6, 16, 4); c->mouth_scale = mii_bits(sd, 6, 20, 4); c->mustache_scale = mii_bits(sd, 6, 24, 4); c->mole_scale = mii_bits(sd, 6, 28, 4);
}
static void mii_to_store(uint8_t *sd, const mii_charinfo *c) {
    memset(sd, 0, MII_STORE);
    memcpy(sd + 0x1c, c->nickname, 20);
    memcpy(sd + 0x30, c->create_id, 16);
    mii_put_bits(sd, 0, 0, 8, c->hair_type); mii_put_bits(sd, 0, 8, 7, c->height); mii_put_bits(sd, 0, 15, 1, c->mole_type); mii_put_bits(sd, 0, 16, 7, c->build);
    mii_put_bits(sd, 0, 23, 1, c->hair_flip); mii_put_bits(sd, 0, 24, 7, c->hair_color); mii_put_bits(sd, 0, 31, 1, c->type);
    mii_put_bits(sd, 1, 0, 7, c->eye_color); mii_put_bits(sd, 1, 7, 1, c->gender); mii_put_bits(sd, 1, 8, 7, c->eyebrow_color); mii_put_bits(sd, 1, 16, 7, c->mouth_color); mii_put_bits(sd, 1, 24, 7, c->beard_color);
    mii_put_bits(sd, 2, 0, 7, c->glass_color); mii_put_bits(sd, 2, 8, 6, c->eye_type); mii_put_bits(sd, 2, 14, 2, c->region_move); mii_put_bits(sd, 2, 16, 6, c->mouth_type);
    mii_put_bits(sd, 2, 22, 2, c->font_region); mii_put_bits(sd, 2, 24, 5, c->eye_y); mii_put_bits(sd, 2, 29, 3, c->glass_scale);
    mii_put_bits(sd, 3, 0, 5, c->eyebrow_type); mii_put_bits(sd, 3, 5, 3, c->mustache_type); mii_put_bits(sd, 3, 8, 5, c->nose_type); mii_put_bits(sd, 3, 13, 3, c->beard_type);
    mii_put_bits(sd, 3, 16, 5, c->nose_y); mii_put_bits(sd, 3, 21, 3, c->mouth_aspect); mii_put_bits(sd, 3, 24, 5, c->mouth_y); mii_put_bits(sd, 3, 29, 3, c->eyebrow_aspect);
    mii_put_bits(sd, 4, 0, 5, c->mustache_y); mii_put_bits(sd, 4, 5, 3, c->eye_rotate); mii_put_bits(sd, 4, 8, 5, c->glass_y); mii_put_bits(sd, 4, 13, 3, c->eye_aspect);
    mii_put_bits(sd, 4, 16, 5, c->mole_x); mii_put_bits(sd, 4, 21, 3, c->eye_scale); mii_put_bits(sd, 4, 24, 5, c->mole_y);
    mii_put_bits(sd, 5, 0, 5, c->glass_type); mii_put_bits(sd, 5, 8, 4, c->favorite_color); mii_put_bits(sd, 5, 12, 4, c->faceline_type); mii_put_bits(sd, 5, 16, 4, c->faceline_color);
    mii_put_bits(sd, 5, 20, 4, c->faceline_wrinkle); mii_put_bits(sd, 5, 24, 4, c->faceline_make); mii_put_bits(sd, 5, 28, 4, c->eye_x);
    unsigned eb_y = c->eyebrow_y >= 3 ? (unsigned)(c->eyebrow_y - 3) : 0;
    mii_put_bits(sd, 6, 0, 4, c->eyebrow_scale); mii_put_bits(sd, 6, 4, 4, c->eyebrow_rotate); mii_put_bits(sd, 6, 8, 4, c->eyebrow_x); mii_put_bits(sd, 6, 12, 4, eb_y);
    mii_put_bits(sd, 6, 16, 4, c->nose_scale); mii_put_bits(sd, 6, 20, 4, c->mouth_scale); mii_put_bits(sd, 6, 24, 4, c->mustache_scale); mii_put_bits(sd, 6, 28, 4, c->mole_scale);
    uint16_t crc = mii_crc16(sd, 0x40);
    sd[0x40] = (uint8_t)(crc >> 8);
    sd[0x41] = (uint8_t)(crc & 0xff);
    uint16_t dcrc = mii_crc16(sd, 0x42);
    sd[0x42] = (uint8_t)(dcrc >> 8);
    sd[0x43] = (uint8_t)(dcrc & 0xff);
}
// The database the service answers from: the console's Miis (g_mii_own of them), then the Miis made in the host's stand-in for the editor (AppendMii), up to MII_DB_MAX in all
static mii_charinfo g_mii_db[MII_DB_MAX];
static int g_mii_n, g_mii_own, g_mii_changed;   // g_mii_changed: a Mii was added since the game last asked IsUpdated about the database
static pthread_mutex_t g_mii_mu = PTHREAD_MUTEX_INITIALIZER;
// <0: the file is not a database (logged); else the Miis read
static int mii_db_parse(const uint8_t *b, size_t size, mii_charinfo *out) {
    if (size != MII_DB_SIZE || memcmp(b, "NFDB", 4)) return -1;
    if (b[0x1a94] != 1 || b[0x1a95] > MII_DB_MAX) return -2;
    if (mii_crc16(b, 0x1a96) != (uint16_t)(b[0x1a96] << 8 | b[0x1a97])) return -3;
    for (int i = 0; i < b[0x1a95]; i++) mii_from_store(&out[i], b + 4 + (size_t)i * MII_STORE);
    return b[0x1a95];
}
// The Miis made in the host's stand-in for the editor (AppendMii), kept for every game in <system folder>/host/MiiDatabase.dat so that a Mii made once is there at the next start, as on a
// console; the player's console file is never written. The host's own format: "DMMI", the version (1), the count, 2 bytes of zeros, then the CharInfos
enum { MII_HOST_HEAD = 8 };
const char *dm_system_dir(char *buf, size_t n);   // hle_fs.c: $DM_SYSTEM_DIR, default $DM_ROOT/run/platform/system
static int mii_host_path(char *p, size_t n) {
    char t[1024];
    const char *sys = dm_system_dir(t, sizeof t);
    if (strstr(sys, ".app/")) {
        const char *home = getenv("HOME");
        if (home && *home)
            return snprintf(p, n, "%s/Library/Application Support/nxnative/system/host/MiiDatabase.dat", home) < (int)n;
        const char *root = getenv("DM_ROOT");
        if (root && *root && !strstr(root, ".app/"))
            return snprintf(p, n, "%s/system/host/MiiDatabase.dat", root) < (int)n;
    }
    return snprintf(p, n, "%s/host/MiiDatabase.dat", sys) < (int)n;
}
// <0: not the host's database; else the Miis read into out (room for max)
static int mii_host_parse(const uint8_t *b, size_t size, mii_charinfo *out, int max) {
    if (size < MII_HOST_HEAD || memcmp(b, "DMMI", 4) || b[4] != 1) return -1;
    int n = b[5]; if (n > max || size != MII_HOST_HEAD + (size_t)n * sizeof(mii_charinfo)) return -1;
    memcpy(out, b + MII_HOST_HEAD, (size_t)n * sizeof(mii_charinfo));
    return n;
}
static int mii_host_save(void) {   // under g_mii_mu: the host's Miis, written to a new file that then replaces the old one
    char p[1024], t[1100]; if (!mii_host_path(p, sizeof p)) return 0;
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    int n = g_mii_n - g_mii_own; uint8_t head[MII_HOST_HEAD] = {'D', 'M', 'M', 'I', 1, (uint8_t)n};
    snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "wb");
    int ok = f && fwrite(head, 1, sizeof head, f) == sizeof head && fwrite(g_mii_db + g_mii_own, sizeof(mii_charinfo), (size_t)n, f) == (size_t)n;
    if (f && fclose(f)) ok = 0;
    if (ok) ok = !rename(t, p);
    if (!ok) { unlink(t); LOG("[RED] mii: cannot write %s: the Mii is kept for this run only", p); }
    return ok;
}
static void mii_db_load(void) {
    char t[1024], p[1100]; const char *sys = dm_system_dir(t, sizeof t); g_mii_n = g_mii_own = 0;
    snprintf(p, sizeof p, "%s/save/8000000000000030/MiiDatabase.dat", sys);
    FILE *f = fopen(p, "rb");
    if (f) {
        uint8_t *b = malloc(MII_DB_SIZE + 1); size_t n = b ? fread(b, 1, MII_DB_SIZE + 1, f) : 0; fclose(f);
        int r = b ? mii_db_parse(b, n, g_mii_db) : -1; free(b);
        static const char *why[] = {"", "not a Mii database (size or NFDB)", "an unknown version or count", "its CRC does not match"};
        if (r < 0) LOG("[RED] mii: %s is %s: the database stays empty", p, why[-r]);
        else { g_mii_n = g_mii_own = r; LOG("mii: %d Miis of the console's database from %s", r, p); }
    }
    if (!mii_host_path(p, sizeof p) || !(f = fopen(p, "rb"))) return;
    size_t cap = MII_HOST_HEAD + MII_DB_MAX * sizeof(mii_charinfo) + 1;
    uint8_t *b = malloc(cap); size_t n = b ? fread(b, 1, cap, f) : 0; fclose(f);
    int r = b ? mii_host_parse(b, n, g_mii_db + g_mii_n, MII_DB_MAX - g_mii_n) : -1; free(b);
    if (r < 0) LOG("[RED] mii: %s is not the host's Mii database, or does not fit beside the console's: its Miis are not listed", p);
    else { g_mii_n += r; LOG("mii: %d Miis made in the host's editor, from %s", r, p); }
}
static void mii_db_once(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, mii_db_load); }

// The object keeps its service pointer at +0, which the constructor zeroes and the destructor releases through its vtable when set; the host never sets it
HLE_FORCE("_ZN2nn3mii8Database10InitializeEv", "mii:u service; the host's database: the six default Miis, the console's own when supplied, those made in AppendMii", Result, h_mii_Init, void *self) {
    (void)self; mii_db_once();
    LOG("mii: Database::Initialize (the six default Miis, %d of the console's own, %d made in the host's editor)", g_mii_own, g_mii_n - g_mii_own); return RES_OK;
}
HLE_FORCE("_ZN2nn3mii8Database8FinalizeEv", "mii:u service; nothing to release", void, h_mii_Fin, void *self) { (void)self; }
HLE_FORCE("_ZNK2nn3mii8Database11IsAvailableEv", "mii:u service; always available", int, h_mii_Avail, const void *self) { (void)self; return 1; }
// True once after a Mii was added (AppendMii), for a question that includes the database; the default Miis never change
HLE_FORCE("_ZN2nn3mii8Database9IsUpdatedEi", "mii:u service; true once after AppendMii added a Mii", int, h_mii_Updated, void *self, int src) {
    (void)self; if (!(src & 1)) return 0;
    pthread_mutex_lock(&g_mii_mu); int r = g_mii_changed; g_mii_changed = 0; pthread_mutex_unlock(&g_mii_mu); return r;
}
HLE_FORCE("_ZNK2nn3mii8Database14IsFullDatabaseEv", "mii:u service; full at 100 Miis", int, h_mii_Full, const void *self) { (void)self; return g_mii_n >= MII_DB_MAX; }
// Source flags: 1 the console's database, 2 the six default Miis every console has; the database's come first, as the mii service lists them. Mario Kart 8 Deluxe asks for the defaults with room for 6 and never reads the count
// (main+0x81e954): its boot-time list of Mii icons (main+0x419dc8) builds a head for each of the six, and zeroed entries failed the model's CharInfo check
static int mii_get(void *buf, int n, int src, size_t stride) {
    int k = 0; pthread_mutex_lock(&g_mii_mu);
    for (int i = 0; (src & MII_SRC_DATABASE) && buf && k < n && i < g_mii_n; i++, k++) {
        uint8_t *e = (uint8_t *)buf + (size_t)k * stride;
        memcpy(e, &g_mii_db[i], sizeof(mii_charinfo));
        if (stride > sizeof(mii_charinfo)) { uint32_t source = 0; memcpy(e + sizeof(mii_charinfo), &source, 4); }   // CharInfoElement: the CharInfo and its source (0 = database)
    }
    for (int d = 0; (src & MII_SRC_DEFAULT) && buf && k < n && d < MII_DEFAULTS; d++, k++) {
        uint8_t *e = (uint8_t *)buf + (size_t)k * stride;
        mii_default((mii_charinfo *)e, d >= 3, d);
        if (stride > sizeof(mii_charinfo)) { uint32_t source = 1; memcpy(e + sizeof(mii_charinfo), &source, 4); }   // 1 = default
    }
    pthread_mutex_unlock(&g_mii_mu); return k;
}
HLE_FORCE("_ZNK2nn3mii8Database8GetCountEi", "mii:u service; the database's Miis and the six default ones", int, h_mii_Count, const void *self, int src) {
    (void)self; return (src & MII_SRC_DATABASE ? g_mii_n : 0) + (src & MII_SRC_DEFAULT ? MII_DEFAULTS : 0);
}
HLE_FORCE("_ZN2nn3mii8Database3GetEPiPNS0_15CharInfoElementEii", "mii:u service; the database's Miis and the six default ones", Result, h_mii_GetE, void *self, int *out, void *buf, int n, int src) {
    (void)self; *out = mii_get(buf, n, src, sizeof(mii_charinfo) + 4); return RES_OK;
}
HLE_FORCE("_ZN2nn3mii8Database3GetEPiPNS0_8CharInfoEii", "mii:u service; the database's Miis and the six default ones", Result, h_mii_Get, void *self, int *out, void *buf, int n, int src) {
    (void)self; *out = mii_get(buf, n, src, sizeof(mii_charinfo)); return RES_OK;
}
// A Mii is found by its create ID (CharInfo +0)
static int mii_find(const void *info) { int r = -1; pthread_mutex_lock(&g_mii_mu); for (int i = 0; info && r < 0 && i < g_mii_n; i++) if (!memcmp(g_mii_db[i].create_id, info, 16)) r = i; pthread_mutex_unlock(&g_mii_mu); return r; }
HLE_FORCE("_ZNK2nn3mii8Database8GetIndexEPiRKNS0_8CharInfoE", "mii:u service; the Mii's place in the console's database", Result, h_mii_Index, const void *self, int *out, const void *info) {
    (void)self; *out = mii_find(info); return *out < 0 ? MII_RES_NOT_FOUND : RES_OK;
}
HLE_FORCE("_ZNK2nn3mii8Database12UpdateLatestEPNS0_8CharInfoEi", "mii:u service; the database's copy of the Mii", Result, h_mii_Latest, const void *self, void *info, int src) {
    (void)self; int i = (src & MII_SRC_DATABASE) ? mii_find(info) : -1;
    if (i < 0) return MII_RES_NOT_FOUND;
    memcpy(info, &g_mii_db[i], sizeof(mii_charinfo)); return RES_OK;
}
// RandomDesc {age, gender, race}: gender 0 male, 1 female, 2 either. The host's "random" Mii is the default one of that gender
HLE_FORCE("_ZN2nn3mii8Database11BuildRandomEPNS0_8CharInfoERKNS0_10RandomDescE", "mii:u service; the default Mii", Result, h_mii_Random, void *self, mii_charinfo *out, const int *desc) {
    (void)self; mii_default(out, desc ? desc[1] : 0, -1); return RES_OK;
}
HLE_FORCE("_ZNK2nn3mii8Database12BuildDefaultEPNS0_8CharInfoEi", "mii:u service; default Mii <index>", Result, h_mii_Default, const void *self, mii_charinfo *out, int index) {
    (void)self; mii_default(out, index >= 3, index < 0 || index >= MII_DEFAULTS ? 0 : index); return RES_OK;
}
HLE_FORCE("_ZN2nn3mii8Database6FormatEv", "mii:u service; the database is already empty", Result, h_mii_Format, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn3mii8Database6ImportEPKvm", "mii:u service; importing needs the system's test mode", Result, h_mii_Import, void *self, const void *p, size_t n) {
    (void)self; (void)p; LOG("[RED] mii: Database::Import of %zu bytes refused (the host has no test mode)", n); return MII_RES_TEST_MODE;
}
HLE_FORCE("_ZNK2nn3mii8Database6ExportEPvm", "mii:u service; nothing to export", Result, h_mii_Export, const void *self, void *p, size_t n) {
    (void)self; (void)p; (void)n; LOG("[RED] mii: Database::Export refused"); return MII_RES_TEST_MODE;
}
// MiiModel system data (0100000000000802) stand-in, also the Switch 2's MiiResource (0400000000000849, below). The Mii rendering code an application links (Mario Kart 8 Deluxe 4.0.0: main+0x8f1400..+0x8fc000) reads one texture file
// (NXTexture{Low,Mid}{Linear,SRGB}.dat) and one shape file (Shape{Mid,High}.dat) whole and builds each part of a head from them; the firmware's files are not the host's to ship, so it writes
// files of the same layout in which every part is blank:
//   texture file: +0x0c u32 largest decompressed size of each of the 11 part kinds, +0x38 their alignments, then from +0x64 one 0x1c-byte entry per part (u32 offset, u32 size, u32 stored size,
//     4 zlib parameters, then the texture: u16 width / height at +0x14, format index, mip count and tile mode index at +0x18), hats 132, eyes 62, eyebrows 24, beards 2, wrinkles 12, make-up 12,
//     glasses 20, moles 2, mouths 37, mustaches 6, noselines 18 (main+0x8fb0dc);
//   shape file: +0x0c the 12 largest sizes, +0x3c their alignments, from +0x6c one 0x58-byte entry per part (offset, size, stored size, zlib parameters, five vertex attribute offsets
//     and sizes, index offset and size, bounding box), beards 4, facelines 12, masks 12, hats / foreheads / hair in normal and cap form 132 each, glasses 1, noses 18, noselines 18
//     (main+0x8faee8); then the hair transforms (132 × 0x48, +0x12704) and faceline transforms (12 × 0x24, +0x14c24).
// Parts cannot be absent: the firmware's files have every one, and the Mii code relies on it. A texture whose size is 0 is skipped when the model is built (main+0x8faaf4; Nintendo
// Switch Sports Resort main+0x1cdead0), but the model's texture getter answers for a part by the create flags alone (Switch Sports Resort main+0x1cd5490: the hat texture whenever
// the normal or hat flag was given), so a game that asks for a skipped part's texture gets an empty holder: with every part of size 0, Switch Sports Resort's two-player skate
// registered the hat texture of a Mii model leaving its practice zone (main+0x3fbc9ac) and called nvnTextureGetFlags with a null texture (main+0x49c54). Every texture entry
// points at one shared blank texture instead: 8x4 texels of format index 0 (nn::gfx R8_Unorm, main+0x42d2688), one level, optimal tiling, the same 0x20 zero bytes as the mesh below,
// which the reader inflates into the texture's storage (512 bytes on NVN, one GOB). Shapes cannot be absent either: the faceline is drawn without a check (its position buffer,
// main+0x8286c4: a null buffer read at main+0xa690d0), so every shape entry points at one shared mesh, one vertex at the origin (attribute 0, the position: 4 halves) and the triangle
// (0, 0, 0) (u16 indices at +0x10), 0x20 bytes of zeros inflated with nn::util::DecompressZlib, stored here as one raw deflate block. It builds, and draws nothing. The largest sizes
// size the model's memory pool (main+0x8f2e6c) and the first alignment is that pool's alignment, so they are one page each instead of 0. The first 12 bytes of each header are not read
enum { MII_TEX_PARTS = 327, MII_TEX_SIZE = 0x64 + 0x1c * MII_TEX_PARTS, MII_SHAPE_TABLES = 0x6c + 0x58 * 857 + 0x48 * 132 + 0x24 * 12, MII_MESH_AT = 0x14de0, MII_MESH = 0x20,
       MII_TEXELS_AT = 0x2430, MII_TEX_W = 8, MII_TEX_H = 4 };
_Static_assert(MII_TEX_SIZE == 0x2428 && MII_SHAPE_TABLES == 0x14dd4 && MII_TEXELS_AT >= MII_TEX_SIZE && MII_TEX_W * MII_TEX_H == MII_MESH, "MiiModel stand-in layout");
static const uint8_t mii_mesh_z[] = {0x78, 0x01, 0x01, MII_MESH, 0, (uint8_t)~MII_MESH, 0xff, [7 ... 7 + MII_MESH - 1] = 0, 0x00, MII_MESH, 0x00, 0x01};   // zlib: stored block, Adler-32 of 32 zeros
enum { MII_SHAPE_SIZE = MII_MESH_AT + sizeof mii_mesh_z, MII_TEXFILE_SIZE = MII_TEXELS_AT + sizeof mii_mesh_z };
static void mii_textures(uint8_t *b) {   // every part the one blank texture (see above): its 0x20 zero bytes are the mesh's
    for (int i = 0; i < MII_TEX_PARTS; i++) {
        uint8_t *e = b + 0x64 + 0x1c * i; uint32_t at = MII_TEXELS_AT, size = MII_MESH, stored = sizeof mii_mesh_z; uint16_t w = MII_TEX_W, h = MII_TEX_H;
        memcpy(e, &at, 4); memcpy(e + 4, &size, 4); memcpy(e + 8, &stored, 4);   // data offset, inflated size, stored size
        memcpy(e + 0x14, &w, 2); memcpy(e + 0x16, &h, 2); e[0x18] = 0; e[0x19] = 1; e[0x1a] = 0;   // R8_Unorm, one level, optimal tiling
    }
    memcpy(b + MII_TEXELS_AT, mii_mesh_z, sizeof mii_mesh_z);
}
static void mii_shapes(uint8_t *b) {
    for (int i = 0; i < 857; i++) {
        uint32_t *e = (uint32_t *)(b + 0x6c + 0x58 * i);
        e[0] = MII_MESH_AT; e[1] = MII_MESH; e[2] = sizeof mii_mesh_z;   // data offset, inflated size, stored size
        e[4 + 5] = 8;                                                    // attribute 0 (offset 0) is one 8-byte vertex; attributes 1-4 absent
        e[14] = 0x10; e[15] = 6;                                         // index offset and size: three u16 indices
    }
    memcpy(b + MII_MESH_AT, mii_mesh_z, sizeof mii_mesh_z);
}
static int mii_write(const char *dir, const char *name, int parts, size_t size) {
    char p[1280]; snprintf(p, sizeof p, "%s/%s", dir, name);
    uint8_t *b = calloc(1, size); FILE *f = fopen(p, "wb");
    for (int i = 0; b && i < parts; i++) { uint32_t page = 0x1000; memcpy(b + 0xc + 4 * i, &page, 4); memcpy(b + 0xc + 4 * parts + 4 * i, &page, 4); }
    if (b && parts == 12) mii_shapes(b);
    if (b && parts == 11) mii_textures(b);
    int ok = b && f && fwrite(b, 1, size, f) == size;
    if (f && fclose(f)) ok = 0;
    free(b);
    if (!ok) LOG("[RED] mii: cannot write the Mii model stand-in %s", p);
    return ok;
}
// One stand-in serves both archives, the Switch's MiiModel (0100000000000802) and the Switch 2's (0400000000000849), whose files have the same layout (Nintendo Switch Sports Resort's
// accessors, main+0x1cdcd80..+0x1cdd0c4: 11 texture part kinds from +0x64 in 0x1c-byte entries, 12 shape kinds from +0x6c in 0x58-byte entries, hair transforms at +0x12704, faceline
// at +0x14c24); the Switch 2's adds its own texture sets, OunceTexture{Low,Mid,High}{Linear,SRGB}.dat, and a game reads only the files it names. Written once per process (the query
// and the mount both ask, for either archive), so a host with another layout never leaves old files behind; again only if the directory changes (a test's DM_TEMP)
int dm_mii_standin(const char *dir) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static char done_dir[1024]; static int done;
    pthread_mutex_lock(&mu);
    if (done && !strcmp(done_dir, dir)) { pthread_mutex_unlock(&mu); return done > 0; }
    char p[1024]; strlcpy(p, dir, sizeof p);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
    static const char *tex[] = {"NXTextureLowLinear.dat", "NXTextureLowSRGB.dat", "NXTextureMidLinear.dat", "NXTextureMidSRGB.dat",
                                "OunceTextureLowLinear.dat", "OunceTextureLowSRGB.dat", "OunceTextureMidLinear.dat", "OunceTextureMidSRGB.dat", "OunceTextureHighLinear.dat", "OunceTextureHighSRGB.dat"},
                      *shape[] = {"ShapeMid.dat", "ShapeHigh.dat"};
    int ok = 1;
    for (size_t i = 0; i < sizeof tex / sizeof *tex; i++) ok &= mii_write(dir, tex[i], 11, MII_TEXFILE_SIZE);
    for (size_t i = 0; i < sizeof shape / sizeof *shape; i++) ok &= mii_write(dir, shape[i], 12, MII_SHAPE_SIZE);
    LOG("mii: Mii model system data is the host's stand-in (every texture part one blank 8x4 texture, every shape one degenerate triangle: Mii heads draw nothing) in %s", dir);
    strlcpy(done_dir, dir, sizeof done_dir); done = ok ? 1 : -1;
    pthread_mutex_unlock(&mu);
    return ok;
}

// The Mii editor (MiiEdit library applet). AppendMii opens it to create a Mii: front_mii_editor launches the
// real MiiEdit applet when present (returning 2) or shows the host's native UI (returning 1), and saving adds it
// to the database and keeps it in the host's file (mii_host_save); *index is its place in the database.
// Headless runs (front_mii_editor returning -1) save the default Mii directly. A full database cancels, as the editor cannot add one then
HLE_FORCE("_ZN2nn3mii9AppendMiiEPi", "MiiEdit applet; create Mii and add to database", Result, h_mii_Append, int *index) {
    DM_APPLET("Mii editor"); mii_db_once();
    pthread_mutex_lock(&g_mii_mu);
    if (g_mii_n >= MII_DB_MAX) { pthread_mutex_unlock(&g_mii_mu); LOG("mii: AppendMii: the database is full (cancelled)"); return MII_RES_CANCELED; }
    int old_n = g_mii_n;
    pthread_mutex_unlock(&g_mii_mu);

    mii_charinfo new_mii;
    mii_default(&new_mii, 0, -1);

    int saved = front_mii_editor(&new_mii, 1);
    if (saved == 0) {
        LOG("mii: AppendMii: cancelled by player");
        return MII_RES_CANCELED;
    }
    if (saved == 2) {
        pthread_mutex_lock(&g_mii_mu);
        mii_db_load();
        int new_n = g_mii_n;
        int i = front_mii_editor_last_index();
        if (i < 0 || i >= new_n) {
            i = (new_n > old_n) ? (new_n - 1) : -1;
        }
        if (i >= 0) g_mii_changed = 1;
        pthread_mutex_unlock(&g_mii_mu);
        if (i < 0) {
            LOG("mii: AppendMii: MiiEdit applet exited without adding a Mii");
            return MII_RES_CANCELED;
        }
        if (index) *index = i;
        LOG("mii: AppendMii: MiiEdit applet stored Mii at database entry %d", i);
        return RES_OK;
    }

    pthread_mutex_lock(&g_mii_mu);
    if (g_mii_n >= MII_DB_MAX) { pthread_mutex_unlock(&g_mii_mu); LOG("mii: AppendMii: database became full"); return MII_RES_CANCELED; }
    int i = g_mii_n++;
    memcpy(&g_mii_db[i], &new_mii, sizeof(mii_charinfo));
    g_mii_changed = 1;
    int kept = mii_host_save();
    pthread_mutex_unlock(&g_mii_mu);
    if (index) *index = i;
    LOG("mii: AppendMii: Mii is added as database entry %d%s", i, kept ? "" : " (this run only)");
    return RES_OK;
}
HLE_FORCE("_ZN2nn3mii11ShowMiiEditEv", "MiiEdit applet; show native Mii editor", Result, h_mii_Edit, void) {
    DM_APPLET("Mii editor"); mii_db_once();
    pthread_mutex_lock(&g_mii_mu);
    if (g_mii_n > 0) {
        int target = g_mii_n - 1;
        mii_charinfo edit_mii = g_mii_db[target];
        pthread_mutex_unlock(&g_mii_mu);

        int saved = front_mii_editor(&edit_mii, 0);
        if (saved == 2) {
            pthread_mutex_lock(&g_mii_mu);
            mii_db_load();
            g_mii_changed = 1;
            pthread_mutex_unlock(&g_mii_mu);
            LOG("mii: ShowMiiEdit: reloaded database after MiiEdit applet (%d Miis)", g_mii_n);
        } else if (saved == 1) {
            pthread_mutex_lock(&g_mii_mu);
            memcpy(&g_mii_db[target], &edit_mii, sizeof(mii_charinfo));
            g_mii_changed = 1;
            mii_host_save();
            pthread_mutex_unlock(&g_mii_mu);
            LOG("mii: ShowMiiEdit: updated Mii entry %d", target);
        }
    } else {
        pthread_mutex_unlock(&g_mii_mu);
        mii_charinfo new_mii;
        mii_default(&new_mii, 0, -1);
        int saved = front_mii_editor(&new_mii, 0);
        if (saved == 2) {
            pthread_mutex_lock(&g_mii_mu);
            mii_db_load();
            if (g_mii_n > 0) g_mii_changed = 1;
            pthread_mutex_unlock(&g_mii_mu);
            LOG("mii: ShowMiiEdit: reloaded database after MiiEdit applet (%d Miis)", g_mii_n);
        } else if (saved == 1) {
            pthread_mutex_lock(&g_mii_mu);
            if (g_mii_n < MII_DB_MAX) {
                int i = g_mii_n++;
                memcpy(&g_mii_db[i], &new_mii, sizeof(mii_charinfo));
                g_mii_changed = 1;
                mii_host_save();
                LOG("mii: ShowMiiEdit: created Mii entry %d", i);
            }
            pthread_mutex_unlock(&g_mii_mu);
        }
    }
    return RES_OK;
}

static int mii_get_store(void *buf, int n, int src, size_t stride) {
    int k = 0; pthread_mutex_lock(&g_mii_mu);
    for (int i = 0; (src & MII_SRC_DATABASE) && buf && k < n && i < g_mii_n; i++, k++) {
        uint8_t *e = (uint8_t *)buf + (size_t)k * stride;
        mii_to_store(e, &g_mii_db[i]);
        if (stride > MII_STORE) { uint32_t source = 0; memcpy(e + MII_STORE, &source, 4); }
    }
    for (int d = 0; (src & MII_SRC_DEFAULT) && buf && k < n && d < MII_DEFAULTS; d++, k++) {
        uint8_t *e = (uint8_t *)buf + (size_t)k * stride;
        mii_charinfo def_c;
        mii_default(&def_c, d >= 3, d);
        mii_to_store(e, &def_c);
        if (stride > MII_STORE) { uint32_t source = 1; memcpy(e + MII_STORE, &source, 4); }
    }
    pthread_mutex_unlock(&g_mii_mu); return k;
}

static Result mii_database_answer(sf_req *q) {
    LOG("sf: IDatabaseService command %u (%zu data words in, nb=%u, na=%u, nx=%u)", q->cmd, q->in_len / 4, q->nb, q->na, q->nx);
    mii_db_once();
    int src_flag = (q->in_len >= 4) ? *(int32_t *)q->in : MII_SRC_DATABASE;
    if (src_flag == 0) src_flag = MII_SRC_DATABASE;
    if (q->cmd == 0) { // IsUpdated
        pthread_mutex_lock(&g_mii_mu);
        int r = (src_flag & MII_SRC_DATABASE) ? g_mii_changed : 0;
        if (src_flag & MII_SRC_DATABASE) g_mii_changed = 0;
        pthread_mutex_unlock(&g_mii_mu);
        q->out_len = 1;
        *(uint8_t *)q->out = (uint8_t)r;
        return RES_OK;
    }
    if (q->cmd == 1) { // IsFullDatabase
        pthread_mutex_lock(&g_mii_mu);
        uint8_t full = (g_mii_n >= MII_DB_MAX);
        pthread_mutex_unlock(&g_mii_mu);
        q->out_len = 1;
        *(uint8_t *)q->out = full;
        return RES_OK;
    }
    if (q->cmd == 2) { // GetCount(SourceFlag)
        pthread_mutex_lock(&g_mii_mu);
        int32_t cnt = (src_flag & MII_SRC_DATABASE ? g_mii_n : 0) + (src_flag & MII_SRC_DEFAULT ? MII_DEFAULTS : 0);
        pthread_mutex_unlock(&g_mii_mu);
        q->out_len = 4;
        *(int32_t *)q->out = cnt;
        return RES_OK;
    }
    if (q->cmd == 3) { // Get (CharInfoElement array, 0x5c each: CharInfo + u32 source)
        int max_elem = (q->nb > 0 && q->b[0].p) ? (int)(q->b[0].size / (sizeof(mii_charinfo) + 4)) : 0;
        int count = mii_get(q->nb > 0 ? q->b[0].p : NULL, max_elem, src_flag, sizeof(mii_charinfo) + 4);
        q->out_len = 4;
        *(int32_t *)q->out = count;
        return RES_OK;
    }
    if (q->cmd == 4) { // Get1 (CharInfo array, 0x58 each)
        int max_elem = (q->nb > 0 && q->b[0].p) ? (int)(q->b[0].size / sizeof(mii_charinfo)) : 0;
        int count = mii_get(q->nb > 0 ? q->b[0].p : NULL, max_elem, src_flag, sizeof(mii_charinfo));
        q->out_len = 4;
        *(int32_t *)q->out = count;
        return RES_OK;
    }
    if (q->cmd == 5) { // UpdateLatest(CharInfo, SourceFlag) -> CharInfo
        if (q->in_len >= sizeof(mii_charinfo)) {
            mii_charinfo c;
            memcpy(&c, q->in, sizeof c);
            int idx = mii_find(c.create_id);
            if (idx < 0) return MII_RES_NOT_FOUND;
            pthread_mutex_lock(&g_mii_mu);
            c = g_mii_db[idx];
            pthread_mutex_unlock(&g_mii_mu);
            q->out_len = sizeof c;
            memcpy(q->out, &c, sizeof c);
            return RES_OK;
        }
        return RES_OK;
    }
    if (q->cmd == 6) { // BuildRandom
        mii_charinfo c;
        int gender = (q->in_len >= 8) ? *(int32_t *)(q->in + 4) : 0;
        mii_default(&c, gender, -1);
        if (q->nb > 0 && q->b[0].p && q->b[0].size >= sizeof(mii_charinfo)) {
            memcpy(q->b[0].p, &c, sizeof(mii_charinfo));
        } else {
            q->out_len = sizeof(mii_charinfo);
            memcpy(q->out, &c, sizeof(mii_charinfo));
        }
        return RES_OK;
    }
    if (q->cmd == 7) { // BuildDefault
        mii_charinfo c;
        int idx = (q->in_len >= 4) ? *(int32_t *)q->in : 0;
        mii_default(&c, idx >= 3, idx < 0 || idx >= MII_DEFAULTS ? 0 : idx);
        if (q->nb > 0 && q->b[0].p && q->b[0].size >= sizeof(mii_charinfo)) {
            memcpy(q->b[0].p, &c, sizeof(mii_charinfo));
        } else {
            q->out_len = sizeof(mii_charinfo);
            memcpy(q->out, &c, sizeof(mii_charinfo));
        }
        return RES_OK;
    }
    if (q->cmd == 8) { // Get2 (StoreDataElement array, 0x48 each: StoreData + u32 source)
        int max_elem = (q->nb > 0 && q->b[0].p) ? (int)(q->b[0].size / (MII_STORE + 4)) : 0;
        int count = mii_get_store(q->nb > 0 ? q->b[0].p : NULL, max_elem, src_flag, MII_STORE + 4);
        q->out_len = 4;
        *(int32_t *)q->out = count;
        return RES_OK;
    }
    if (q->cmd == 9) { // Get3 (StoreData array, 0x44 each)
        int max_elem = (q->nb > 0 && q->b[0].p) ? (int)(q->b[0].size / MII_STORE) : 0;
        int count = mii_get_store(q->nb > 0 ? q->b[0].p : NULL, max_elem, src_flag, MII_STORE);
        q->out_len = 4;
        *(int32_t *)q->out = count;
        return RES_OK;
    }
    if (q->cmd == 10) { // UpdateLatest1(StoreData, SourceFlag) -> StoreData
        if (q->in_len >= MII_STORE) {
            int idx = mii_find(q->in + 0x30);
            if (idx < 0) return MII_RES_NOT_FOUND;
            q->out_len = MII_STORE;
            pthread_mutex_lock(&g_mii_mu);
            mii_to_store(q->out, &g_mii_db[idx]);
            pthread_mutex_unlock(&g_mii_mu);
            return RES_OK;
        }
        return MII_RES_NOT_FOUND;
    }
    if (q->cmd == 11) { // FindIndex(CreateId, is_special) -> s32
        int idx = (q->in_len >= 16) ? mii_find(q->in) : -1;
        q->out_len = 4;
        *(int32_t *)q->out = idx;
        return idx < 0 ? MII_RES_NOT_FOUND : RES_OK;
    }
    if (q->cmd == 12) { // Move(CreateId, s32 new_index)
        if (q->in_len >= 20) {
            int old_idx = mii_find(q->in);
            int new_idx = *(int32_t *)(q->in + 16);
            pthread_mutex_lock(&g_mii_mu);
            if (old_idx >= 0 && new_idx >= 0 && new_idx < g_mii_n && old_idx != new_idx) {
                mii_charinfo tmp = g_mii_db[old_idx];
                if (old_idx < new_idx) memmove(&g_mii_db[old_idx], &g_mii_db[old_idx + 1], (size_t)(new_idx - old_idx) * sizeof(mii_charinfo));
                else memmove(&g_mii_db[new_idx + 1], &g_mii_db[new_idx], (size_t)(old_idx - new_idx) * sizeof(mii_charinfo));
                g_mii_db[new_idx] = tmp;
                g_mii_changed = 1;
                mii_host_save();
            }
            pthread_mutex_unlock(&g_mii_mu);
        }
        return RES_OK;
    }
    if (q->cmd == 13) { // AddOrReplace(StoreData)
        pthread_mutex_lock(&g_mii_mu);
        const uint8_t *src = NULL;
        size_t sz = 0;
        if (q->na > 0 && q->a[0].p && q->a[0].size >= MII_STORE) { src = q->a[0].p; sz = q->a[0].size; }
        else if (q->nb > 0 && q->b[0].p && q->b[0].size >= MII_STORE) { src = q->b[0].p; sz = q->b[0].size; }
        else if (q->in_len >= MII_STORE) { src = q->in; sz = q->in_len; }
        if (src) {
            mii_charinfo c;
            if (sz == sizeof(mii_charinfo)) memcpy(&c, src, sizeof c);
            else mii_from_store(&c, src);
            int existing = -1;
            for (int i = 0; i < g_mii_n; i++) {
                if (!memcmp(g_mii_db[i].create_id, c.create_id, 16)) { existing = i; break; }
            }
            if (existing >= 0) {
                g_mii_db[existing] = c;
                g_mii_changed = 1;
                mii_host_save();
                LOG("mii: IDatabaseService AddOrReplace: updated Mii at entry %d", existing);
            } else if (g_mii_n < MII_DB_MAX) {
                g_mii_db[g_mii_n++] = c;
                g_mii_changed = 1;
                mii_host_save();
                LOG("mii: IDatabaseService AddOrReplace: added Mii at entry %d", g_mii_n - 1);
            }
        }
        pthread_mutex_unlock(&g_mii_mu);
        return RES_OK;
    }
    if (q->cmd == 14) { // Delete(CreateId)
        if (q->in_len >= 16) {
            pthread_mutex_lock(&g_mii_mu);
            for (int i = 0; i < g_mii_n; i++) {
                if (!memcmp(g_mii_db[i].create_id, q->in, 16)) {
                    if (i + 1 < g_mii_n) memmove(&g_mii_db[i], &g_mii_db[i + 1], (size_t)(g_mii_n - i - 1) * sizeof(mii_charinfo));
                    g_mii_n--;
                    if (i < g_mii_own && g_mii_own > 0) g_mii_own--;
                    g_mii_changed = 1;
                    mii_host_save();
                    LOG("mii: IDatabaseService Delete: removed Mii at entry %d", i);
                    break;
                }
            }
            pthread_mutex_unlock(&g_mii_mu);
        }
        return RES_OK;
    }
    if (q->cmd == 15 || q->cmd == 16 || q->cmd == 17) { // DestroyFile / DeleteFile / Format
        return RES_OK;
    }
    if (q->cmd == 20) { // IsBrokenDatabaseWithClearFlag -> u8
        q->out_len = 1;
        *(uint8_t *)q->out = 0;
        return RES_OK;
    }
    if (q->cmd == 21) { // GetIndex(CharInfo) -> s32
        int idx = (q->in_len >= 16) ? mii_find(q->in) : -1;
        q->out_len = 4;
        *(int32_t *)q->out = idx;
        return idx < 0 ? MII_RES_NOT_FOUND : RES_OK;
    }
    if (q->cmd == 22) { // SetInterfaceVersion(u32)
        return RES_OK;
    }
    return RES_OK;
}

static Result mii_static_answer(sf_req *q) {
    LOG("sf: mii:e/u command %u (%zu data words in)", q->cmd, q->in_len / 4);
    if (q->cmd == 0) { // GetDatabaseService
        mii_db_once();
        q->out_move = sf_session("IDatabaseService", mii_database_answer, NULL, NULL);
        return RES_OK;
    }
    return RES_OK;
}
static Result mii_e_answer(sf_req *q) { return mii_static_answer(q); }
static Result mii_u_answer(sf_req *q) { return mii_static_answer(q); }
SF_SERVICE("mii:e", mii_e_answer);
SF_SERVICE("mii:u", mii_u_answer);

static Result miiimg_answer(sf_req *q) {
    LOG("sf: miiimg command %u (%zu data words in, nb=%u)", q->cmd, q->in_len / 4, q->nb);
    if (q->cmd == 0) return RES_OK; // Initialize
    if (q->cmd == 10) return RES_OK; // Reload
    if (q->cmd == 11) { // GetCount
        q->out_len = 4;
        *(uint32_t *)q->out = 0;
        return RES_OK;
    }
    if (q->cmd == 12) { // IsEmpty
        q->out_len = 1;
        *(uint8_t *)q->out = 1;
        return RES_OK;
    }
    if (q->cmd == 13) { // IsFull
        q->out_len = 1;
        *(uint8_t *)q->out = 0;
        return RES_OK;
    }
    return RES_OK;
}
SF_SERVICE("miiimg", miiimg_answer);

