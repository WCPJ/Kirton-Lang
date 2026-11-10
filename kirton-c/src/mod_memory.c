/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* Kirton Memory System V2 (preview): explicit bounded arenas with checked
   byte buffers. No language-level raw pointers, no unsafe dangling access. */
#include "kirton/native.h"
#include "kirton/value.h"
#include "kirton/diag.h"
#include "kirton/common.h"
#include "native_internal.h"
#include <stdint.h>
#include <string.h>

static Value memory_error(const char* message){
    kir_diag_error("<память>",1,1,"%s",message);
    return v_nil();
}

static int nonnegative_size(Value arg, size_t* out){
    int64_t n=0;
    if (arg.kind!=VAL_NUM || !v_num_fits_i64(arg,&n) || n<0 ||
        (uint64_t)n>SIZE_MAX) return 0;
    *out=(size_t)n;
    return 1;
}

static Value memory_create(int argc, const Value* args){
    size_t bytes=0;
    size_t base_alignment=64;
    if ((argc!=1 && argc!=2) || !nonnegative_size(args[0],&bytes) ||
        (argc==2 && !nonnegative_size(args[1],&base_alignment)))
        return memory_error("создать(байты, [макс_выравнивание]): ожидаются неотрицательные целые");
    if (!base_alignment || (base_alignment&(base_alignment-1))!=0 || base_alignment>1024u*1024u)
        return memory_error("Максимальное выравнивание области: степень двойки, не более 1048576");
    Value arena=v_arena_new_aligned(bytes,base_alignment);
    if (arena.kind!=VAL_ARENA) return memory_error("Не удалось выделить область памяти");
    return arena;
}

static Value memory_allocate(int argc, const Value* args){
    size_t bytes=0, alignment=1;
    if ((argc!=2 && argc!=3) || args[0].kind!=VAL_ARENA ||
        !nonnegative_size(args[1],&bytes) ||
        (argc==3 && !nonnegative_size(args[2],&alignment)))
        return memory_error("выделить(область, байты, [выравнивание]): некорректные аргументы");
    if (!alignment || (alignment&(alignment-1))!=0)
        return memory_error("Выравнивание должно быть степенью двойки (1, 2, 4, 8, ...)");
    Value buffer=v_arena_allocate(args[0],bytes,alignment);
    if (buffer.kind!=VAL_BUFFER)
        return memory_error("Выделение не удалось: область закрыта, чужая, недостаточно места или выравнивание превышает возможности области");
    return buffer;
}

static Value memory_reset(int argc, const Value* args){
    if (argc!=1 || !v_arena_reset(args[0]))
        return memory_error("сбросить(область): область не существует или уже закрыта");
    return v_bool(true);
}

static Value memory_close(int argc, const Value* args){
    if (argc!=1 || !v_arena_close(args[0]))
        return memory_error("закрыть(область): область не существует или уже закрыта");
    return v_bool(true);
}

static Value memory_capacity(int argc, const Value* args){
    size_t capacity=0;
    if (argc!=1 || !v_arena_stats(args[0],&capacity,NULL,NULL))
        return memory_error("емкость(область): ожидается действительная область памяти");
    return v_int_from_size(capacity);
}

static Value memory_used(int argc, const Value* args){
    size_t used=0;
    if (argc!=1 || !v_arena_stats(args[0],NULL,&used,NULL))
        return memory_error("занято(область): ожидается действительная область памяти");
    return v_int_from_size(used);
}

static Value memory_valid(int argc, const Value* args){
    if (argc!=1 || args[0].kind!=VAL_BUFFER)
        return memory_error("действителен(буфер): ожидается буфер");
    return v_bool(v_buffer_valid(args[0])!=0);
}

static Value memory_length(int argc, const Value* args){
    size_t length=0;
    if (argc!=1 || !v_buffer_length(args[0],&length))
        return memory_error("длина(буфер): буфер недействителен");
    return v_int_from_size(length);
}

static Value memory_write(int argc, const Value* args){
    size_t index=0, byte=0;
    if (argc!=3 || !nonnegative_size(args[1],&index) ||
        !nonnegative_size(args[2],&byte) || byte>255 ||
        !v_buffer_set_u8(args[0],index,(uint8_t)byte))
        return memory_error("записать(буфер, индекс, байт): недействительный буфер, индекс или байт (0..255)");
    return v_bool(true);
}

static Value memory_read(int argc, const Value* args){
    size_t index=0;
    uint8_t byte=0;
    if (argc!=2 || !nonnegative_size(args[1],&index) ||
        !v_buffer_get_u8(args[0],index,&byte))
        return memory_error("прочитать(буфер, индекс): недействительный буфер или индекс");
    return v_int((int64_t)byte);
}

