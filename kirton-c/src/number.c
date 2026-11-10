/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/number.h"
#include "kirton/common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define KIR_BIGINT_BASE 1000000000u

struct KirBigInt {
    int sign;
    size_t len;
    size_t cap;
    uint32_t* limbs; /* little-endian, base 10^9 */
    KirBigIntHeap* heap;
    struct KirBigInt* heap_prev;
    struct KirBigInt* heap_next;
    bool marked;
};

struct KirBigIntHeap {
    KirBigInt* head;
    size_t count;
};

KirBigIntHeap* kir_bigint_heap_new(void){
    KirBigIntHeap* heap = (KirBigIntHeap*)kir_malloc(sizeof(*heap));
    heap->head = NULL;
    heap->count = 0;
    return heap;
}

int kir_bigint_heap_manage(KirBigIntHeap* heap, KirBigInt* value){
    if (!heap || !value) return 0;
    if (value->heap) return value->heap == heap;
    value->heap = heap;
    value->heap_prev = NULL;
    value->heap_next = heap->head;
    value->marked = false;
    if (heap->head) heap->head->heap_prev = value;
    heap->head = value;
    ++heap->count;
    return 1;
}

int kir_bigint_heap_accepts_value(const KirBigIntHeap* heap,const KirBigInt* value){
    return value && (!value->heap || value->heap==heap);
}

int kir_bigint_heap_mark(KirBigIntHeap* heap, KirBigInt* value){
    if (!heap || !value || value->heap != heap) return 0;
    value->marked = true;
    return 1;
}

size_t kir_bigint_heap_count(const KirBigIntHeap* heap){
    return heap ? heap->count : 0;
}

size_t kir_bigint_heap_release_all(KirBigIntHeap* heap){
    if (!heap) return 0;
    size_t released=heap->count;
    KirBigInt* value=heap->head;
    while (value){
        KirBigInt* next=value->heap_next;
        value->heap=NULL;
        value->heap_prev=NULL;
        value->heap_next=NULL;
        value->marked=false;
        value=next;
    }
    heap->head=NULL;
    heap->count=0;
    return released;
}

static KirBigInt* bigint_new(size_t cap){
    KirBigInt* value = (KirBigInt*)kir_malloc(sizeof(KirBigInt));
    value->sign = 0;
    value->len = 0;
    value->cap = cap ? cap : 1;
    value->heap = NULL;
    value->heap_prev = NULL;
    value->heap_next = NULL;
    value->marked = false;
    value->limbs = (uint32_t*)kir_xmallocn(value->cap, sizeof(uint32_t));
    memset(value->limbs, 0, value->cap * sizeof(uint32_t));
    return value;
}

void kir_bigint_free(KirBigInt* value){
    if (!value) return;
    if (value->heap){
        KirBigIntHeap* heap = value->heap;
        if (value->heap_prev) value->heap_prev->heap_next = value->heap_next;
        else heap->head = value->heap_next;
        if (value->heap_next) value->heap_next->heap_prev = value->heap_prev;
        --heap->count;
    }
    kir_free(value->limbs);
    kir_free(value);
}

size_t kir_bigint_heap_sweep(KirBigIntHeap* heap){
    if (!heap) return 0;
    size_t freed=0;
    KirBigInt* value=heap->head;
    while (value){
        KirBigInt* next=value->heap_next;
        if (value->marked) value->marked=false;
        else { kir_bigint_free(value); ++freed; }
        value=next;
    }
    return freed;
}

void kir_bigint_heap_destroy(KirBigIntHeap* heap){
    if (!heap) return;
    while (heap->head) kir_bigint_free(heap->head);
    kir_free(heap);
}

static void bigint_reserve(KirBigInt* value, size_t need){
    if (need <= value->cap) return;
    size_t cap = value->cap;
    while (cap < need) cap *= 2;
    value->limbs = (uint32_t*)kir_xreallocn(value->limbs, cap, sizeof(uint32_t));
    memset(value->limbs + value->cap, 0, (cap - value->cap) * sizeof(uint32_t));
    value->cap = cap;
}

