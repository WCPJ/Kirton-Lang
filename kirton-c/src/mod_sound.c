/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_sound.c — многоканальный звуковой модуль Киртона (Windows, WinMM waveOut)
// Возможности:
//   звук_загрузить(путь)        -> id (число >0) или 0 при ошибке
//   звук_играть(id)             -> bool (однократное воспроизведение)
//   звук_играть_цикл(id)        -> bool (играет в цикле, пока не остановишь)
//   звук_остановить()           -> bool (останавливает все активные звуки)
//   звук_освободить(id)         -> bool (выгружает звук из памяти)
//
// Поддерживаемые форматы:
//   - WAV PCM, 8 или 16 бит на сэмпл
//   - 1 или 2 канала (mono / stereo)
//   - любая частота дискретизации (играет с «родной» скоростью)
// Внутренний формат микшера: stereo 16-bit, 44100 Hz.
//
// Требует: kirton/native.h, kirton/value.h
// Линковка: winmm.lib

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <wchar.h>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

/* ===================== Константы аудиосистемы ===================== */

#define SND_MIXER_SAMPLE_RATE   44100      // Гц
#define SND_MIXER_CHANNELS      2          // стерео
#define SND_MIXER_BITS          16         // 16 бит
#define SND_MIXER_BYTES_PER_SMP 2          // 2 байта на сэмпл
#define SND_MIXER_FRAME_BYTES   (SND_MIXER_CHANNELS * SND_MIXER_BYTES_PER_SMP)

#define SND_BUFFER_FRAMES  1024           // кадров на один буфер ( ~23мс при 44.1кГц )
#define SND_NUM_BUFFERS    4              // количество буферов в круге

#define SND_MAX_SOUNDS     256            // максимум загруженных звуков
#define SND_MAX_VOICES     32             // максимум одновременно играющих "голосов"

/* ===================== UTF-8 → UTF-16 (Windows) ===================== */

static wchar_t* snd_u8_to_w(const char* s, int n){
    if (!s) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, (n>=0)?n:-1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, (n>=0)?n:-1, w, wlen);
    w[wlen] = 0;
    return w;
}

/* ===================== WAV-структуры ===================== */

#pragma pack(push,1)
typedef struct {
    char     chunkID[4];   // "RIFF"
    uint32_t chunkSize;
    char     format[4];    // "WAVE"
} RiffHeader;

typedef struct {
    char     subchunk1ID[4];   // "fmt "
    uint32_t subchunk1Size;    // >=16 для PCM
    uint16_t audioFormat;      // 1 = PCM
    uint16_t numChannels;      // 1 или 2
    uint32_t sampleRate;       // 44100 и т.п.
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;    // 8 или 16
} FmtChunk;

typedef struct {
    char     id[4];
    uint32_t size;
} GenericChunkHeader;
#pragma pack(pop)

/* ===================== Структуры хранилища звуков ===================== */

typedef struct {
    int     frames;    // количество кадров (frame = стерео-сэмпл)
    int16_t* data;     // длина frames * 2 (L,R,L,R,...)
} Sound;

static Sound g_sounds[SND_MAX_SOUNDS];
static int   g_sounds_used = 0;

/* ===================== Голоса (активные проигрывания) ===================== */

typedef struct {
    int   sound_index;   // индекс в g_sounds, или -1, если не используется
    int   pos;           // текущая позиция в кадрах
    int   loop;          // 1 = играть по кругу
    float volume;        // 0.0..1.0 (пока используем 1.0)
} Voice;

static Voice g_voices[SND_MAX_VOICES];

/* ===================== Синхронизация и waveOut ===================== */

static CRITICAL_SECTION g_snd_cs;
static int g_snd_initialized = 0;

static HWAVEOUT g_hwo = NULL;
static WAVEHDR  g_hdrs[SND_NUM_BUFFERS];
static int16_t  g_buffers[SND_NUM_BUFFERS][SND_BUFFER_FRAMES * SND_MIXER_CHANNELS];

static HANDLE   g_thread = NULL;
static volatile LONG g_thread_running = 0;
static int g_snd_atexit_registered = 0;

static void snd_system_shutdown(void);

/* ===================== Утилиты хранилища звуков ===================== */

