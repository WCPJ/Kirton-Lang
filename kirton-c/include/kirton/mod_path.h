/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
/* kirton/mod_path.h — нативные функции работы с путями (путь_соединить, имя, папка, расширение, нормализовать) */

#ifdef __cplusplus
extern "C" {
#endif

/* Регистрация набора нативных функций модуля путей в рантайме Kirton.
   Вызывай один раз при инициализации нативов (например, в native_init()). */
void path_init(void);

#ifdef __cplusplus
}
#endif