static void bigint_normalize(KirBigInt* value){
    while (value->len && value->limbs[value->len - 1] == 0) --value->len;
    if (!value->len) value->sign = 0;
}

static KirBigInt* bigint_copy(const KirBigInt* source){
    KirBigInt* copy = bigint_new(source->len);
    copy->sign = source->sign;
    copy->len = source->len;
    if (source->len) memcpy(copy->limbs, source->limbs, source->len * sizeof(uint32_t));
    return copy;
}

static void bigint_mul_small_in_place(KirBigInt* value, uint32_t factor){
    if (!value->len || factor == 0){
        value->len = 0;
        value->sign = 0;
        return;
    }
    uint64_t carry = 0;
    for (size_t i = 0; i < value->len; ++i){
        uint64_t product = (uint64_t)value->limbs[i] * factor + carry;
        value->limbs[i] = (uint32_t)(product % KIR_BIGINT_BASE);
        carry = product / KIR_BIGINT_BASE;
    }
    if (carry){
        bigint_reserve(value, value->len + 1);
        value->limbs[value->len++] = (uint32_t)carry;
    }
}

static void bigint_add_small_in_place(KirBigInt* value, uint32_t addend){
    uint64_t carry = addend;
    size_t i = 0;
    while (carry && i < value->len){
        uint64_t sum = (uint64_t)value->limbs[i] + carry;
        value->limbs[i] = (uint32_t)(sum % KIR_BIGINT_BASE);
        carry = sum / KIR_BIGINT_BASE;
        ++i;
    }
    if (carry){
        bigint_reserve(value, value->len + 1);
        value->limbs[value->len++] = (uint32_t)carry;
    } else if (!value->len && addend){
        bigint_reserve(value, 1);
        value->limbs[0] = addend;
        value->len = 1;
    }
    if (value->len && !value->sign) value->sign = 1;
}

static int bigint_compare_magnitude(const KirBigInt* a, const KirBigInt* b){
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    for (size_t i = a->len; i > 0; --i){
        if (a->limbs[i - 1] < b->limbs[i - 1]) return -1;
        if (a->limbs[i - 1] > b->limbs[i - 1]) return 1;
    }
    return 0;
}

static KirBigInt* bigint_add_magnitude(const KirBigInt* a, const KirBigInt* b, int sign){
    size_t len = a->len > b->len ? a->len : b->len;
    KirBigInt* result = bigint_new(len + 1);
    uint64_t carry = 0;
    for (size_t i = 0; i < len; ++i){
        uint64_t sum = carry;
        if (i < a->len) sum += a->limbs[i];
        if (i < b->len) sum += b->limbs[i];
        result->limbs[i] = (uint32_t)(sum % KIR_BIGINT_BASE);
        carry = sum / KIR_BIGINT_BASE;
    }
    result->len = len;
    if (carry) result->limbs[result->len++] = (uint32_t)carry;
    result->sign = sign;
    bigint_normalize(result);
    return result;
}

static KirBigInt* bigint_sub_magnitude(const KirBigInt* larger, const KirBigInt* smaller, int sign){
    KirBigInt* result = bigint_new(larger->len);
    int64_t borrow = 0;
    for (size_t i = 0; i < larger->len; ++i){
        int64_t digit = (int64_t)larger->limbs[i] - borrow;
        if (i < smaller->len) digit -= smaller->limbs[i];
        if (digit < 0){
            digit += KIR_BIGINT_BASE;
            borrow = 1;
        } else {
            borrow = 0;
        }
        result->limbs[i] = (uint32_t)digit;
    }
    result->len = larger->len;
    result->sign = sign;
    bigint_normalize(result);
    return result;
}

