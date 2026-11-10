/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
/* Kirton FFI V2.6 — stable, deliberately narrow C plugin calling convention.
   Native plugins execute in-process, are trusted code, and MAY crash the host.
   This is a uniform C ABI, NOT a general-purpose dynamic C signature caller.
   All pointers are borrowed, synchronous views. Never retain them past invoke.
   No Kirton VM/Value pointers are exposed to plugins. */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KIR_FFI_ABI_V1 UINT32_C(1)
#define KIR_FFI_MAX_ARGS 16u

typedef enum {
    KIR_FFI_VOID = 0,
    KIR_FFI_I64 = 1,
    KIR_FFI_F64 = 2,
    KIR_FFI_BOOL = 3,
    KIR_FFI_TEXT = 4
} KirFfiType;

typedef struct {
    const char* bytes;
    size_t length; /* raw UTF-8 byte length; embedded NUL is supported */
} KirFfiText;

typedef struct {
    uint32_t type;   /* KirFfiType */
    uint32_t reserved;
    union {
        int64_t i64;
        double f64;
        int32_t boolean; /* always 0 or 1 */
        KirFfiText text;
    } data;
} KirFfiValue;

/* Return 1 for success. 0 indicates failure with no return value.
   If returning TEXT, data.text.bytes must remain valid until invoke returns.
   Host immediately copies it to GC-owned storage. */
typedef int (*KirFfiInvoke)(uint32_t argc, const KirFfiValue* argv,
                            KirFfiValue* result);

typedef struct {
    const char* name;        /* unique name within module, UTF-8 */
    uint32_t argc;          /* <= KIR_FFI_MAX_ARGS */
    const uint32_t* args;   /* argc KirFfiType tags */
    uint32_t returns;       /* KirFfiType tag, VOID allowed */
    KirFfiInvoke invoke;
} KirFfiFunctionV1;

typedef struct {
    uint32_t abi_version;     /* KIR_FFI_ABI_V1 */
    uint32_t struct_size;     /* sizeof(KirFfiModuleV1) */
    const char* module_name;  /* unique within VM */
    uint32_t function_count;
    const KirFfiFunctionV1* functions;
} KirFfiModuleV1;

/* Every plugin must export this exact symbol with C linkage.
   Metadata must remain valid for the entire time the library is loaded. */
typedef const KirFfiModuleV1* (*KirFfiModuleEntryV1)(void);

#ifdef __cplusplus
}
#endif

/* -------------------------------------------------------------------------
   Experimental plugin ABI V2 (Kirton Memory System V2.7).
   V1 remains supported unchanged. V2 is an opt-in, separate entrypoint.
   All V2 pointers and resource tokens are valid only during invoke().
   Plugins are trusted native code, not sandboxed.
   ------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif
#define KIR_FFI_ABI_V2 UINT32_C(2)

typedef enum {
    KIR_FFI_V2_VOID = 0,
    KIR_FFI_V2_I64 = 1,
    KIR_FFI_V2_F64 = 2,
    KIR_FFI_V2_BOOL = 3,
    KIR_FFI_V2_TEXT = 4,
    KIR_FFI_V2_RECORD = 5,
    KIR_FFI_V2_RESOURCE = 6
} KirFfiTypeV2;

typedef enum {
    KIR_FFI_FIELD_U8 = 1,
    KIR_FFI_FIELD_I32 = 2,
    KIR_FFI_FIELD_I64 = 3,
    KIR_FFI_FIELD_F64 = 4
} KirFfiFieldTypeV2;

typedef struct {
    const char* name;
    uint32_t type; /* KirFfiFieldTypeV2 */
    size_t offset;
} KirFfiFieldV2;

typedef struct {
    size_t size;
    size_t alignment;
    uint32_t field_count;
    const KirFfiFieldV2* fields;
} KirFfiRecordSchemaV2;

typedef struct {
    uint32_t type; /* KirFfiTypeV2 */
    /* Required only for RECORD; exact field names, types and offsets checked. */
    const KirFfiRecordSchemaV2* record;
    /* Required only for RESOURCE; UTF-8 C string of expected native type. */
    const char* resource_type;
} KirFfiParamV2;

typedef struct {
    const unsigned char* bytes;
    size_t length;
    /* Bytes are an isolated snapshot for arguments, NEVER arena pointers.
       Returned bytes must survive until the callback has returned. */
} KirFfiRecordV2;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    union {
        int64_t i64;
        double f64;
        int32_t boolean;
        KirFfiText text;
        KirFfiRecordV2 record;
        uint64_t resource_token;
    } data;
} KirFfiValueV2;

/* The host provides a synchronous, scoped visitor for RESOURCE arguments.
   The raw handle can ONLY be used during visitor(). The host protects the
   Kirton resource from GC/close while visiting. Never retain handle/context. */
typedef int (*KirFfiResourceVisitorV2)(void* handle, void* user_data);
typedef struct KirFfiHostV2 KirFfiHostV2;
struct KirFfiHostV2 {
    uint32_t struct_size;
    void* context; /* opaque, only valid during invoke */
    int (*visit_resource)(const KirFfiHostV2* host, uint64_t token,
                          const char* type, KirFfiResourceVisitorV2 visitor,
                          void* user_data);
};

typedef int (*KirFfiInvokeV2)(const KirFfiHostV2* host, uint32_t argc,
                              const KirFfiValueV2* args, KirFfiValueV2* result);
typedef struct {
    const char* name;
    uint32_t argc;
    const KirFfiParamV2* args;
    KirFfiParamV2 returns;
    KirFfiInvokeV2 invoke;
} KirFfiFunctionV2;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char* module_name;
    uint32_t function_count;
    const KirFfiFunctionV2* functions;
} KirFfiModuleV2;

typedef const KirFfiModuleV2* (*KirFfiModuleEntryV2)(void);
/* Native module exports kirton_ffi_module_v2 (C linkage) instead of V1.
   V2 results may be primitive, a copied RECORD, or an existing RESOURCE
   token from an input. New resource adoption is intentionally unsupported. */
#ifdef __cplusplus
}
#endif
