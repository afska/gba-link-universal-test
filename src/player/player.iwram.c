#include "player.h"

#include <gba_dma.h>
#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_sound.h>
#include <gba_systemcalls.h>
#include <gba_timers.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>  // for memset

#include "../utils/gbfs/gbfs.h"
#include "PlaybackState.h"

#define BUFFER_SIZE 608

#define TIMER_16MHZ 0
#define FIFO_ADDR_B 0x040000A4
#define CHANNEL_B_MUTE 0xcfff   /*0b1100111111111111*/
#define CHANNEL_B_UNMUTE 0x3000 /*0b0011000000000000*/
#define AUDIO_CHUNK_SIZE_PCM 304
#define FRACUMUL_PRECISION 0xFFFFFFFF
#define AS_MSECS_PCM 118273043  // 0xffffffff * (1000/36314)
#define AS_CURSOR_PCM 1348619731
#define REG_DMA2CNT_L *(vu16*)(REG_BASE + 0x0d0)
#define REG_DMA2CNT_H *(vu16*)(REG_BASE + 0x0d2)

#define CODE_ROM __attribute__((section(".code")))
#define CODE_EWRAM __attribute__((section(".ewram")))
#define INLINE static inline __attribute__((always_inline))

Playback PlaybackState;

// ------------------------------------
// Music player (for PCM files)
// ------------------------------------
// Audio is taken from the embedded GBFS file in ROM.
// FIFO channel B, Timer 1 and DMA2 are used.
// The sample rate is 36314hz.
// Each PCM chunk is 304 bytes and represents 304 samples.
// Two chunks are copied per frame, filling the 608 entries of the buffer.
// (This is one of the few combinations of sample rate / buffer size that
// time out perfectly in the 280896 cycles of a GBA frame)
// See: (JS code)
//   for (var i=80; i<1000; i++)
//     if ((280896%i)==0 && (i%16) == 0)
//       console.log(
//         'timer =', 65536-(280896/i), '; buffer =',
//         i, '; sample rate =', i*(1<<24)/280896, 'hz'
//       );

static const u32 rate_delays[] = {1, 2, 4, 0, 4, 2, 1};
static int rate = 0;
static u32 rate_counter = 0;
static u32 current_audio_chunk = 0;
static bool did_run = false;
static bool is_paused = false;

#define AS_MSECS (AS_MSECS_PCM)

#define AUDIO_PROCESS(ON_STEP, ON_STOP)                              \
  did_run = true;                                                    \
  buffer = double_buffers[cur_buffer];                               \
                                                                     \
  if (src != NULL) {                                                 \
    if (src_pos < src_len) {                                         \
      u32 pending_bytes = src_len - src_pos;                         \
      u32 bytes_to_read =                                            \
          pending_bytes < BUFFER_SIZE ? pending_bytes : BUFFER_SIZE; \
      for (u32 i = 0; i < bytes_to_read / 4; i++)                    \
        ((u32*)buffer)[i] = ((u32*)(src + src_pos))[i];              \
      src_pos += bytes_to_read;                                      \
      if (src_pos >= src_len) {                                      \
        ON_STOP;                                                     \
      }                                                              \
    } else {                                                         \
      ON_STOP;                                                       \
    }                                                                \
  }

u32 fracumul(u32 x, u32 frac) __attribute__((long_call));
static const GBFS_FILE* fs;
static const unsigned char* src = NULL;
static u32 src_len = 0;
static u32 src_pos = 0;
static s8 double_buffers[2][BUFFER_SIZE] __attribute__((aligned(4)));
static u32 decode_pos = 160, cur_buffer = 0;
static s8* buffer;
static int last_sample = 0;

INLINE void mute() {
  DSOUNDCTRL = DSOUNDCTRL & CHANNEL_B_MUTE;
}

INLINE void unmute() {
  DSOUNDCTRL = DSOUNDCTRL | CHANNEL_B_UNMUTE;
}

INLINE void turn_on_sound() {
  SETSNDRES(1);
  SNDSTAT = SNDSTAT_ENABLE;
  DSOUNDCTRL = 0xf00c; /*0b1111000000001100*/
  mute();
}

INLINE void init() {
  /* TMxCNT_L is count; TMxCNT_H is control */
  REG_TM1CNT_H = 0;
  REG_TM1CNT_L = 0x10000 - (924 / 2);        // 0x10000 - (16777216 / 36314)
  REG_TM1CNT_H = TIMER_16MHZ | TIMER_START;  //            cpuFreq  / sampleRate

  mute();
}

INLINE void stop() {
  mute();
  src = NULL;
  decode_pos = 160;
  cur_buffer = 0;
  last_sample = 0;
  for (u32 i = 0; i < 2; i++) {
    u32* bufferPtr = (u32*)double_buffers[i];
    for (u32 j = 0; j < BUFFER_SIZE / 4; j++)
      bufferPtr[j] = 0;
  }
}