KirBigInt* kir_bigint_parse(const char* text, size_t len, unsigned int base){
    if (!text || (base != 10 && base != 16)) return NULL;
    size_t i = 0;
    int sign = 1;
    if (i < len && (text[i] == '-' || text[i] == '+')){
        if (text[i] == '-') sign = -1;
        ++i;
    }
    if (base == 16 && i + 1 < len && text[i] == '0' &&
        (text[i + 1] == 'x' || text[i + 1] == 'X')) i += 2;

    KirBigInt* value = bigint_new(1);
    int have_digit = 0;
    for (; i < len; ++i){
        unsigned char c = (unsigned char)text[i];
        unsigned int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = 10 + c - 'a';
        else if (base == 16 && c >= 'A' && c <= 'F') digit = 10 + c - 'A';
        else { kir_bigint_free(value); return NULL; }
        if (digit >= base){ kir_bigint_free(value); return NULL; }
        bigint_mul_small_in_place(value, base);
        bigint_add_small_in_place(value, digit);
        have_digit = 1;
    }
    if (!have_digit){ kir_bigint_free(value); return NULL; }
    if (value->len) value->sign = sign;
    return value;
}

KirBigInt* kir_bigint_from_i64(int64_t integer){
    KirBigInt* value = bigint_new(3);
    uint64_t magnitude;
    if (integer < 0){
        value->sign = -1;
        magnitude = (uint64_t)(-(integer + 1)) + 1;
    } else {
        value->sign = integer ? 1 : 0;
        magnitude = (uint64_t)integer;
    }
    while (magnitude){
        value->limbs[value->len++] = (uint32_t)(magnitude % KIR_BIGINT_BASE);
        magnitude /= KIR_BIGINT_BASE;
    }
    return value;
}

KirBigInt* kir_bigint_add(const KirBigInt* a, const KirBigInt* b){
    if (!a->sign) return bigint_copy(b);
    if (!b->sign) return bigint_copy(a);
    if (a->sign == b->sign) return bigint_add_magnitude(a, b, a->sign);
    int cmp = bigint_compare_magnitude(a, b);
    if (cmp == 0) return kir_bigint_from_i64(0);
    return cmp > 0 ? bigint_sub_magnitude(a, b, a->sign)
                   : bigint_sub_magnitude(b, a, b->sign);
}

KirBigInt* kir_bigint_sub(const KirBigInt* a, const KirBigInt* b){
    KirBigInt negative = *b;
    negative.sign = -negative.sign;
    return kir_bigint_add(a, &negative);
}

KirBigInt* kir_bigint_mul(const KirBigInt* a, const KirBigInt* b){
    if (!a->sign || !b->sign) return kir_bigint_from_i64(0);
    KirBigInt* result = bigint_new(a->len + b->len);
    result->len = a->len + b->len;
    result->sign = a->sign * b->sign;
    for (size_t i = 0; i < a->len; ++i){
        uint64_t carry = 0;
        for (size_t j = 0; j < b->len; ++j){
            size_t at = i + j;
            uint64_t product = (uint64_t)a->limbs[i] * b->limbs[j] + result->limbs[at] + carry;
            result->limbs[at] = (uint32_t)(product % KIR_BIGINT_BASE);
            carry = product / KIR_BIGINT_BASE;
        }
        size_t at = i + b->len;
        while (carry){
            uint64_t sum = result->limbs[at] + carry;
            result->limbs[at] = (uint32_t)(sum % KIR_BIGINT_BASE);
            carry = sum / KIR_BIGINT_BASE;
            ++at;
        }
    }
    bigint_normalize(result);
    return result;
}

static uint64_t bigint_div_small_in_place(KirBigInt* value, uint64_t divisor){
    uint64_t remainder = 0;
    for (size_t i=value->len; i>0; --i){
        uint64_t current = remainder*KIR_BIGINT_BASE + value->limbs[i-1];
        value->limbs[i-1] = (uint32_t)(current/divisor);
        remainder = current%divisor;
    }
    bigint_normalize(value);
    return remainder;
}

