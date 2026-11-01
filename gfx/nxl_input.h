// SPDX-License-Identifier: GPL-3.0-or-later
// Controllers and keyboard (SDL3) for nxnative's nn::hid layer; see nxl_input.c.
#pragma once
#include <stdint.h>
#include <SDL3/SDL.h>

void nxl_input_init(void);                    // after SDL_Init, on the thread that pumps the window's events
void nxl_input_event(const SDL_Event *e);     // each event the pump sees
void nxl_input_tick(void);                    // once per pump: controller state and rumble
void nxl_input_block(int on);                // 1: the game sees nothing pressed (NativeNX's menu is open over it)
// the mapping, for tests
int32_t nxl_input_axis(int16_t v);
void nxl_input_stick(int16_t x, int16_t y, float dead, int32_t *ox, int32_t *oy);
uint64_t nxl_input_buttons(SDL_Gamepad *g, int labels);
