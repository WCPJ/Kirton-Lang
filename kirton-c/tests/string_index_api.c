/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include "kirton/diag.h"
#include "kirton/common.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(int argc,char** argv){
    CHECK(argc==2);
    VM* vm=vm_new();
    CHECK(vm!=NULL);
    CHECK(vm_run_root(vm,argv[1])!=NULL);

    static const char overlong[]={'A',(char)0xc0,(char)0xaf};
    static const char invalid_continuation[]={'A',(char)0x80};
    static const char truncated[]={'A',(char)0xe2,(char)0x82};
    const char* malformed[]={overlong,invalid_continuation,truncated};
    const size_t lengths[]={sizeof(overlong),sizeof(invalid_continuation),sizeof(truncated)};
    for (size_t i=0;i<sizeof(malformed)/sizeof(malformed[0]);++i){
        Value argument=v_str_copy(malformed[i],(int)lengths[i]);
        Value result=v_nil();
        unsigned int before=kir_diag_error_count();
        int call_ok=vm_call_by_name(vm,"второй_символ",1,&argument,&result);
        unsigned int after=kir_diag_error_count();
        if (call_ok || after<=before){
            fprintf(stderr,"malformed UTF-8 case %zu: call=%d errors=%u->%u\n",
                    i,call_ok,before,after);
            return __LINE__;
        }
        kir_free(argument.as.str.s);
    }

    Value valid=v_str_copy("А🙂",(int)strlen("А🙂")),first=v_nil();
    CHECK(vm_call_by_name(vm,"второй_символ",1,&valid,&first));
    CHECK(first.kind==VAL_STR && first.as.str.n==strlen("🙂") &&
          memcmp(first.as.str.s,"🙂",first.as.str.n)==0);
    kir_free(valid.as.str.s);

    CHECK(vm_try_free(vm));
    return 0;
}