static void bigint_mul_2_32_add(KirBigInt* value, uint32_t word){
    uint64_t carry = word;
    for (size_t i=0; i<value->len; ++i){
        uint64_t product = (uint64_t)value->limbs[i]*UINT64_C(4294967296) + carry;
        value->limbs[i] = (uint32_t)(product%KIR_BIGINT_BASE);
        carry = product/KIR_BIGINT_BASE;
    }
    while (carry){
        bigint_reserve(value, value->len+1);
        value->limbs[value->len++] = (uint32_t)(carry%KIR_BIGINT_BASE);
        carry /= KIR_BIGINT_BASE;
    }
    if (value->len && !value->sign) value->sign = 1;
}

static KirBigInt* bigint_mul_small_copy(const KirBigInt* value, uint32_t factor){
    KirBigInt* result = bigint_copy(value);
    if (result->len) result->sign = 1;
    bigint_mul_small_in_place(result, factor);
    return result;
}

static void bigint_divmod_magnitude(const KirBigInt* dividend, const KirBigInt* divisor,
                                    KirBigInt** quotient, KirBigInt** remainder){
    KirBigInt* q = bigint_new(dividend->len);
    q->len = dividend->len;
    q->sign = dividend->len ? 1 : 0;
    KirBigInt* r = bigint_new(1);

    for (size_t i=dividend->len; i>0; --i){
        if (r->len){
            bigint_reserve(r, r->len+1);
            memmove(r->limbs+1, r->limbs, r->len*sizeof(uint32_t));
            ++r->len;
        } else {
            r->len = 1;
        }
        r->limbs[0] = dividend->limbs[i-1];
        r->sign = r->len ? 1 : 0;
        bigint_normalize(r);

        uint32_t low=0, high=KIR_BIGINT_BASE-1;
        while (low<high){
            uint32_t middle = low + (high-low+1)/2;
            KirBigInt* product = bigint_mul_small_copy(divisor, middle);
            int cmp = bigint_compare_magnitude(product, r);
            kir_bigint_free(product);
            if (cmp<=0) low=middle;
            else high=middle-1;
        }
        q->limbs[i-1] = low;
        if (low){
            KirBigInt* product = bigint_mul_small_copy(divisor, low);
            KirBigInt* next_r = bigint_sub_magnitude(r, product, 1);
            kir_bigint_free(product);
            kir_bigint_free(r);
            r = next_r;
        }
    }
    bigint_normalize(q);
    bigint_normalize(r);
    *quotient=q;
    *remainder=r;
}

int kir_bigint_divmod_floor(const KirBigInt* a, const KirBigInt* b,
                            KirBigInt** quotient, KirBigInt** remainder){
    if (!a || !b || !quotient || !remainder || !b->sign) return 0;
    KirBigInt positive_a = *a, positive_b = *b;
    if (positive_a.len) positive_a.sign = 1;
    if (positive_b.len) positive_b.sign = 1;
    KirBigInt* q=NULL;
    KirBigInt* r=NULL;
    bigint_divmod_magnitude(&positive_a, &positive_b, &q, &r);

    if (q->len) q->sign = a->sign*b->sign;
    if (r->len) r->sign = a->sign;
    if (r->len && a->sign != b->sign){
        KirBigInt* one = kir_bigint_from_i64(1);
        KirBigInt* next_q = bigint_add_magnitude(q, one, 1);
        KirBigInt* next_r = bigint_sub_magnitude(&positive_b, r, 1);
        kir_bigint_free(one);
        kir_bigint_free(q);
        kir_bigint_free(r);
        q=next_q;
        r=next_r;
        q->sign=-1;
        r->sign=b->sign;
    }
    bigint_normalize(q);
    bigint_normalize(r);
    *quotient=q;
    *remainder=r;
    return 1;
}

static size_t bigint_twos_word_count(const KirBigInt* value){
    if (value->len > (SIZE_MAX-64)/30) return 0;
    return (value->len*30)/32 + 2;
}

