/*---------------------------------------------------------------------------
//
//                                3 Band EQ :)
//
// EQ.H - Header file for 3 band EQ
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

/* Fixed-point port: the filter no longer uses double/sin(). Coefficients and
// gains are Q16, the pole/history state is Q24, and the crossover coefficient
// 2*sin(pi*f/fs) is evaluated with an integer polynomial (see eq.c). This keeps
// the audio path free of floating point (deterministic, no libm); output tracks
// the previous double implementation to within ~1 LSB. */

#ifndef __EQ3BAND__
#define __EQ3BAND__

/* signed 64-bit intermediate (this directory already relies on long long in
// blip_buf.c; MSVC provides __int64) */
#if defined(_MSC_VER)
typedef __int64 eq_int64;
#else
typedef long long eq_int64;
#endif

/* ------------
//| Structures |
// ------------*/

typedef struct {
    /* Filter #1 (Low band) -- Q16 coefficient, Q24 pole state */

    eq_int64 lf;      /* Frequency (2*sin(pi*lowfreq/mixfreq), Q16) */
    eq_int64 f1p0;      /* Poles ... */
    eq_int64 f1p1;
    eq_int64 f1p2;
    eq_int64 f1p3;

    /* Filter #2 (High band) */

    eq_int64 hf;      /* Frequency (Q16) */
    eq_int64 f2p0;      /* Poles ... */
    eq_int64 f2p1;
    eq_int64 f2p2;
    eq_int64 f2p3;

    /* Sample history buffer (Q24) */

    eq_int64 sdm1;      /* Sample data minus 1 */
    eq_int64 sdm2;      /*                   2 */
    eq_int64 sdm3;      /*                   3 */

    /* Gain Controls (Q16, 100% == unity) */

    eq_int64 lg;      /* low  gain */
    eq_int64 mg;      /* mid  gain */
    eq_int64 hg;      /* high gain */

} EQSTATE;


/* ---------
//| Exports |
// ---------*/

extern void init_3band_state(EQSTATE * es, int lowfreq, int highfreq,
           int mixfreq, int lg, int mg, int hg);
extern int do_3band(EQSTATE * es, int sample);


#endif        /* #ifndef __EQ3BAND__ */