/* Typed arrays store only scalar values, never VM pointers. */
static int read_array_type(Value v, KirElementType* type){
    if (v.kind!=VAL_STR || !v.as.str.s) return 0;
    const char* name=v.as.str.s;
    size_t n=v.as.str.n;
    if (n==strlen("байт") && memcmp(name,"байт",n)==0) *type=KIR_ELEM_U8;
    else if (n==strlen("целое32") && memcmp(name,"целое32",n)==0) *type=KIR_ELEM_I32;
    else if (n==strlen("целое64") && memcmp(name,"целое64",n)==0) *type=KIR_ELEM_I64;
    else if (n==strlen("число64") && memcmp(name,"число64",n)==0) *type=KIR_ELEM_F64;
    else return 0;
    return 1;
}

static Value memory_array(int argc, const Value* args){
    size_t count=0;
    KirElementType type=0;
    if (argc!=3 || args[0].kind!=VAL_ARENA ||
        !read_array_type(args[1],&type) || !nonnegative_size(args[2],&count))
        return memory_error("массив(область, тип, количество): ожидаются тип байт/целое32/целое64/число64 и неотрицательная длина");
    Value result=v_typed_array_new(args[0],count,type);
    if (result.kind!=VAL_TYPED_ARRAY)
        return memory_error("Не удалось создать массив: область закрыта, чужая, недостаточно памяти или размер переполнен");
    return result;
}

static Value memory_array_length(int argc, const Value* args){
    size_t count=0;
    if (argc!=1 || !v_typed_array_length(args[0],&count))
        return memory_error("длина_массива(массив): массив недействителен");
    return v_int_from_size(count);
}

static Value memory_array_valid(int argc, const Value* args){
    if (argc!=1 || args[0].kind!=VAL_TYPED_ARRAY)
        return memory_error("действителен_массив(массив): ожидается типизированный массив");
    return v_bool(v_typed_array_valid(args[0])!=0);
}

static Value memory_array_type(int argc, const Value* args){
    KirElementType type=0;
    if (argc!=1 || !v_typed_array_type(args[0],&type))
        return memory_error("тип_массива(массив): массив недействителен");
    switch (type){
        case KIR_ELEM_U8: return v_str_copy("байт",-1);
        case KIR_ELEM_I32: return v_str_copy("целое32",-1);
        case KIR_ELEM_I64: return v_str_copy("целое64",-1);
        case KIR_ELEM_F64: return v_str_copy("число64",-1);
        default: return memory_error("Неизвестный тип элемента массива");
    }
}

static Value memory_array_read(int argc, const Value* args){
    size_t index=0;
    Value value=v_nil();
    if (argc!=2 || !nonnegative_size(args[1],&index) ||
        !v_typed_array_get(args[0],index,&value))
        return memory_error("взять(массив, индекс): недействительный массив или индекс");
    return value;
}

static Value memory_array_write(int argc, const Value* args){
    size_t index=0;
    if (argc!=3 || !nonnegative_size(args[1],&index) ||
        !v_typed_array_set(args[0],index,args[2]))
        return memory_error("положить(массив, индекс, число): неверное значение, индекс или срок жизни");
    return v_bool(true);
}

/* Immutable structure layouts contain only fixed-size numeric scalar fields.
   Pairs are (field_name, type_name), with natural host alignment. */
static Value memory_layout(int argc, const Value* args){
    if (argc<2 || (argc&1) || argc>512)
        return memory_error("структура(имя1, тип1, ...): нужны пары из имени и типа (не более 256 полей)");
    size_t count=(size_t)argc/2;
    KirRecordFieldDef fields[256];
    for (size_t i=0;i<count;++i){
        const Value* name=&args[2*i];
        if (name->kind!=VAL_STR || !name->as.str.s ||
            !read_array_type(args[2*i+1],&fields[i].type))
            return memory_error("Неверное имя или тип поля структуры: байт/целое32/целое64/число64");
        fields[i].name=name->as.str.s;
        fields[i].name_length=name->as.str.n;
    }
    Value layout=v_record_layout_new(fields,count);
    if (layout.kind!=VAL_RECORD_LAYOUT)
        return memory_error("Невозможно описать структуру: повтор имени, пустое имя, недопустимый размер или чужая VM");
    return layout;
}

