/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int run_script(const char *path_utf8) {
    VM *vm = vm_new();
    if (!vm) return 70;

    KirEnv *root = vm_run_root(vm, path_utf8);
    int destroyed = vm_try_free(vm);
    return root != 0 && destroyed ? 0 : 1;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 64;
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[1], -1,
                                   NULL, 0, NULL, NULL);
    if (count <= 0) return 65;
    char *path = (char *)malloc((size_t)count);
    if (!path) return 70;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[1], -1,
                             path, count, NULL, NULL)) {
        free(path);
        return 65;
    }
    int result = run_script(path);
    free(path);
    return result;
}
#else
int main(int argc, char **argv) {
    if (argc != 2) return 64;
    return run_script(argv[1]);
}
#endif
