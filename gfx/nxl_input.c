// SPDX-License-Identifier: GPL-3.0-or-later
// Controllers and keyboard for the game (SDL3), feeding nxnative's nn::hid layer (dm_input.h, hle_hid.c) the way its Mac
// front end does (front_mac.m):
//   - each gamepad SDL knows (Xbox, PlayStation, Switch Pro, 8BitDo, ...) holds a player slot (pad_slots.h: player 1
//     first, a reconnected controller gets its player back) and is that player's controller (dm_pad_set_player);
//   - face buttons by position, as on a Switch: right = A, bottom = B, top = X, left = Y (so an Xbox controller's B is
//     the Switch's A); NXL_PAD_LAYOUT=labels maps them by their letters instead (Xbox A = Switch A);
//   - bumpers L / R, triggers ZL / ZR, Start / Menu = +, Back / View = -, stick clicks, d-pad; sticks with a radial
//     dead zone (NXL_DEADZONE, default 0.12 of full travel; the Switch's own sticks report a small one);
//   - the keyboard is player 1's too (ORed with player 1's controller): WASD left stick, J K U I = A B X Y, F G = stick
//     clicks, Q E Z C = L R ZL ZR, Return = +, Tab = -, arrows = d-pad, Space = shake;
//   - rumble: the game's vibration (hle_haptics.c) on the controller's two motors (DM_RUMBLE=0 turns it off,
//     DM_RUMBLE_GAIN scales it);
//   - when the window loses focus everything is released, so nothing stays held.
// The presenting thread owns the window and calls nxl_input_event for each SDL event and nxl_input_tick once per pump.
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <SDL3/SDL.h>
#include "dm_input.h"
#include "hle_haptics.h"
#include "pad_slots.h"
#include "nxl_input.h"

static pad_slots g_slots;
static SDL_Gamepad *g_pad[DM_PLAYERS];
static int g_labels;           // NXL_PAD_LAYOUT=labels
static float g_dead = 0.12f;   // NXL_DEADZONE
static float g_gain = 1.0f;    // DM_RUMBLE_GAIN
static int g_focus = 1;
static _Atomic int g_block;     // NativeNX's menu is over the game: the game sees nothing pressed (nxl_input_block); 2: it
                                // closed, and the game gets the buttons back once none is held (not the B that closed it)
static uint64_t g_kbtn;
static int g_kw, g_ka, g_ks, g_kd;

// ---------- rumble: the sink runs under hle_haptics' lock on a game thread; the hardware is driven from the tick ----------
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static struct { float low, high; int dirty; uint64_t sent_ms; } g_rumble[DM_PLAYERS];
// The game set a controller's vibration: kept for the window's thread to send to the controller.
static void rumble_sink(dm_haptic_state s) {
    if (s.player < 0 || s.player >= DM_PLAYERS) return;
    float lo = fmaxf(s.motor[0].amplitude_low, s.motor[1].amplitude_low), hi = fmaxf(s.motor[0].amplitude_high, s.motor[1].amplitude_high);
    pthread_mutex_lock(&g_rmu);
    g_rumble[s.player].low = lo, g_rumble[s.player].high = hi, g_rumble[s.player].dirty = 1;
    pthread_mutex_unlock(&g_rmu);
}
static uint16_t motor(float a) {
    float v = a * g_gain;
    if (!(v > 0)) return 0;
    return v >= 1 ? 0xFFFF : (uint16_t)(v * 65535.f);
}
// Sends the vibration the game set to each controller (renewed before SDL's duration runs out).
static void rumble_tick(void) {
    uint64_t now = SDL_GetTicks();
    for (int p = 0; p < DM_PLAYERS; p++) {
        if (!g_pad[p]) continue;
        pthread_mutex_lock(&g_rmu);
        float lo = g_rumble[p].low, hi = g_rumble[p].high;
        int send = g_rumble[p].dirty || ((lo > 0 || hi > 0) && now - g_rumble[p].sent_ms > 1000);   // a held vibration is renewed before it runs out
        g_rumble[p].dirty = 0;
        if (send) g_rumble[p].sent_ms = now;
        pthread_mutex_unlock(&g_rmu);
        if (send) SDL_RumbleGamepad(g_pad[p], motor(lo), motor(hi), 2000);
    }
}