static Value memory_record(int argc, const Value* args){
    if (argc!=2 || args[0].kind!=VAL_ARENA || args[1].kind!=VAL_RECORD_LAYOUT)
        return memory_error("запись(область, структура): ожидались область памяти и схема записи");
    Value result=v_record_new(args[0],args[1]);
    if (result.kind!=VAL_RECORD)
        return memory_error("Невозможно создать запись: недействительная область, схема, недостаточно памяти или выравнивания");
    return result;
}

static Value memory_layout_size(int argc, const Value* args){
    size_t size=0;
    if (argc!=1 || !v_record_layout_info(args[0],&size,NULL,NULL))
        return memory_error("размер_структуры(структура): требуется действительная схема");
    return v_int_from_size(size);
}

static Value memory_layout_alignment(int argc, const Value* args){
    size_t alignment=0;
    if (argc!=1 || !v_record_layout_info(args[0],NULL,&alignment,NULL))
        return memory_error("выравнивание_структуры(структура): требуется действительная схема");
    return v_int_from_size(alignment);
}

static int read_field_name(Value value, const char** name, size_t* length){
    if (value.kind!=VAL_STR || !value.as.str.s) return 0;
    *name=value.as.str.s;
    *length=value.as.str.n;
    return 1;
}

static Value memory_field_offset(int argc, const Value* args){
    const char* name=NULL; size_t length=0, offset=0;
    if (argc!=2 || !read_field_name(args[1],&name,&length) ||
        !v_record_field_info(args[0],name,length,&offset,NULL))
        return memory_error("смещение_поля(структура, имя): неизвестное поле или схема");
    return v_int_from_size(offset);
}

static Value memory_field_type(int argc, const Value* args){
    const char* name=NULL; size_t length=0;
    KirElementType type=0;
    if (argc!=2 || !read_field_name(args[1],&name,&length) ||
        !v_record_field_info(args[0],name,length,NULL,&type))
        return memory_error("тип_поля(структура, имя): неизвестное поле или схема");
    switch (type){
        case KIR_ELEM_U8: return v_str_copy("байт",-1);
        case KIR_ELEM_I32: return v_str_copy("целое32",-1);
        case KIR_ELEM_I64: return v_str_copy("целое64",-1);
        case KIR_ELEM_F64: return v_str_copy("число64",-1);
        default: return memory_error("Неизвестный тип поля структуры");
    }
}

static Value memory_record_valid(int argc, const Value* args){
    if (argc!=1 || args[0].kind!=VAL_RECORD)
        return memory_error("действительна_запись(запись): ожидается запись");
    return v_bool(v_record_valid(args[0])!=0);
}

static Value memory_field_get(int argc, const Value* args){
    const char* name=NULL; size_t length=0;
    Value result=v_nil();
    if (argc!=2 || !read_field_name(args[1],&name,&length) ||
        !v_record_get(args[0],name,length,&result))
        return memory_error("взять_поле(запись, имя): неизвестное поле или недействительная запись");
    return result;
}

static Value memory_field_set(int argc, const Value* args){
    const char* name=NULL; size_t length=0;
    if (argc!=3 || !read_field_name(args[1],&name,&length) ||
        !v_record_set(args[0],name,length,args[2]))
        return memory_error("положить_поле(запись, имя, число): неверный тип, диапазон, поле или срок жизни");
    return v_bool(true);
}

static const KirNativeDef MEMORY_FUNCS[] = {
    {"создать",memory_create},
    {"выделить",memory_allocate},
    {"сбросить",memory_reset},
    {"закрыть",memory_close},
    {"емкость",memory_capacity},
    {"занято",memory_used},
    {"действителен",memory_valid},
    {"длина",memory_length},
    {"записать",memory_write},
    {"прочитать",memory_read},
    {"массив",memory_array},
    {"длина_массива",memory_array_length},
    {"тип_массива",memory_array_type},
    {"действителен_массив",memory_array_valid},
    {"взять",memory_array_read},
    {"положить",memory_array_write},
    {"структура",memory_layout},
    {"запись",memory_record},
    {"размер_структуры",memory_layout_size},
    {"выравнивание_структуры",memory_layout_alignment},
    {"смещение_поля",memory_field_offset},
    {"тип_поля",memory_field_type},
    {"действительна_запись",memory_record_valid},
    {"взять_поле",memory_field_get},
    {"положить_поле",memory_field_set},
};

void memory_init(void){
    kir_native_register_module("память",MEMORY_FUNCS,KIR_ARRAY_LEN(MEMORY_FUNCS));
    /* These callbacks allocate through the active Value heap. The AST VM and
       Bytecode VM both scope that heap around native callback execution. */
    kir_native_mark_bytecode_module_safe("память");
}
