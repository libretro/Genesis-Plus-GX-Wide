/***************************************************************************************
 *  Genesis Plus GX
 *  FM bus enhancement (opt-in, fixed-point, FM-only)
 *
 *  An optional, deliberately non-authentic post-processor applied ONLY to the
 *  FM (YM2612/YM3438) bus. It runs at the output sample rate on a band-limited
 *  FM-only stream (a dedicated Blip Buffer), so it is independent of the FM
 *  core's internal sample rate and is mixed back into the final output.
 *
 *  Three integer-only stages, in this order:
 *    1. stereo widener   (mid/side, widens the hard-panned FM image)
 *    2. reverb send       (compact fixed-point Schroeder/Freeverb-style room)
 *    3. analog stage      (gentle one-pole low-pass + cubic soft-saturation;
 *                          also absorbs the peak energy added by 1 and 2)
 *
 *  Everything is fixed-point (Q15 gains, integer delay lines), so output is
 *  bit-exact across platforms and the feature does not introduce float into the
 *  deterministic audio path. State is audio-only (never feeds emulation) and is
 *  intentionally not serialized into savestates (it self-heals in a few ms).
 *
 *  Levels: 0 = Off, 1 = Light, 2 = Rich.
 ***************************************************************************************/

#ifndef _FM_ENHANCE_H_
#define _FM_ENHANCE_H_

/* Size the delay lines once for the given output sample rate, and clear all
   state. Safe to call again on a sample-rate change. */
extern void fm_enhance_init(int sample_rate);

/* Select the enhancement strength (0 = Off, 1 = Light, 2 = Rich). Only swaps
   the gain set; does not resize buffers, so it is cheap to call on an option
   change. Clears tails when toggling to/from Off. */
extern void fm_enhance_set_level(int level);

/* Clear reverb/filter history (call on audio reset). */
extern void fm_enhance_reset(void);

/* Process one block of interleaved stereo 16-bit FM samples in place.
   No-op when the level is Off. */
extern void fm_enhance_block(short *buf, int frames);

#endif /* _FM_ENHANCE_H_ */