static uint32_t* bigint_to_twos_words(const KirBigInt* value, size_t count){
    uint32_t* words = (uint32_t*)kir_xmallocn(count, sizeof(uint32_t));
    memset(words, 0, count*sizeof(uint32_t));
    KirBigInt* magnitude = bigint_copy(value);
    if (magnitude->len) magnitude->sign=1;
    for (size_t i=0; i<count && magnitude->len; ++i)
        words[i] = (uint32_t)bigint_div_small_in_place(magnitude, UINT64_C(4294967296));
    kir_bigint_free(magnitude);

    if (value->sign<0){
        uint64_t carry=1;
        for (size_t i=0; i<count; ++i){
            uint64_t digit=(uint64_t)(~words[i])+carry;
            words[i]=(uint32_t)digit;
            carry=digit>>32;
        }
    }
    return words;
}

static KirBigInt* bigint_from_twos_words(uint32_t* words, size_t count){
    if (!count) return kir_bigint_from_i64(0);
    int negative = (words[count-1] & UINT32_C(0x80000000)) != 0;
    if (negative){
        uint64_t carry=1;
        for (size_t i=0; i<count; ++i){
            uint64_t digit=(uint64_t)(~words[i])+carry;
            words[i]=(uint32_t)digit;
            carry=digit>>32;
        }
    }
    KirBigInt* result=bigint_new(1);
    for (size_t i=count; i>0; --i)
        bigint_mul_2_32_add(result, words[i-1]);
    if (result->len) result->sign=negative ? -1 : 1;
    return result;
}

KirBigInt* kir_bigint_bitwise(const KirBigInt* a, const KirBigInt* b, char op){
    size_t a_count=bigint_twos_word_count(a), b_count=bigint_twos_word_count(b);
    size_t count=a_count>b_count ? a_count : b_count;
    if (!count) return NULL;
    uint32_t* aw=bigint_to_twos_words(a,count);
    uint32_t* bw=bigint_to_twos_words(b,count);
    for (size_t i=0; i<count; ++i){
        if (op=='&') aw[i] &= bw[i];
        else if (op=='|') aw[i] |= bw[i];
        else if (op=='^') aw[i] ^= bw[i];
        else { kir_free(aw); kir_free(bw); return NULL; }
    }
    kir_free(bw);
    KirBigInt* result=bigint_from_twos_words(aw,count);
    kir_free(aw);
    return result;
}

KirBigInt* kir_bigint_bitnot(const KirBigInt* value){
    size_t count=bigint_twos_word_count(value);
    if (!count) return NULL;
    uint32_t* words=bigint_to_twos_words(value,count);
    for (size_t i=0; i<count; ++i) words[i]=~words[i];
    KirBigInt* result=bigint_from_twos_words(words,count);
    kir_free(words);
    return result;
}

KirBigInt* kir_bigint_shift_left(const KirBigInt* value, size_t bits){
    if (!value->sign) return kir_bigint_from_i64(0);
    size_t count=bigint_twos_word_count(value);
    size_t word_shift=bits/32;
    unsigned int bit_shift=(unsigned int)(bits%32);
    if (!count || count==SIZE_MAX || word_shift > SIZE_MAX-count-1) return NULL;
    size_t result_count=count+word_shift+1;
    uint32_t* source=bigint_to_twos_words(value,result_count);
    uint32_t* result=(uint32_t*)kir_xmallocn(result_count,sizeof(uint32_t));
    memset(result,0,result_count*sizeof(uint32_t));
    for (size_t i=word_shift; i<result_count; ++i){
        size_t from=i-word_shift;
        if (from<result_count) result[i] |= source[from] << bit_shift;
        if (bit_shift && from>0 && from-1<result_count)
            result[i] |= source[from-1] >> (32-bit_shift);
    }
    KirBigInt* shifted=bigint_from_twos_words(result,result_count);
    kir_free(source);
    kir_free(result);
    return shifted;
}

