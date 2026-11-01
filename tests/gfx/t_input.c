// SPDX-License-Identifier: GPL-3.0-or-later
// The controller and keyboard mapping (gfx/nxl_input.c) with an SDL virtual Xbox controller: no window and no hardware.
// nn::hid's side (dm_pad_set_player, dm_input_set, the rumble router) is recorded here instead of nxnative's hle_hid.c.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "dm_input.h"
#include "hle_haptics.h"
#include "nxl_input.h"

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++, printf("ok - " __VA_ARGS__), puts(""); else fails++, printf("not ok - " __VA_ARGS__), puts(""); } while (0)

// what the game would see
static struct { uint64_t b; int32_t lx, ly, rx, ry; int calls; } pad[DM_PLAYERS], kbd;
static uint32_t players;
static int routed[DM_PLAYERS];
static void (*sink)(dm_haptic_state);
void dm_pad_set_player(int p, uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { pad[p].b = b, pad[p].lx = lx, pad[p].ly = ly, pad[p].rx = rx, pad[p].ry = ry, pad[p].calls++; }
void dm_input_set(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { kbd.b = b, kbd.lx = lx, kbd.ly = ly, kbd.rx = rx, kbd.ry = ry, kbd.calls++; }
void dm_pad_players(uint32_t m) { players = m; }
uint32_t dm_player_npad(int p) { return (uint32_t)p; }
void dm_haptics_init(void (*s)(dm_haptic_state), int on) { sink = on ? s : NULL; }
void dm_haptics_route(int p, int on) { routed[p] = on; }

// the virtual controller's rumble
static uint16_t rum_lo, rum_hi;
static int rum_calls;
static bool SDLCALL rumble(void *u, Uint16 lo, Uint16 hi) { (void)u; rum_lo = lo, rum_hi = hi, rum_calls++; return true; }

static void pump(void) {
    SDL_Event e;
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&e)) nxl_input_event(&e);
    nxl_input_tick();
}
static void key(SDL_Scancode c, int down) {
    SDL_Event e = {0};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = c, e.key.down = down;
    nxl_input_event(&e);
}

