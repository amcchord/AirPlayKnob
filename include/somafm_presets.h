#pragma once
#include <stdint.h>

struct StationPreset {
    char id[24];
    char name[40];
    char genre[32];
    char url[128];
    uint32_t color;
};

static const StationPreset DEFAULT_SOMAFM_PRESETS[] = {
    {"groovesalad",   "Groove Salad",          "Ambient / Chill",    "http://ice6.somafm.com/groovesalad-128-mp3",   0x10B981},
    {"dronezone",     "Drone Zone",            "Atmospheric Ambient","http://ice6.somafm.com/dronezone-128-mp3",     0x06B6D4},
    {"indiepop",      "Indie Pop Rocks!",      "Indie / Alt Rock",   "http://ice6.somafm.com/indiepop-128-mp3",      0xF59E0B},
    {"deepspaceone",  "Deep Space One",        "Deep Space Ambient", "http://ice6.somafm.com/deepspaceone-128-mp3",  0x6366F1},
    {"gsclassic",     "Groove Salad Classic",  "Early 2000s Chill",  "http://ice6.somafm.com/gsclassic-128-mp3",     0x34D399},
    {"spacestation",  "Space Station Soma",    "Mid-tempo Electronica","http://ice6.somafm.com/spacestation-128-mp3",0x8B5CF6},
    {"u80s",          "Underground 80s",       "80s Synthpop / Wave","http://ice6.somafm.com/u80s-128-mp3",          0xEC4899},
    {"synphaera",     "Synphaera Radio",       "Modern Electronic",  "http://ice6.somafm.com/synphaera-128-mp3",     0x38BDF8},
    {"secretagent",   "Secret Agent",          "Spy Lounge / Bossa", "http://ice6.somafm.com/secretagent-128-mp3",   0xEF4444},
    {"lush",          "Lush",                  "Sensuous Vocals",    "http://ice6.somafm.com/lush-128-mp3",          0xF43F5E},
    {"defcon",        "DEF CON Radio",         "Hacker / Electronic","http://ice6.somafm.com/defcon-128-mp3",        0x22C55E},
    {"seventies",     "Left Coast 70s",        "Mellow 70s Rock",    "http://ice6.somafm.com/seventies-128-mp3",     0xFB923C},
    {"folkfwd",       "Folk Forward",          "Indie Folk / Acoustic","http://ice6.somafm.com/folkfwd-128-mp3",     0xA3E635},
    {"beatblender",   "Beat Blender",          "Deep House / Dub",   "http://ice6.somafm.com/beatblender-128-mp3",   0xA855F7},
    {"bootliquor",    "Boot Liquor",           "Americana / Roots",  "http://ice6.somafm.com/bootliquor-128-mp3",    0xD97706},
    {"bossa",         "Bossa Beyond",          "Bossa Nova / Samba", "http://ice6.somafm.com/bossa-128-mp3",         0xFBBF24},
    {"poptron",       "PopTron",               "Electropop / Indie", "http://ice6.somafm.com/poptron-128-mp3",       0xE879F9},
    {"darkzone",      "The Dark Zone",         "Dark Ambient",       "http://ice6.somafm.com/darkzone-128-mp3",      0x64748B},
    {"sonicuniverse", "Sonic Universe",        "Nu-Jazz / Avant",    "http://ice6.somafm.com/sonicuniverse-128-mp3", 0x3B82F6},
    {"fluid",         "Fluid",                 "Instrumental HipHop","http://ice6.somafm.com/fluid-128-mp3",         0x14B8A6},
    {"thetrip",       "The Trip",              "Progressive House",  "http://ice6.somafm.com/thetrip-128-mp3",       0xC084FC},
    {"7soul",         "Seven Inch Soul",       "Vintage Soul 45s",   "http://ice6.somafm.com/7soul-128-mp3",         0xF97316},
    {"thistle",       "ThistleRadio",          "Celtic Roots",       "http://ice6.somafm.com/thistle-128-mp3",       0x4ADE80},
    {"vaporwaves",    "Vaporwaves",            "Vaporwave / Future", "http://ice6.somafm.com/vaporwaves-128-mp3",    0xF472B6},
    {"illstreet",     "Illinois Street Lounge","Exotica / Bachelor", "http://ice6.somafm.com/illstreet-128-mp3",     0xFACC15},
    {"suburbsofgoa",  "Suburbs of Goa",        "Asian World Beats",  "http://ice6.somafm.com/suburbsofgoa-128-mp3",  0xFB7185},
    {"cliqhop",       "cliqhop idm",           "IDM / Glitch Beats", "http://ice6.somafm.com/cliqhop-128-mp3",       0x2DD4BF},
    {"digitalis",     "Digitalis",             "Analog Rock",        "http://ice6.somafm.com/digitalis-128-mp3",     0x818CF8},
    {"tikitime",      "Tiki Time",             "Tiki / Island",      "http://ice6.somafm.com/tikitime-128-mp3",      0x22D3EE},
    {"missioncontrol","Mission Control",       "NASA + Ambient",     "http://ice6.somafm.com/missioncontrol-128-mp3",0x60A5FA}
};

static const int DEFAULT_SOMAFM_PRESET_COUNT = sizeof(DEFAULT_SOMAFM_PRESETS) / sizeof(DEFAULT_SOMAFM_PRESETS[0]);