static void snd_sounds_init(void){
    for (int i = 0; i < SND_MAX_SOUNDS; i++){
        g_sounds[i].frames = 0;
        g_sounds[i].data   = NULL;
    }
    g_sounds_used = 0;
}

static void snd_voices_init(void){
    for (int i = 0; i < SND_MAX_VOICES; i++){
        g_voices[i].sound_index = -1;
        g_voices[i].pos         = 0;
        g_voices[i].loop        = 0;
        g_voices[i].volume      = 1.0f;
    }
}

/* Найти свободный слот под загруженный звук. Возвращает индекс или -1. */
static int snd_alloc_sound_slot(void){
    for (int i = 0; i < SND_MAX_SOUNDS; i++){
        if (!g_sounds[i].data){
            if (i >= g_sounds_used) g_sounds_used = i+1;
            return i;
        }
    }
    return -1;
}

/* Освободить все звуки (на всякий, сейчас напрямую не вызывается) */
static void snd_free_all_sounds(void){
    for (int i = 0; i < SND_MAX_SOUNDS; i++){
        if (g_sounds[i].data){
            free(g_sounds[i].data);
            g_sounds[i].data = NULL;
            g_sounds[i].frames = 0;
        }
    }
    g_sounds_used = 0;
}

/* ===================== Парсинг WAV PCM 8/16 бит ===================== */
/* Конвертируем любой PCM 8/16 бит, mono/stereo → наш stereo 16-bit. */

static int snd_load_wav_any(const char* utf8_path, int utf8_len,
                            int* out_frames, int16_t** out_data){
    *out_frames = 0;
    *out_data   = NULL;

    if (!utf8_path) return 0;

    FILE* f = NULL;
#ifdef _WIN32
    wchar_t* wp = snd_u8_to_w(utf8_path, utf8_len);
    if (!wp) return 0;
    f = _wfopen(wp, L"rb");
    free(wp);
#else
    f = fopen(utf8_path, "rb");
#endif
    if (!f) return 0;

    RiffHeader rh;
    if (fread(&rh, sizeof(rh), 1, f) != 1){
        fclose(f);
        return 0;
    }
    if (memcmp(rh.chunkID, "RIFF", 4) != 0 || memcmp(rh.format, "WAVE", 4) != 0){
        fclose(f);
        return 0;
    }

    FmtChunk fmt;
    memset(&fmt, 0, sizeof(fmt));
    int got_fmt = 0;
    uint32_t data_offset = 0;
    uint32_t data_size   = 0;

    // Читаем чанки до тех пор, пока не найдём fmt и data
    while (!got_fmt || data_size == 0){
        GenericChunkHeader ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1){
            fclose(f);
            return 0;
        }

        if (memcmp(ch.id, "fmt ", 4) == 0){
            // PCM гарантированно имеет минимум 16 байт параметров
            if (ch.size < 16){
                fseek(f, ch.size, SEEK_CUR);
                continue;
            }
            // читаем только первые 16 байт fmt-чанка (остальное нам не интересно)
            if (fread(&fmt, 16, 1, f) != 1){
                fclose(f);
                return 0;
            }
            if (ch.size > 16){
                fseek(f, ch.size - 16, SEEK_CUR);
            }
            got_fmt = 1;
        }else if (memcmp(ch.id, "data", 4) == 0){
            data_offset = (uint32_t)ftell(f);
            data_size   = ch.size;
            fseek(f, ch.size, SEEK_CUR);
        }else{
            // пропустить неизвестный чанк
            fseek(f, ch.size, SEEK_CUR);
        }

        if (feof(f)) break;
    }

    if (!got_fmt || data_size == 0){
        fclose(f);
        return 0;
    }

    // Проверяем формат: PCM, 8/16 бит, mono/stereo
    if (fmt.audioFormat != 1){ // только несжатый PCM
        fclose(f);
        return 0;
    }
    if (fmt.bitsPerSample != 8 && fmt.bitsPerSample != 16){
        fclose(f);
        return 0;
    }
    if (fmt.numChannels != 1 && fmt.numChannels != 2){
        fclose(f);
        return 0;
    }

    // Позиционируемся на data
    if (fseek(f, (long)data_offset, SEEK_SET) != 0){
        fclose(f);
        return 0;
    }

    int bytes_per_sample = fmt.bitsPerSample / 8;
    int bytes_per_frame_src = bytes_per_sample * fmt.numChannels;
    if (bytes_per_frame_src <= 0){
        fclose(f);
        return 0;
    }

    int total_frames_src = (int)(data_size / (uint32_t)bytes_per_frame_src);
    if (total_frames_src <= 0){
        fclose(f);
        return 0;
    }

    // Читаем сырые байты
    uint8_t* raw = (uint8_t*)malloc((size_t)total_frames_src * (size_t)bytes_per_frame_src);
    if (!raw){
        fclose(f);
        return 0;
    }
    size_t read_cnt = fread(raw, (size_t)bytes_per_frame_src, (size_t)total_frames_src, f);
    fclose(f);
    if (read_cnt != (size_t)total_frames_src){
        free(raw);
        return 0;
    }

    // Конвертация в stereo 16-bit
    int16_t* dst = (int16_t*)malloc((size_t)total_frames_src * 2 * sizeof(int16_t));
    if (!dst){
        free(raw);
        return 0;
    }

    if (fmt.bitsPerSample == 16){
        const int16_t* src16 = (const int16_t*)raw;
        if (fmt.numChannels == 1){
            // моно 16 → стерео
            for (int i = 0; i < total_frames_src; i++){
                int16_t s = src16[i];
                dst[i*2 + 0] = s;
                dst[i*2 + 1] = s;
            }
        }else{
            // стерео 16 → копируем как есть
            for (int i = 0; i < total_frames_src; i++){
                dst[i*2 + 0] = src16[i*2 + 0];
                dst[i*2 + 1] = src16[i*2 + 1];
            }
        }
    }else{
        // 8 бит unsigned → 16 бит signed
        const uint8_t* src8 = raw;
        if (fmt.numChannels == 1){
            for (int i = 0; i < total_frames_src; i++){
                uint8_t u = src8[i];
                int16_t s = (int16_t)(((int)u - 128) << 8);
                dst[i*2 + 0] = s;
                dst[i*2 + 1] = s;
            }
        }else{
            for (int i = 0; i < total_frames_src; i++){
                uint8_t uL = src8[i*2 + 0];
                uint8_t uR = src8[i*2 + 1];
                int16_t sL = (int16_t)(((int)uL - 128) << 8);
                int16_t sR = (int16_t)(((int)uR - 128) << 8);
                dst[i*2 + 0] = sL;
                dst[i*2 + 1] = sR;
            }
        }
    }

    free(raw);

    *out_frames = total_frames_src;
    *out_data   = dst;
    return 1;
}

