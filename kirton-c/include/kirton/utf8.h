/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

int  kir_utf8_decode(const char* s, const char* end, uint32_t* out_cp);
bool kir_is_letter(uint32_t cp);
bool kir_is_digit(uint32_t cp);
bool kir_is_ident_start(uint32_t cp);
bool kir_is_ident_cont(uint32_t cp);
