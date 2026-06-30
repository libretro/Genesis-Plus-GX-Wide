/***************************************************************************************
 *  Genesis Plus GX
 *  FM bus enhancement (opt-in, fixed-point, FM-only) - implementation
 *
 *  See fm_enhance.h for the design overview. All arithmetic is integer/Q15 so
 *  the result is deterministic across platforms (no float in the audio path).
 *  Written to compile as MSVC C89: declarations at the top of each block,
 *  C-style comments only.
 ***************************************************************************************/

#include <string.h>
#include "fm_enhance.h"

/* ----- fixed-point helpers -------------------------------------------------- */

#define FMENH_Q15        15
#define FMENH_ONE_Q15    (1 << FMENH_Q15)   /* 1.0 in Q15 */

/* Reverb topology: 4 parallel combs into 2 series all-passes (a compact
   Freeverb). Base delay lengths are quoted at 44100 Hz and scaled to the
   active rate. Buffers are statically sized for the worst case (96000 Hz). */
#define FMENH_NCOMB      4
#define FMENH_NAP        2
#define FMENH_COMB_MAX   3072   /* >= ceil(1356 * 96000/44100) */
#define FMENH_AP_MAX     1280   /* >= ceil( 556 * 96000/44100) */

static const int comb_base[FMENH_NCOMB] = { 1116, 1188, 1277, 1356 };
static const int ap_base  [FMENH_NAP]   = {  556,  441 };

/* ----- per-level voicing (all Q15 unless noted) ----------------------------- */

typedef struct
{
  int width;      /* mid/side side-gain (Q15, > ONE widens) */
  int room;       /* comb feedback / room size (Q15) */
  int damp;       /* comb damping (Q15, high-frequency absorption) */
  int wet;        /* reverb send/return level (Q15) */
  int lp_a;       /* analog one-pole coefficient (Q16) */
  int drive;      /* soft-saturation drive (Q15, > ONE pushes harder) */
} fmenh_voice;

/* index 0 unused (Off); 1 = Light, 2 = Rich */
static const fmenh_voice voices[3] =
{
  { 0,            0,          0,           0,        0,       0      },
  { 40960 /*1.25*/, 22938 /*.70*/, 16384 /*.50*/,  3277 /*.10*/, 0x2666, 36045 /*1.10*/ },
  { 52429 /*1.60*/, 27525 /*.84*/, 13107 /*.40*/,  5898 /*.18*/, 0x4000, 45875 /*1.40*/ }
};

#define FMENH_AP_FB      16384  /* all-pass coefficient (~0.5, Q15) */
#define FMENH_SAT_FS     32768  /* soft-sat reference full-scale */

/* ----- state ---------------------------------------------------------------- */

static int   comb_buf[FMENH_NCOMB][FMENH_COMB_MAX];
static int   comb_len[FMENH_NCOMB];
static int   comb_pos[FMENH_NCOMB];
static int   comb_store[FMENH_NCOMB];        /* one-pole damping memory */

static int   ap_buf[FMENH_NAP][FMENH_AP_MAX];
static int   ap_len[FMENH_NAP];
static int   ap_pos[FMENH_NAP];

static int   lp_l, lp_r;                      /* analog low-pass memory (Q16) */

static int   level    = 0;
static fmenh_voice v;                         /* active voicing (copy) */

/* ----- internals ------------------------------------------------------------ */

static int scale_len(int base, int sample_rate, int maxlen)
{
  /* base is quoted at 44100 Hz; scale to the active rate */
  int len = (int)(((long)base * (long)sample_rate) / 44100L);
  if (len < 1)      len = 1;
  if (len > maxlen) len = maxlen;
  return len;
}

static void fm_enhance_clear(void)
{
  memset(comb_buf,   0, sizeof(comb_buf));
  memset(comb_store, 0, sizeof(comb_store));
  memset(comb_pos,   0, sizeof(comb_pos));
  memset(ap_buf,     0, sizeof(ap_buf));
  memset(ap_pos,     0, sizeof(ap_pos));
  lp_l = 0;
  lp_r = 0;
}

