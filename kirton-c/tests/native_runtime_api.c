#include "native_runtime.h"

#include <stdio.h>
#include <string.h>

int main(int argc,char** argv){
    KirtonNativeRuntime* runtime=kirton_native_runtime_new();
    if (!runtime) return 1;
    if (argc==2 && strcmp(argv[1],"abort")==0){
        KirtonNativeFrame* frame=kirton_native_frame_enter(runtime);
        if (!frame) return 8;
        if (!kirton_native_string(runtime,"Ошибка",strlen("Ошибка"))) return 9;
        kirton_native_runtime_abort(runtime,71);
        return 71;
    }

    KirtonNativeValue* live=kirton_native_string(runtime,"Живая строка",strlen("Живая строка"));
    if (!live) return 2;
    KirtonNativeValue* discarded=kirton_native_string(runtime,"temporary",9);
    if (!discarded) return 3;
    const KirtonNativeValue* roots[]={live};
    kirton_native_safepoint(runtime,roots,1);

    KirtonNativeFrame* frame=kirton_native_frame_enter(runtime);
    if (!frame) return 4;
    KirtonNativeValue* returned=kirton_native_string(
        runtime,"Возвращённая строка",strlen("Возвращённая строка"));
    if (!returned) return 5;
    KirtonNativeValue* temporary=kirton_native_string(runtime,"discard me",10);
    if (!temporary) return 6;
    const KirtonNativeValue* child_roots[]={returned};
    kirton_native_safepoint(runtime,child_roots,1);
    KirtonNativeValue* transferred=kirton_native_frame_return(runtime,frame,returned);
    if (!transferred) return 7;

    const KirtonNativeValue* caller_roots[]={live,transferred};
    kirton_native_safepoint(runtime,caller_roots,2);
    kirton_native_print_value(runtime,live);
    kirton_native_print_space(runtime);
    kirton_native_print_value(runtime,transferred);
    kirton_native_print_end(runtime);
    return kirton_native_runtime_destroy(runtime);
}
