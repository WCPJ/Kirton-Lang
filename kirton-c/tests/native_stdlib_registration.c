/* SPDX-License-Identifier: MPL-2.0
 * Verify compile-time stdlib names are present in the independently
 * registered runtime table; prevents silent ID/table drift across builds.
 */
#include "kirton/native.h"
#include "native_internal.h"
#include "native_stdlib_whitelist.h"
#include <stdio.h>
#include <string.h>

void str_init(void);
void list_init(void);
void dict_init(void);
void math_init(void);

int main(void){
    str_init(); list_init(); dict_init(); math_init();
    for (size_t i=0;i<KIRTON_NATIVE_STDLIB_COUNT;++i){
        const char* name=kirton_native_stdlib_names[i];
        if (!kir_native_has_function(name,strlen(name))){
            fprintf(stderr,"missing native stdlib implementation: %s\n",name);
            return 1;
        }
        for (size_t j=0;j<i;++j)
            if (!strcmp(name,kirton_native_stdlib_names[j])){
                fprintf(stderr,"duplicate stdlib dispatch entry: %s\n",name);
                return 2;
            }
    }
    printf("checked %zu native stdlib dispatch entries\n",
           (size_t)KIRTON_NATIVE_STDLIB_COUNT);
    return 0;
}