// ---------- mapping ----------
int32_t nxl_input_axis(int16_t v) { return v < -DM_STICK_MAX ? -DM_STICK_MAX : v; }
// A stick's position as the Switch reads it: up positive, the dead zone cut and the rest scaled to full travel.
void nxl_input_stick(int16_t x, int16_t y, float dead, int32_t *ox, int32_t *oy) {
    float fx = nxl_input_axis(x) / (float)DM_STICK_MAX, fy = -nxl_input_axis(y) / (float)DM_STICK_MAX;   // SDL: down is positive; the Switch: up is
    float m = sqrtf(fx * fx + fy * fy);
    if (m <= dead || m == 0) { *ox = *oy = 0; return; }
    float s = (fminf(m, 1.f) - dead) / (1.f - dead) / m;   // rescaled so full travel stays full
    *ox = (int32_t)lrintf(fmaxf(-1.f, fminf(1.f, fx * s)) * DM_STICK_MAX);
    *oy = (int32_t)lrintf(fmaxf(-1.f, fminf(1.f, fy * s)) * DM_STICK_MAX);
}
// A controller's buttons as Switch buttons: by position (the Switch layout), or by the letters printed on them.
uint64_t nxl_input_buttons(SDL_Gamepad *g, int labels) {
    uint64_t m = 0;
#define K(sdl, bit) if (SDL_GetGamepadButton(g, sdl)) m |= (bit)
    if (labels && SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_A) {   // Xbox letters: bottom A, right B, left X, top Y
        K(SDL_GAMEPAD_BUTTON_SOUTH, DM_BTN_A); K(SDL_GAMEPAD_BUTTON_EAST, DM_BTN_B); K(SDL_GAMEPAD_BUTTON_WEST, DM_BTN_X); K(SDL_GAMEPAD_BUTTON_NORTH, DM_BTN_Y);
    } else {
        K(SDL_GAMEPAD_BUTTON_EAST, DM_BTN_A); K(SDL_GAMEPAD_BUTTON_SOUTH, DM_BTN_B); K(SDL_GAMEPAD_BUTTON_NORTH, DM_BTN_X); K(SDL_GAMEPAD_BUTTON_WEST, DM_BTN_Y);
    }
    K(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, DM_BTN_L); K(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, DM_BTN_R);
    K(SDL_GAMEPAD_BUTTON_START, DM_BTN_PLUS); K(SDL_GAMEPAD_BUTTON_BACK, DM_BTN_MINUS);
    K(SDL_GAMEPAD_BUTTON_LEFT_STICK, DM_BTN_LS); K(SDL_GAMEPAD_BUTTON_RIGHT_STICK, DM_BTN_RS);
    K(SDL_GAMEPAD_BUTTON_DPAD_LEFT, DM_BTN_LEFT); K(SDL_GAMEPAD_BUTTON_DPAD_UP, DM_BTN_UP);
    K(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DM_BTN_RIGHT); K(SDL_GAMEPAD_BUTTON_DPAD_DOWN, DM_BTN_DOWN);
#undef K
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) m |= DM_BTN_ZL;   // a quarter of the travel
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) m |= DM_BTN_ZR;
    return m;
}
// Reads a controller into its player's state (nothing while the window is not focused or the menu is up).
static void pad_read(int p) {
    SDL_Gamepad *g = g_pad[p];
    if (!g) return;
    if (!g_focus || g_block) { dm_pad_set_player(p, 0, 0, 0, 0, 0); return; }
    int32_t lx, ly, rx, ry;
    nxl_input_stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY), g_dead, &lx, &ly);
    nxl_input_stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY), g_dead, &rx, &ry);
    dm_pad_set_player(p, nxl_input_buttons(g, g_labels), lx, ly, rx, ry);
}

// the keyboard: the Mac front end's keys, by position (scancodes), so other layouts keep the same places
static uint64_t key_bit(SDL_Scancode c) {
    switch (c) {
    case SDL_SCANCODE_J: return DM_BTN_A;      case SDL_SCANCODE_K: return DM_BTN_B;
    case SDL_SCANCODE_U: return DM_BTN_X;      case SDL_SCANCODE_I: return DM_BTN_Y;
    case SDL_SCANCODE_F: return DM_BTN_LS;     case SDL_SCANCODE_G: return DM_BTN_RS;
    case SDL_SCANCODE_Q: return DM_BTN_L;      case SDL_SCANCODE_E: return DM_BTN_R;
    case SDL_SCANCODE_Z: return DM_BTN_ZL;     case SDL_SCANCODE_C: return DM_BTN_ZR;
    case SDL_SCANCODE_RETURN: return DM_BTN_PLUS; case SDL_SCANCODE_TAB: return DM_BTN_MINUS;
    case SDL_SCANCODE_LEFT: return DM_BTN_LEFT;   case SDL_SCANCODE_UP: return DM_BTN_UP;
    case SDL_SCANCODE_RIGHT: return DM_BTN_RIGHT; case SDL_SCANCODE_DOWN: return DM_BTN_DOWN;
    case SDL_SCANCODE_SPACE: return DM_BTN_SHAKE;
    default: return 0;
    }
}
static void kbd_push(void) { dm_input_set(g_kbtn, (g_kd - g_ka) * DM_STICK_MAX, (g_kw - g_ks) * DM_STICK_MAX, 0, 0); }
static void release_all(void) {
    g_kbtn = 0, g_kw = g_ka = g_ks = g_kd = 0;
    kbd_push();
    for (int p = 0; p < DM_PLAYERS; p++) if (g_pad[p]) dm_pad_set_player(p, 0, 0, 0, 0, 0);
}