/* ===================== Микшер ===================== */

static void snd_mix_buffer(int16_t* out, int frames){
    // out: [frames * 2] int16 (L,R, L,R, ...)

    int32_t accL, accR;

    EnterCriticalSection(&g_snd_cs);

    for (int i = 0; i < frames; i++){
        accL = 0;
        accR = 0;

        for (int v = 0; v < SND_MAX_VOICES; v++){
            Voice* voice = &g_voices[v];
            if (voice->sound_index < 0) continue;

            Sound* snd = &g_sounds[voice->sound_index];
            if (!snd->data || snd->frames <= 0) continue;

            int pos = voice->pos;
            if (pos >= snd->frames){
                if (voice->loop){
                    pos = pos % snd->frames;
                    voice->pos = pos;
                }else{
                    voice->sound_index = -1;
                    continue;
                }
            }

            int idx = pos * 2;
            int16_t sL = snd->data[idx + 0];
            int16_t sR = snd->data[idx + 1];

            float vol = voice->volume;
            int32_t vL = (int32_t)((float)sL * vol);
            int32_t vR = (int32_t)((float)sR * vol);

            accL += vL;
            accR += vR;

            voice->pos++;
        }

        if (accL > 32767) accL = 32767;
        if (accL < -32768) accL = -32768;
        if (accR > 32767) accR = 32767;
        if (accR < -32768) accR = -32768;

        out[i*2 + 0] = (int16_t)accL;
        out[i*2 + 1] = (int16_t)accR;
    }

    LeaveCriticalSection(&g_snd_cs);
}

/* ===================== Аудио-поток (waveOut) ===================== */