void fm_enhance_init(int sample_rate)
{
  int i;

  if (sample_rate < 1)
    sample_rate = 44100;

  for (i = 0; i < FMENH_NCOMB; i++)
    comb_len[i] = scale_len(comb_base[i], sample_rate, FMENH_COMB_MAX);
  for (i = 0; i < FMENH_NAP; i++)
    ap_len[i] = scale_len(ap_base[i], sample_rate, FMENH_AP_MAX);

  fm_enhance_clear();
}

void fm_enhance_set_level(int new_level)
{
  if (new_level < 0) new_level = 0;
  if (new_level > 2) new_level = 2;

  /* clear tails when crossing the On/Off boundary so nothing lingers */
  if ((new_level == 0) != (level == 0))
    fm_enhance_clear();

  level = new_level;
  v     = voices[new_level];
}

void fm_enhance_reset(void)
{
  fm_enhance_clear();
}

/* one mono reverb tick */
static int reverb_tick(int x)
{
  int i, out, y, store;

  out = 0;
  for (i = 0; i < FMENH_NCOMB; i++)
  {
    y = comb_buf[i][comb_pos[i]];

    /* low-pass inside the feedback path: store = y*(1-damp) + store*damp */
    store = ((y * (FMENH_ONE_Q15 - v.damp)) + (comb_store[i] * v.damp)) >> FMENH_Q15;
    comb_store[i] = store;

    comb_buf[i][comb_pos[i]] = x + ((store * v.room) >> FMENH_Q15);
    if (++comb_pos[i] >= comb_len[i])
      comb_pos[i] = 0;

    out += y;
  }

  /* average the parallel combs */
  out >>= 2;

  /* series all-passes */
  for (i = 0; i < FMENH_NAP; i++)
  {
    int bufout = ap_buf[i][ap_pos[i]];
    int in     = out;

    out = bufout - in;
    ap_buf[i][ap_pos[i]] = in + ((bufout * FMENH_AP_FB) >> FMENH_Q15);
    if (++ap_pos[i] >= ap_len[i])
      ap_pos[i] = 0;
  }

  return out;
}

/* cubic soft clip: y = 1.5*n - 0.5*n^3, n the drive-scaled input in
   [-FS, FS] (FS == 1.0). Operates on int16-range samples, integer only. */
static int soft_sat(int x, int drive)
{
  int n, n2, n3;

  n = (x * drive) >> FMENH_Q15;
  if (n >  FMENH_SAT_FS) n =  FMENH_SAT_FS;
  if (n < -FMENH_SAT_FS) n = -FMENH_SAT_FS;

  n2 = (n  * n) >> FMENH_Q15;
  n3 = (n2 * n) >> FMENH_Q15;
  return ((n * 3) - n3) >> 1;
}

void fm_enhance_block(short *buf, int frames)
{
  int i;

  if (level == 0)
    return;

  for (i = 0; i < frames; i++)
  {
    int l = buf[2 * i];
    int r = buf[2 * i + 1];
    int m, s, wet, sat;

    /* 1. stereo widener (mid/side) */
    m = (l + r) >> 1;
    s = (l - r) >> 1;
    s = (s * v.width) >> FMENH_Q15;
    l = m + s;
    r = m - s;

    /* 2. reverb send (mono in, wet added back to both channels) */
    wet = reverb_tick(m);
    wet = (wet * v.wet) >> FMENH_Q15;
    l += wet;
    r += wet;

    /* 3a. analog one-pole low-pass (16.16) */
    lp_l = (lp_l * v.lp_a + l * (0x10000 - v.lp_a)) >> 16;
    lp_r = (lp_r * v.lp_a + r * (0x10000 - v.lp_a)) >> 16;
    l = lp_l;
    r = lp_r;

    /* 3b. cubic soft-saturation (also tames peaks from stages 1/2) */
    sat = soft_sat(l, v.drive);
    if (sat >  32767) sat =  32767;
    if (sat < -32768) sat = -32768;
    buf[2 * i] = (short)sat;

    sat = soft_sat(r, v.drive);
    if (sat >  32767) sat =  32767;
    if (sat < -32768) sat = -32768;
    buf[2 * i + 1] = (short)sat;
  }
}