// ---------- controllers coming and going ----------
static const char *pad_name(SDL_Gamepad *g) { const char *n = SDL_GetGamepadName(g); return n && *n ? n : "controller"; }
static void npad_name(int p, char *out, size_t cap) { uint32_t id = dm_player_npad(p); if (id == 0x20) snprintf(out, cap, "handheld"); else snprintf(out, cap, "%u", id); }
// A controller was connected: given a free player (its old one when it comes back), its name logged.
static void pad_added(SDL_JoystickID id) {
    for (int p = 0; p < DM_PLAYERS; p++) if (g_pad[p] && SDL_GetGamepadID(g_pad[p]) == id) return;
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) { fprintf(stderr, "[nxl] [RED] a controller could not be opened: %s\n", SDL_GetError()); return; }
    char key[96];   // a reconnected controller gets its player back: same name and the same USB/Bluetooth ids
    snprintf(key, sizeof key, "%s %04x:%04x", pad_name(g), SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g));
    int p = pad_slot_take(&g_slots, g, key);
    if (p < 0) { fprintf(stderr, "[nxl] controller '%s' connected: all %d players are taken, not used\n", pad_name(g), DM_PLAYERS); SDL_CloseGamepad(g); return; }
    g_pad[p] = g;
    int rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    dm_pad_players(pad_slot_mask(&g_slots));
    dm_haptics_route(p, rumble);
    char nb[16];
    npad_name(p, nb, sizeof nb);
    fprintf(stderr, "[nxl] controller '%s' connected: player %d (npad %s)%s%s\n", pad_name(g), p + 1, nb, rumble ? ", rumble" : "",
            g_labels ? ", buttons by their letters" : ", buttons by position (Switch layout)");
    pad_read(p);
}
// A controller was disconnected: its player emptied.
static void pad_removed(SDL_JoystickID id) {
    for (int p = 0; p < DM_PLAYERS; p++) {
        if (!g_pad[p] || SDL_GetGamepadID(g_pad[p]) != id) continue;
        char nb[16];
        npad_name(p, nb, sizeof nb);
        fprintf(stderr, "[nxl] controller '%s' disconnected: player %d (npad %s) is free\n", pad_name(g_pad[p]), p + 1, nb);
        dm_haptics_route(p, 0);
        dm_pad_set_player(p, 0, 0, 0, 0, 0);   // nothing stays pressed
        pad_slot_free(&g_slots, g_pad[p]);
        SDL_CloseGamepad(g_pad[p]);
        g_pad[p] = NULL;
        dm_pad_players(pad_slot_mask(&g_slots));
    }
}

// Reads the input settings once (NXL_PAD_LAYOUT) and sets up the vibration callback.
void nxl_input_init(void) {
    static int done;
    if (done++) return;
    const char *e = getenv("NXL_PAD_LAYOUT");
    g_labels = e && !strcasecmp(e, "labels");
    if ((e = getenv("NXL_DEADZONE"))) { float v = strtof(e, NULL); if (v >= 0 && v < 0.9f) g_dead = v; }
    if ((e = getenv("DM_RUMBLE_GAIN"))) { float v = strtof(e, NULL); if (isfinite(v)) g_gain = fmaxf(0, fminf(3, v)); }
    e = getenv("DM_RUMBLE");
    dm_haptics_init(rumble_sink, !e || atoi(e) != 0);
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) fprintf(stderr, "[nxl] [RED] controllers: SDL could not start them (%s); the keyboard still works\n", SDL_GetError());
    // controllers already plugged in arrive as SDL_EVENT_GAMEPAD_ADDED events
}
// Handles an SDL event for the game's input: controllers added or removed, the keyboard, focus lost.
void nxl_input_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: pad_added(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e->gdevice.which); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: g_focus = 0; release_all(); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: g_focus = 1; break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (e->key.repeat || !g_focus || g_block) break;
        int down = e->type == SDL_EVENT_KEY_DOWN;
        switch (e->key.scancode) {
        case SDL_SCANCODE_W: g_kw = down; break;
        case SDL_SCANCODE_A: g_ka = down; break;
        case SDL_SCANCODE_S: g_ks = down; break;
        case SDL_SCANCODE_D: g_kd = down; break;
        default: break;
        }
        uint64_t m = key_bit(e->key.scancode);
        g_kbtn = down ? (g_kbtn | m) : (g_kbtn & ~m);
        kbd_push();
        break;
    }
    default: break;
    }
}
void nxl_input_block(int on) {
    if (on && !g_block) release_all();
    g_block = on ? 1 : g_block ? 2 : 0;
}
// Whether a button is held on any controller.
static int any_held(void) {
    for (int p = 0; p < DM_PLAYERS; p++) {
        SDL_Gamepad *g = g_pad[p];
        if (!g) continue;
        if (nxl_input_buttons(g, g_labels)) return 1;
        int32_t lx, ly;
        nxl_input_stick(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY), g_dead, &lx, &ly);
        if (lx || ly) return 1;
    }
    const bool *k = SDL_GetKeyboardState(NULL);
    return k && (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_J] || k[SDL_SCANCODE_K] || k[SDL_SCANCODE_TAB]);
}
void nxl_input_tick(void) {
    if (g_block == 2 && !any_held()) g_block = 0;
    for (int p = 0; p < DM_PLAYERS; p++) pad_read(p);
    rumble_tick();
}