INLINE void disable_audio_dma() {
  // This convoluted process disables DMA2 in a "safe" way,
  // avoiding DMA lockups.
  //
  // 32-bit write
  // enabled = 1; start timing = immediately; transfer type = 32 bits;
  // repeat = off; destination = fixed; other bits = no change
  REG_DMA2CNT = (REG_DMA2CNT & 0xcdff /*0b00000000000000001100110111111111*/) |
                (0x0004 << 16) | DMA_ENABLE | DMA32 | DMA_DST_FIXED;
  //
  // wait 4 cycles
  asm volatile("eor r0, r0; eor r0, r0" ::: "r0");
  asm volatile("eor r0, r0; eor r0, r0" ::: "r0");
  //
  // 16-bit write
  // enabled = 0; start timing = immediately; transfer type = 32 bits;
  // repeat = off; destination = fixed; other bits = no change
  REG_DMA2CNT_H = (REG_DMA2CNT_H & 0x4dff /*0b0100110111111111*/) |
                  0x500 /*0b0000010100000000*/;  // DMA32 | DMA_DST_FIXED
  //
  // wait 4 more cycles
  asm volatile("eor r0, r0; eor r0, r0" ::: "r0");
  asm volatile("eor r0, r0; eor r0, r0" ::: "r0");
}

INLINE void dsound_start_audio_copy(const void* source) {
  // disable DMA2
  disable_audio_dma();

  // setup DMA2 for audio
  REG_DMA2SAD = (intptr_t)source;
  REG_DMA2DAD = (intptr_t)FIFO_ADDR_B;
  REG_DMA2CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL |
                DMA_ENABLE | 1;
}

INLINE void load_file(const char* name) {
  PlaybackState.msecs = 0;
  PlaybackState.hasFinished = false;
  PlaybackState.isLooping = false;
  is_paused = false;
  rate = 0;
  rate_counter = 0;
  current_audio_chunk = 0;

  stop();

  src = gbfs_get_obj(fs, name, &src_len);
  src_pos = 0;
}

CODE_ROM void player_init() {
  fs = find_first_gbfs_file(0);
  turn_on_sound();
  init();

  PlaybackState.msecs = 0;
  PlaybackState.hasFinished = false;
  PlaybackState.isLooping = false;
}

CODE_ROM void player_unload() {
  disable_audio_dma();
}

CODE_ROM void player_playPCM(const char* name) {
  load_file(name);
}

CODE_ROM void player_setLoop(bool enable) {
  PlaybackState.isLooping = enable;
}

CODE_ROM void player_setPause(bool enable) {
  is_paused = enable;
}

CODE_ROM void player_seek(unsigned int msecs) {
  // (cursor must be a multiple of AUDIO_CHUNK_SIZE)
  // cursor = src_pos

  // cursor = msecs * (sampleRate / 1000) = msecs * 36.314
  // => cursor = msecs * (36 + 0.314)

  unsigned int cursor = msecs * 36 + fracumul(msecs, AS_CURSOR_PCM);
  cursor = (cursor / AUDIO_CHUNK_SIZE_PCM) * AUDIO_CHUNK_SIZE_PCM;
  src_pos = cursor;
  rate_counter = 0;
  current_audio_chunk = 0;
}

CODE_ROM unsigned int player_getCursor() {
  return src_pos;
}

CODE_ROM void player_setCursor(unsigned int cursor) {
  src_pos = cursor;
}

CODE_ROM void player_setRate(int newRate) {
  rate = newRate;
  rate_counter = 0;
}

CODE_ROM void player_stop() {
  stop();

  PlaybackState.msecs = 0;
  PlaybackState.hasFinished = false;
  PlaybackState.isLooping = false;
  is_paused = false;
  rate = 0;
  rate_counter = 0;
  current_audio_chunk = 0;
}

CODE_ROM bool player_isPlaying() {
  return src != NULL;
}

void player_onVBlank() {
  dsound_start_audio_copy(double_buffers[cur_buffer]);

  if (!did_run)
    return;

  if (src != NULL)
    unmute();

  cur_buffer = !cur_buffer;
  did_run = false;
}

CODE_ROM void update_rate() {
  if (rate != 0) {
    rate_counter++;
    if (rate_counter == rate_delays[rate + RATE_LEVELS]) {
      rate_counter = 0;
    }
  }
}

void player_update(int expectedAudioChunk,
                   void (*onAudioChunks)(unsigned int current)) {
  if (is_paused) {
    mute();
    return;
  }

  // > multiplayer audio sync
  bool isSynchronized = expectedAudioChunk > 0;
  int availableAudioChunks = expectedAudioChunk - current_audio_chunk;
  if (isSynchronized && availableAudioChunks > AUDIO_SYNC_LIMIT) {
    // underrun (slave is behind master)
    unsigned int diff = availableAudioChunks - AUDIO_SYNC_LIMIT;

    src_pos += AUDIO_CHUNK_SIZE_PCM * diff;
    current_audio_chunk += diff;
    availableAudioChunks = AUDIO_SYNC_LIMIT;
  }

  // > adjust position based on audio rate
  update_rate();

  // > audio processing (back buffer)
  AUDIO_PROCESS(
      {
        if (isSynchronized) {
          availableAudioChunks--;

          if (availableAudioChunks < -AUDIO_SYNC_LIMIT) {
            // overrun (master is behind slave)
            src_pos -= AUDIO_CHUNK_SIZE_PCM;
            availableAudioChunks = -AUDIO_SYNC_LIMIT;
          } else
            current_audio_chunk++;
        } else
          current_audio_chunk++;
      },
      {
        if (PlaybackState.isLooping)
          player_seek(0);
        else {
          player_stop();
          PlaybackState.hasFinished = true;
        }
      });

  // > notify multiplayer audio sync cursor
  onAudioChunks(current_audio_chunk);

  // > calculate played milliseconds
  PlaybackState.msecs = fracumul(src_pos, AS_MSECS);
}