int main(void) {
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
    setenv("SDL_JOYSTICK_HIDAPI", "0", 1);   // only the virtual controller
    if (!SDL_Init(SDL_INIT_EVENTS)) { printf("not ok - SDL_Init: %s\n", SDL_GetError()); return 1; }
    nxl_input_init();
    CHECK(sink != NULL, "rumble is on by default");

    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = 0x045e, d.product_id = 0x02ea;   // an Xbox One controller
    d.naxes = SDL_GAMEPAD_AXIS_COUNT, d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "Virtual Xbox One";
    d.Rumble = rumble;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    CHECK(id != 0, "a virtual Xbox controller (%s)", id ? "attached" : SDL_GetError());
    SDL_Joystick *j = SDL_OpenJoystick(id);
    // a virtual controller's trigger axes are raw: -32768 at rest, 32767 pulled fully (a real one reads 0 to 32767 through SDL)
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    pump();
    CHECK(players == 1, "it is player 1 (players mask %#x)", players);
    CHECK(routed[0] == 1, "player 1's rumble is routed to it");

    // face buttons by position: Xbox B (right) is the Switch's A, Xbox A (bottom) is B, Y (top) is X, X (left) is Y
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, true);
    pump();
    CHECK(pad[0].b == DM_BTN_A, "Xbox B (right) = A (%#llx)", (unsigned long long)pad[0].b);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump();
    CHECK(pad[0].b == DM_BTN_B, "Xbox A (bottom) = B");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_NORTH, true);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_WEST, true);
    pump();
    CHECK(pad[0].b == (DM_BTN_X | DM_BTN_Y), "Xbox Y and X = X and Y");
    SDL_GamepadButton rest[] = {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
                                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    uint64_t want[] = {DM_BTN_L, DM_BTN_R, DM_BTN_PLUS, DM_BTN_MINUS, DM_BTN_LS, DM_BTN_RS, DM_BTN_UP, DM_BTN_DOWN, DM_BTN_LEFT, DM_BTN_RIGHT};
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_NORTH, false);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_WEST, false);
    int all = 1;
    for (int i = 0; i < 10; i++) {
        SDL_SetJoystickVirtualButton(j, rest[i], true);
        pump();
        if (pad[0].b != want[i]) all = 0, printf("# button %d gave %#llx\n", rest[i], (unsigned long long)pad[0].b);
        SDL_SetJoystickVirtualButton(j, rest[i], false);
    }
    CHECK(all, "bumpers, Start, Back, stick clicks and the d-pad");
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -24000);   // about 4400 of 32767
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    pump();
    CHECK(pad[0].b == DM_BTN_ZR, "a full right trigger is ZR, a light left one is not ZL");
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);        // half
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    pump();
    CHECK(pad[0].b == DM_BTN_ZL, "the left trigger past a quarter is ZL");
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);

    // sticks: up on the stick is positive Y for the game; a small drift is ignored; full travel stays full
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
    pump();
    CHECK(pad[0].ly == DM_STICK_MAX && pad[0].lx == 0, "left stick up = +%d (%d, %d)", DM_STICK_MAX, pad[0].lx, pad[0].ly);
    CHECK(pad[0].rx == DM_STICK_MAX && pad[0].ry == 0, "right stick right = +%d", DM_STICK_MAX);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 2500);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTX, -3000);
    pump();
    CHECK(pad[0].ly == 0 && pad[0].rx == 0, "drift inside the dead zone reads as centred");
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, 16384);
    pump();
    CHECK(pad[0].lx > 0 && pad[0].lx < 16384, "half travel right reads as a bit less than half (%d)", pad[0].lx);
    int32_t ox, oy;
    nxl_input_stick(23170, 23170, 0.12f, &ox, &oy);
    CHECK(ox > 23000 && -oy > 23000 && ox <= DM_STICK_MAX, "a diagonal keeps its length (%d, %d)", ox, oy);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 0);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    pump();

    // buttons by their letters
    SDL_Gamepad *g = SDL_GetGamepadFromID(id);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump();
    CHECK(nxl_input_buttons(g, 1) == DM_BTN_A, "NXL_PAD_LAYOUT=labels: Xbox A = A");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, false);
    pump();

    // the keyboard: WASD stick, J = A, Return = +, Space = shake; held keys are released when the window loses focus
    key(SDL_SCANCODE_D, 1); key(SDL_SCANCODE_W, 1); key(SDL_SCANCODE_J, 1); key(SDL_SCANCODE_RETURN, 1);
    CHECK(kbd.b == (DM_BTN_A | DM_BTN_PLUS) && kbd.lx == DM_STICK_MAX && kbd.ly == DM_STICK_MAX, "keyboard: W D J Return = up-right, A, +");
    key(SDL_SCANCODE_J, 0); key(SDL_SCANCODE_SPACE, 1);
    CHECK(kbd.b == (DM_BTN_PLUS | DM_BTN_SHAKE), "keyboard: J released, Space shakes");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, true);
    pump();
    SDL_Event fe = {0};
    fe.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    nxl_input_event(&fe);
    CHECK(kbd.b == 0 && kbd.lx == 0 && kbd.ly == 0, "focus lost: the keyboard is released");
    pump();
    CHECK(pad[0].b == 0, "focus lost: a held controller button reads as released");
    fe.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    nxl_input_event(&fe);
    pump();
    CHECK(pad[0].b == DM_BTN_A, "focus back: the controller is read as it is now");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, false);
    pump();

    // rumble: the game's low and high bands on the two motors
    dm_haptic_state s = {0};
    s.player = 0;
    s.motor[0].amplitude_low = 0.5f, s.motor[1].amplitude_high = 0.25f;
    sink(s);
    pump();
    CHECK(rum_calls >= 1 && abs(rum_lo - 32767) < 2 && abs(rum_hi - 16383) < 2, "rumble: low 0.5 and high 0.25 (%u, %u)", rum_lo, rum_hi);
    memset(&s.motor, 0, sizeof s.motor);
    sink(s);
    pump();
    CHECK(rum_lo == 0 && rum_hi == 0, "rumble stops");

    // unplugged: player 1 is free and nothing stays held
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, true);
    pump();
    SDL_CloseJoystick(j);
    SDL_DetachVirtualJoystick(id);
    pump();
    CHECK(players == 0 && pad[0].b == 0 && routed[0] == 0, "unplugged: player 1 is free, released, and rumble unrouted");

    // plugged in again: the same player
    id = SDL_AttachVirtualJoystick(&d);
    pump();
    CHECK(players == 1, "plugged in again: player 1 again");

    SDL_Quit();
    printf("nxl-tests: %d passed, %d failed\n", passes, fails);
    if (!fails) puts("nxl-tests: passed");
    return fails != 0;
}