static DWORD WINAPI snd_thread_proc(LPVOID param){
    (void)param;

    WAVEFORMATEX fmt;
    ZeroMemory(&fmt, sizeof(fmt));
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = SND_MIXER_CHANNELS;
    fmt.nSamplesPerSec  = SND_MIXER_SAMPLE_RATE;
    fmt.wBitsPerSample  = SND_MIXER_BITS;
    fmt.nBlockAlign     = (fmt.nChannels * fmt.wBitsPerSample) / 8;
    fmt.nAvgBytesPerSec = fmt.nBlockAlign * fmt.nSamplesPerSec;

    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR){
        g_hwo = NULL;
        InterlockedExchange(&g_thread_running,0);
        return 0;
    }

    for (int i = 0; i < SND_NUM_BUFFERS; i++){
        ZeroMemory(&g_hdrs[i], sizeof(WAVEHDR));
        g_hdrs[i].lpData         = (LPSTR)g_buffers[i];
        g_hdrs[i].dwBufferLength = (DWORD)(SND_BUFFER_FRAMES * SND_MIXER_FRAME_BYTES);
        g_hdrs[i].dwFlags        = 0;
        waveOutPrepareHeader(g_hwo, &g_hdrs[i], sizeof(WAVEHDR));
    }

    while (InterlockedCompareExchange(&g_thread_running,0,0)){
        for (int i = 0; i < SND_NUM_BUFFERS; i++){
            WAVEHDR* hdr = &g_hdrs[i];
            if (!(hdr->dwFlags & WHDR_INQUEUE)){
                snd_mix_buffer((int16_t*)hdr->lpData, SND_BUFFER_FRAMES);
                hdr->dwBufferLength = (DWORD)(SND_BUFFER_FRAMES * SND_MIXER_FRAME_BYTES);
                hdr->dwFlags &= ~WHDR_DONE;
                waveOutWrite(g_hwo, hdr, sizeof(WAVEHDR));
            }
        }
        Sleep(5);
    }

    waveOutReset(g_hwo);

    for (int i = 0; i < SND_NUM_BUFFERS; i++){
        waveOutUnprepareHeader(g_hwo, &g_hdrs[i], sizeof(WAVEHDR));
    }

    waveOutClose(g_hwo);
    g_hwo = NULL;

    return 0;
}

/* ===================== Инициализация / остановка подсистемы ===================== */

static void snd_system_init_once(void){
    if (g_snd_initialized) return;
    g_snd_initialized = 1;

    InitializeCriticalSection(&g_snd_cs);
    snd_sounds_init();
    snd_voices_init();

    InterlockedExchange(&g_thread_running,1);
    g_thread = CreateThread(NULL, 0, snd_thread_proc, NULL, 0, NULL);
    if (!g_snd_atexit_registered){
        atexit(snd_system_shutdown);
        g_snd_atexit_registered=1;
    }
}

static void snd_system_shutdown(void){
    if (!g_snd_initialized) return;
    InterlockedExchange(&g_thread_running,0);
    if (g_thread){
        WaitForSingleObject(g_thread,INFINITE);
        CloseHandle(g_thread);
        g_thread=NULL;
    }
    if (g_hwo){
        waveOutReset(g_hwo);
        waveOutClose(g_hwo);
        g_hwo=NULL;
    }
    EnterCriticalSection(&g_snd_cs);
    for (int i=0;i<SND_MAX_SOUNDS;++i){
        free(g_sounds[i].data);
        g_sounds[i].data=NULL;
        g_sounds[i].frames=0;
    }
    LeaveCriticalSection(&g_snd_cs);
    DeleteCriticalSection(&g_snd_cs);
    g_snd_initialized=0;
}

/* ===================== Нативные функции для Киртона ===================== */

// звук_загрузить(путь) -> id (>0) или 0 при ошибке
static Value k_звук_загрузить(int n, const Value* a){
    snd_system_init_once();

    if (n < 1 || a[0].kind != VAL_STR || a[0].as.str.n > INT_MAX) return v_int(0);

    int frames = 0;
    int16_t* data = NULL;

    if (!snd_load_wav_any(a[0].as.str.s, (int)a[0].as.str.n, &frames, &data)){
        return v_int(0);
    }

    EnterCriticalSection(&g_snd_cs);

    int slot = snd_alloc_sound_slot();
    if (slot < 0){
        LeaveCriticalSection(&g_snd_cs);
        free(data);
        return v_int(0);
    }

    g_sounds[slot].frames = frames;
    g_sounds[slot].data   = data;

    LeaveCriticalSection(&g_snd_cs);

    int id = slot + 1;
    return v_int(id);
}