KirBigInt* kir_bigint_shift_right(const KirBigInt* value, size_t bits){
    size_t count=bigint_twos_word_count(value);
    size_t word_shift=bits/32;
    unsigned int bit_shift=(unsigned int)(bits%32);
    if (!count) return NULL;
    uint32_t sign_fill=value->sign<0 ? UINT32_MAX : 0;
    if (word_shift>=count) return kir_bigint_from_i64(value->sign<0 ? -1 : 0);
    uint32_t* source=bigint_to_twos_words(value,count);
    uint32_t* result=(uint32_t*)kir_xmallocn(count,sizeof(uint32_t));
    for (size_t i=0; i<count; ++i){
        size_t from=i+word_shift;
        uint32_t low=from<count ? source[from] : sign_fill;
        uint32_t high=(from+1)<count ? source[from+1] : sign_fill;
        result[i]=bit_shift ? (low>>bit_shift) | (high<<(32-bit_shift)) : low;
    }
    KirBigInt* shifted=bigint_from_twos_words(result,count);
    kir_free(source);
    kir_free(result);
    return shifted;
}

int kir_bigint_compare(const KirBigInt* a, const KirBigInt* b){
    if (a->sign < b->sign) return -1;
    if (a->sign > b->sign) return 1;
    if (!a->sign) return 0;
    int cmp = bigint_compare_magnitude(a, b);
    return a->sign > 0 ? cmp : -cmp;
}

int kir_bigint_fits_i64(const KirBigInt* value, int64_t* out){
    uint64_t limit = value->sign < 0 ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t magnitude = 0;
    for (size_t i = value->len; i > 0; --i){
        if (magnitude > (limit - value->limbs[i - 1]) / KIR_BIGINT_BASE) return 0;
        magnitude = magnitude * KIR_BIGINT_BASE + value->limbs[i - 1];
    }
    if (out){
        if (value->sign < 0){
            *out = magnitude == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)magnitude;
        } else {
            *out = (int64_t)magnitude;
        }
    }
    return 1;
}

double kir_bigint_to_double(const KirBigInt* value){
    double result = 0.0;
    for (size_t i = value->len; i > 0; --i)
        result = result * (double)KIR_BIGINT_BASE + (double)value->limbs[i - 1];
    return value->sign < 0 ? -result : result;
}

char* kir_bigint_to_string(const KirBigInt* value, size_t* out_len){
    size_t capacity = value->len * 9 + 2;
    char* result = (char*)kir_malloc(capacity);
    size_t used = 0;
    if (value->sign < 0) result[used++] = '-';
    if (!value->len){
        result[used++] = '0';
    } else {
        int written = snprintf(result + used, capacity - used, "%u", value->limbs[value->len - 1]);
        if (written > 0) used += (size_t)written;
        for (size_t i = value->len - 1; i > 0; --i){
            written = snprintf(result + used, capacity - used, "%09u", value->limbs[i - 1]);
            if (written > 0) used += (size_t)written;
        }
    }
    result[used] = '\0';
    if (out_len) *out_len = used;
    return result;
}

int kir_bigint_compare_double(const KirBigInt* integer, double real, int* out_compare){
    if (isnan(real)) return 0;
    if (isinf(real)){
        if (out_compare) *out_compare = real > 0 ? -1 : 1;
        return 1;
    }
    if (integer->sign == 0){
        if (out_compare) *out_compare = real < 0 ? 1 : (real > 0 ? -1 : 0);
        return 1;
    }
    if (real == 0.0){
        if (out_compare) *out_compare = integer->sign;
        return 1;
    }
    int real_sign = real < 0 ? -1 : 1;
    if (integer->sign != real_sign){
        if (out_compare) *out_compare = integer->sign < real_sign ? -1 : 1;
        return 1;
    }

    double magnitude = fabs(real);
    double integral_part = 0.0;
    double fractional_part = modf(magnitude, &integral_part);
    char buffer[400];
    int length = snprintf(buffer, sizeof(buffer), "%.0f", integral_part);
    if (length <= 0 || (size_t)length >= sizeof(buffer)) return 0;
    KirBigInt* truncated = kir_bigint_parse(buffer, (size_t)length, 10);
    if (!truncated) return 0;
    int cmp = bigint_compare_magnitude(integer, truncated);
    kir_bigint_free(truncated);
    if (cmp == 0 && fractional_part != 0.0) cmp = -1;
    if (integer->sign < 0) cmp = -cmp;
    if (out_compare) *out_compare = cmp;
    return 1;
}
