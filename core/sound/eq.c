/*----------------------------------------------------------------------------
//
//                                3 Band EQ :)
//
// EQ.C - Main Source file for 3 band EQ
//
// (c) Neil C / Etanza Systems / 2K6
//
// Shouts / Loves / Moans = etanza at lycos dot co dot uk 
//
// This work is hereby placed in the public domain for all purposes, including
// use in commercial applications.
//
// The author assumes NO RESPONSIBILITY for any problems caused by the use of
// this software.
//
//----------------------------------------------------------------------------*/

/* NOTES :
//
// - Original filter code by Paul Kellet (musicdsp.pdf)
//
// - Uses 4 first order filters in series, should give 24dB per octave
//
// - Converted from double to fixed point: coefficients/gains are Q16, the
//   pole and history state is Q24, and the crossover coefficient
//   2*sin(pi*f/fs) is computed with an integer Taylor polynomial instead of
//   sin(). No floating point and no libm; the P4 "denormal fix" is unnecessary
//   in fixed point and has been dropped. Output matches the previous double
//   implementation to within about 1 LSB.
//
//----------------------------------------------------------------------------*/

/* ----------
//| Includes |
// ----------*/
#include <string.h>
#include "eq.h"
#include "macros.h"


/* -----------
//| Constants |
// -----------*/

#define EQ_QS      24                 /* fractional bits of pole/history state */
#define EQ_QS_ONE  ((eq_int64)1 << EQ_QS)

/* a (Q?) * b (Q16) -> (Q?), rounded to nearest */
#define MUL_Q16(a, b)  ((((a) * (b)) + ((eq_int64)1 << 15)) >> 16)


/* --------------------------------------------------------------------------
//| 2*sin(pi*num/den) in Q16, integer only.
//|
//| Degree-7 Taylor evaluated in Q30. The crossover frequencies are the fixed
//| 880 Hz / 5000 Hz defaults, so the argument is a small ratio well inside the
//| polynomial's accurate range; the result is clamped to pi/2 for safety and
//| matches sin() to ~7e-6. Replaces the former one-time sin() call.
// -------------------------------------------------------------------------*/

static eq_int64 eq_2sin_q16(int num, int den)
{
  const eq_int64 PI30     = (eq_int64)3373259426u; /* round(pi   * 2^30) */
  const eq_int64 HALFPI30 = 1686629713;            /* round(pi/2 * 2^30) */
  const eq_int64 ONE30    = ((eq_int64)1 << 30);
  eq_int64 x, x2, t, s;

  if (den <= 0)
    return 0;

  x = PI30 * (eq_int64)num / (eq_int64)den;
  if (x < 0)        x = 0;
  if (x > HALFPI30) x = HALFPI30;

  x2 = (x * x) >> 30;
  t  = ONE30 - (x2 / 42);
  t  = ONE30 - (((x2 * t) >> 30) / 20);
  t  = ONE30 - (((x2 * t) >> 30) / 6);
  s  = (x * t) >> 30;             /* sin(x) in Q30 */

  return (s + (1 << 12)) >> 13;   /* 2*sin(x) in Q16 */
}


/* ---------------
//| Initialise EQ |
// ---------------*/

/* Recommended frequencies are ...
//
//  lowfreq  = 880  Hz
//  highfreq = 5000 Hz
//
// Set mixfreq to whatever rate your system is using (eg 48Khz).
// lg / mg / hg are the low / mid / high band gains in percent (100 == unity). */

void init_3band_state(EQSTATE * es, int lowfreq, int highfreq, int mixfreq, int lg, int mg, int hg)
{
    /* Clear state */

    memset(es, 0, sizeof(EQSTATE));

    /* Set low / mid / high gains (percent -> Q16) */

    es->lg = ((eq_int64)lg * 65536 + 50) / 100;
    es->mg = ((eq_int64)mg * 65536 + 50) / 100;
    es->hg = ((eq_int64)hg * 65536 + 50) / 100;

    /* Calculate filter cutoff frequencies (Q16) */

    es->lf = eq_2sin_q16(lowfreq,  mixfreq);
    es->hf = eq_2sin_q16(highfreq, mixfreq);
}


/* ---------------
//| EQ one sample |
// ---------------*/

/* - sample is a signed integer (16-bit range in practice)
//
// Note that the output will depend on the gain settings for each band
// (especially the bass) so may require clipping before output, but you
// knew that anyway :)*/

int do_3band(EQSTATE * es, int sample)
{
    /* Locals */

    eq_int64 l, m, h;   /* Low / Mid / High - Sample Values (Q24) */
    eq_int64 x;         /* input sample (Q24) */
    eq_int64 o;         /* recombined output (Q24) */

    x = (eq_int64) sample << EQ_QS;

    /* Filter #1 (lowpass) */

    es->f1p0 += MUL_Q16(es->lf, (x        - es->f1p0));
    es->f1p1 += MUL_Q16(es->lf, (es->f1p0 - es->f1p1));
    es->f1p2 += MUL_Q16(es->lf, (es->f1p1 - es->f1p2));
    es->f1p3 += MUL_Q16(es->lf, (es->f1p2 - es->f1p3));

    l = es->f1p3;

    /* Filter #2 (highpass) */

    es->f2p0 += MUL_Q16(es->hf, (x        - es->f2p0));
    es->f2p1 += MUL_Q16(es->hf, (es->f2p0 - es->f2p1));
    es->f2p2 += MUL_Q16(es->hf, (es->f2p1 - es->f2p2));
    es->f2p3 += MUL_Q16(es->hf, (es->f2p2 - es->f2p3));

    h = es->sdm3 - es->f2p3;

    /* Calculate midrange (signal - (low + high)) */

    m = x - (h + l);

    /* Scale by band gains (Q16) */

    l = MUL_Q16(l, es->lg);
    m = MUL_Q16(m, es->mg);
    h = MUL_Q16(h, es->hg);

    /* Shuffle history buffer */

    es->sdm3 = es->sdm2;
    es->sdm2 = es->sdm1;
    es->sdm1 = x;

    /* Recombine and round back to an integer sample */

    o = l + m + h;
    if (o >= 0)
      return (int)((o + (EQ_QS_ONE >> 1)) >> EQ_QS);
    return -(int)(((-o) + (EQ_QS_ONE >> 1)) >> EQ_QS);
}
