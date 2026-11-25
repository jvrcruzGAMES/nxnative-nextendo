#pragma once
#include <stdint.h>
#include <stddef.h>

#define MII_RES_NOT_FOUND 0x87Eu        // nn::mii module 126, description 4
#define MII_RES_CANCELED 0xE7Eu         // description 7: the player closed the applet (the SDK's ShowMiiEdit treats it as success)
#define MII_RES_TEST_MODE 0x1987Eu      // description 204: importing a database needs the system's test mode

typedef struct __attribute__((packed)) {
    uint8_t create_id[16]; uint16_t nickname[11];
    uint8_t font_region, favorite_color, gender, height, build, type, region_move;
    uint8_t faceline_type, faceline_color, faceline_wrinkle, faceline_make;
    uint8_t hair_type, hair_color, hair_flip;
    uint8_t eye_type, eye_color, eye_scale, eye_aspect, eye_rotate, eye_x, eye_y;
    uint8_t eyebrow_type, eyebrow_color, eyebrow_scale, eyebrow_aspect, eyebrow_rotate, eyebrow_x, eyebrow_y;
    uint8_t nose_type, nose_scale, nose_y;
    uint8_t mouth_type, mouth_color, mouth_scale, mouth_aspect, mouth_y;
    uint8_t beard_color, beard_type, mustache_type, mustache_scale, mustache_y;
    uint8_t glass_type, glass_color, glass_scale, glass_y;
    uint8_t mole_type, mole_scale, mole_x, mole_y, reserved;
} mii_charinfo;
_Static_assert(sizeof(mii_charinfo) == 0x58, "nn::mii::CharInfo is 0x58 bytes");

enum { MII_SRC_DATABASE = 1, MII_SRC_DEFAULT = 2, MII_DEFAULTS = 6 };

typedef struct {
    const char *name;
    uint8_t gender;          // 0 = male, 1 = female
    uint8_t favorite_color;  // 0: red, 1: orange, 2: yellow, 3: lime, 4: green, 5: blue, 6: cyan, 7: pink, 8: purple, 9: brown, 10: white, 11: black
    uint8_t faceline_color;  // skin tone
    uint8_t hair_type;
    uint8_t hair_color;
    uint8_t eye_type;
    uint8_t eye_color;
    uint8_t eyebrow_type;
    uint8_t eyebrow_color;
    uint8_t nose_type;
    uint8_t mouth_type;
} mii_guest_preset;

static const mii_guest_preset s_guest_miis[MII_DEFAULTS] = {
    // Guest A (Male, Green, Black Hair)
    { "Guest A", 0, 4, 3, 20, 1, 0, 8, 0, 1, 0, 0 },
    // Guest B (Male, Blue, Dark Blonde Hair)
    { "Guest B", 0, 5, 0,  9, 7, 0, 5, 0, 7, 0, 0 },
    // Guest C (Male, Red, Brown Hair)
    { "Guest C", 0, 0, 2,  1, 2, 0, 8, 0, 2, 0, 0 },
    // Guest D (Female, Yellow, Black Hair)
    { "Guest D", 1, 2, 5, 98, 1, 1, 8, 1, 1, 0, 0 },
    // Guest E (Female, Cyan, Blonde Hair)
    { "Guest E", 1, 6, 0, 61, 7, 1, 4, 1, 7, 0, 0 },
    // Guest F (Female, Pink, Brown Hair)
    { "Guest F", 1, 7, 0, 55, 2, 1, 8, 1, 2, 0, 0 },
};

void dm_mii_applet_prepare(void *charinfo_out, int is_append);
int dm_mii_applet_commit(const void *charinfo, int saved, int is_append, int applet_index);
