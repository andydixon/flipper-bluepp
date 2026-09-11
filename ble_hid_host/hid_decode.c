#include "hid_decode.h"

// USB HID Usage Table (keyboard/keypad page 0x07), common subset.
const char* hid_key_name(uint8_t u) {
    static char one[2];
    if(u >= 0x04 && u <= 0x1D) { // a-z
        one[0] = 'a' + (u - 0x04);
        one[1] = 0;
        return one;
    }
    if(u >= 0x1E && u <= 0x26) { // 1-9
        one[0] = '1' + (u - 0x1E);
        one[1] = 0;
        return one;
    }
    switch(u) {
    case 0x27: return "0";
    case 0x28: return "Enter";
    case 0x29: return "Esc";
    case 0x2A: return "Bksp";
    case 0x2B: return "Tab";
    case 0x2C: return "Space";
    case 0x2D: return "-";
    case 0x2E: return "=";
    case 0x2F: return "[";
    case 0x30: return "]";
    case 0x31: return "\\";
    case 0x33: return ";";
    case 0x34: return "'";
    case 0x35: return "`";
    case 0x36: return ",";
    case 0x37: return ".";
    case 0x38: return "/";
    case 0x39: return "Caps";
    case 0x3A: return "F1";
    case 0x3B: return "F2";
    case 0x3C: return "F3";
    case 0x3D: return "F4";
    case 0x3E: return "F5";
    case 0x3F: return "F6";
    case 0x40: return "F7";
    case 0x41: return "F8";
    case 0x42: return "F9";
    case 0x43: return "F10";
    case 0x44: return "F11";
    case 0x45: return "F12";
    case 0x46: return "PrtSc";
    case 0x47: return "ScrLk";
    case 0x48: return "Pause";
    case 0x49: return "Ins";
    case 0x4A: return "Home";
    case 0x4B: return "PgUp";
    case 0x4C: return "Del";
    case 0x4D: return "End";
    case 0x4E: return "PgDn";
    case 0x4F: return "Right";
    case 0x50: return "Left";
    case 0x51: return "Down";
    case 0x52: return "Up";
    case 0x53: return "NumLk";
    default: return NULL;
    }
}

void hid_mods_str(FuriString* out, uint8_t m) {
    if(m & 0x01) furi_string_cat_str(out, "LCtrl+");
    if(m & 0x02) furi_string_cat_str(out, "LShift+");
    if(m & 0x04) furi_string_cat_str(out, "LAlt+");
    if(m & 0x08) furi_string_cat_str(out, "LGui+");
    if(m & 0x10) furi_string_cat_str(out, "RCtrl+");
    if(m & 0x20) furi_string_cat_str(out, "RShift+");
    if(m & 0x40) furi_string_cat_str(out, "RAlt+");
    if(m & 0x80) furi_string_cat_str(out, "RGui+");
}