// звук_освободить(id) -> bool
static Value k_звук_освободить(int n, const Value* a){
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int64_t id64=0;
    if (!v_num_fits_i64(a[0], &id64) || id64 < INT_MIN || id64 > INT_MAX) return v_bool(false);
    int id = (int)id64;
    if (id <= 0 || id > SND_MAX_SOUNDS) return v_bool(false);
    int idx = id - 1;

    EnterCriticalSection(&g_snd_cs);

    for (int v = 0; v < SND_MAX_VOICES; v++){
        if (g_voices[v].sound_index == idx){
            g_voices[v].sound_index = -1;
        }
    }

    if (g_sounds[idx].data){
        free(g_sounds[idx].data);
        g_sounds[idx].data   = NULL;
        g_sounds[idx].frames = 0;
    }

    LeaveCriticalSection(&g_snd_cs);

    return v_bool(true);
}

// Вспомогательная: добавить голос (звук + loop) -> bool
static int snd_start_voice(int sound_index, int loop){
    if (sound_index < 0 || sound_index >= SND_MAX_SOUNDS) return 0;
    if (!g_sounds[sound_index].data || g_sounds[sound_index].frames <= 0) return 0;

    EnterCriticalSection(&g_snd_cs);

    int ok = 0;
    for (int i = 0; i < SND_MAX_VOICES; i++){
        if (g_voices[i].sound_index < 0){
            g_voices[i].sound_index = sound_index;
            g_voices[i].pos         = 0;
            g_voices[i].loop        = loop ? 1 : 0;
            g_voices[i].volume      = 1.0f;
            ok = 1;
            break;
        }
    }

    LeaveCriticalSection(&g_snd_cs);
    return ok;
}

// звук_играть(id) -> bool
static Value k_звук_играть(int n, const Value* a){
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int64_t id64=0;
    if (!v_num_fits_i64(a[0], &id64) || id64 < INT_MIN || id64 > INT_MAX) return v_bool(false);
    int id = (int)id64;
    if (id <= 0 || id > SND_MAX_SOUNDS) return v_bool(false);
    int idx = id - 1;

    int ok = snd_start_voice(idx, 0);
    return v_bool(ok != 0);
}

// звук_играть_цикл(id) -> bool
static Value k_звук_играть_цикл(int n, const Value* a){
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int64_t id64=0;
    if (!v_num_fits_i64(a[0], &id64) || id64 < INT_MIN || id64 > INT_MAX) return v_bool(false);
    int id = (int)id64;
    if (id <= 0 || id > SND_MAX_SOUNDS) return v_bool(false);
    int idx = id - 1;

    int ok = snd_start_voice(idx, 1);
    return v_bool(ok != 0);
}

// звук_остановить() -> bool (останавливает все активные голоса)
static Value k_звук_остановить(int n, const Value* a){
    (void)n; (void)a;
    EnterCriticalSection(&g_snd_cs);
    for (int i = 0; i < SND_MAX_VOICES; i++){
        g_voices[i].sound_index = -1;
    }
    LeaveCriticalSection(&g_snd_cs);
    return v_bool(true);
}

/* ===================== Регистрация ===================== */

static const KirNativeDef FUNCS[] = {
    {"звук_загрузить",     k_звук_загрузить},
    {"звук_играть",        k_звук_играть},
    {"звук_играть_цикл",   k_звук_играть_цикл},
    {"звук_остановить",    k_звук_остановить},
    {"звук_освободить",    k_звук_освободить},
};

void sound_init(void){
    snd_system_init_once();
    kir_native_register(FUNCS, (int)(sizeof(FUNCS)/sizeof(FUNCS[0])));
    kir_native_mark_bytecode_safe(FUNCS, sizeof(FUNCS)/sizeof(FUNCS[0]));
}

#else  // не Windows — заглушки

static const KirNativeDef FUNCS[] = {
    // Пусто: на не-Windows здесь звука нет
};

void sound_init(void){
    (void)FUNCS;
}

#endif // _WIN32
