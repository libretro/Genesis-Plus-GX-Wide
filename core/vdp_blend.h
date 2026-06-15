/***************************************************************************************
 *  Genesis Plus GX
 *  SIMD pixel-blend kernels for the line remap stage (vdp_render.c)
 *
 *  These accelerate the two *arithmetic* per-pixel blend paths used when
 *  converting the internal line buffer to the output framebuffer:
 *
 *    - LCD image-persistence (ghosting) filter   [config.lcd]
 *    - widescreen extra-column translucency       [config.h40_extra_columns_alpha]
 *
 *  The palette lookup (pixel[*src]) is a data-dependent gather and stays
 *  scalar (SSE2/NEON have no useful gather here); only the R/G/B channel
 *  arithmetic is vectorised, 8 pixels at a time. The scalar fallback is
 *  bit-for-bit identical to the RENDER_PIXEL_LCD / RENDER_PIXEL_LOWER_ALPHA
 *  macros, and the SIMD paths have been verified bit-exact against it.
 *
 *  Only the 16bpp (RGB565) and 15bpp (RGB555/ABGR1555) output formats used
 *  by the libretro build are accelerated; every other configuration (8bpp,
 *  32bpp, or no SIMD ISA) transparently uses the scalar path.
 *
 *  SIMD is selected from the compiler's own predefined macros (__SSE2__,
 *  __ARM_NEON / __ARM_NEON__), so no build-system changes are required: the
 *  per-platform arch flags already present in the Makefile enable it. The
 *  single NEON path covers both ARMv7 (-mfpu=neon) and AArch64.
 ***************************************************************************************/

#ifndef _VDP_BLEND_H_
#define _VDP_BLEND_H_

/* Decide whether a vectorised path is available for the active pixel format.
 * Only the packed 16-bit formats (565 / 555) are handled in SIMD. */
#if (defined(USE_16BPP_RENDERING) || defined(USE_15BPP_RENDERING))
  #if defined(__SSE2__)
    #define VDP_BLEND_SSE2 1
    #include <emmintrin.h>
  #elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    #define VDP_BLEND_NEON 1
    #include <arm_neon.h>
  #endif
#endif

/* Per-format channel field geometry (matches PIXEL/GET_* in vdp_render.h). */
#if defined(USE_16BPP_RENDERING)
  /* RGB565: R[15:11] G[10:5] B[4:0] */
  #define VDP_BLEND_RMASK 0xf800
  #define VDP_BLEND_RSH   11
  #define VDP_BLEND_GMASK 0x07e0
  #define VDP_BLEND_GSH   5
  #define VDP_BLEND_BMASK 0x001f
  #define VDP_BLEND_BSH   0
  #define VDP_BLEND_HIBIT 0x0000
#elif defined(USE_15BPP_RENDERING)
  #if defined(USE_ABGR)
    /* ABGR1555: A[15] B[14:10] G[9:5] R[4:0] -- field positions only;
       the blend is symmetric per channel so naming does not matter, we
       just preserve each 5-bit field and the top bit. */
    #define VDP_BLEND_RMASK 0x7c00
    #define VDP_BLEND_RSH   10
    #define VDP_BLEND_GMASK 0x03e0
    #define VDP_BLEND_GSH   5
    #define VDP_BLEND_BMASK 0x001f
    #define VDP_BLEND_BSH   0
  #else
    /* RGB1555: 1 R[14:10] G[9:5] B[4:0] */
    #define VDP_BLEND_RMASK 0x7c00
    #define VDP_BLEND_RSH   10
    #define VDP_BLEND_GMASK 0x03e0
    #define VDP_BLEND_GSH   5
    #define VDP_BLEND_BMASK 0x001f
    #define VDP_BLEND_BSH   0
  #endif
  #define VDP_BLEND_HIBIT 0x8000
#endif

/* x / 100 via reciprocal multiply, exact for x in [0, 6300] (the maximum
 * channel(63) * alpha(100) product). (x*5243)>>19 == x/100. Implemented as
 * mulhi by 5243 then >>3 so it fits in 16-bit lanes. */
#define VDP_BLEND_DIV100_MAGIC 5243

/* Right-aligned channels have a shift of 0 (e.g. blue in 565 / RGB555).
 * NEON's vshrq_n_u16 / vshlq_n_u16 require an immediate in 1..16 and reject
 * 0 at compile time, so guard every by-shift on its (compile-time constant)
 * amount. SSE shift-by-0 is legal but is guarded too for symmetry. */
#if defined(VDP_BLEND_SSE2)
  #if VDP_BLEND_RSH
    #define VDP_SHR_R(v) _mm_srli_epi16((v), VDP_BLEND_RSH)
    #define VDP_SHL_R(v) _mm_slli_epi16((v), VDP_BLEND_RSH)
  #else
    #define VDP_SHR_R(v) (v)
    #define VDP_SHL_R(v) (v)
  #endif
  #if VDP_BLEND_GSH
    #define VDP_SHR_G(v) _mm_srli_epi16((v), VDP_BLEND_GSH)
    #define VDP_SHL_G(v) _mm_slli_epi16((v), VDP_BLEND_GSH)
  #else
    #define VDP_SHR_G(v) (v)
    #define VDP_SHL_G(v) (v)
  #endif
  #if VDP_BLEND_BSH
    #define VDP_SHR_B(v) _mm_srli_epi16((v), VDP_BLEND_BSH)
    #define VDP_SHL_B(v) _mm_slli_epi16((v), VDP_BLEND_BSH)
  #else
    #define VDP_SHR_B(v) (v)
    #define VDP_SHL_B(v) (v)
  #endif
#elif defined(VDP_BLEND_NEON)
  #if VDP_BLEND_RSH
    #define VDP_SHR_R(v) vshrq_n_u16((v), VDP_BLEND_RSH)
    #define VDP_SHL_R(v) vshlq_n_u16((v), VDP_BLEND_RSH)
  #else
    #define VDP_SHR_R(v) (v)
    #define VDP_SHL_R(v) (v)
  #endif
  #if VDP_BLEND_GSH
    #define VDP_SHR_G(v) vshrq_n_u16((v), VDP_BLEND_GSH)
    #define VDP_SHL_G(v) vshlq_n_u16((v), VDP_BLEND_GSH)
  #else
    #define VDP_SHR_G(v) (v)
    #define VDP_SHL_G(v) (v)
  #endif
  #if VDP_BLEND_BSH
    #define VDP_SHR_B(v) vshrq_n_u16((v), VDP_BLEND_BSH)
    #define VDP_SHL_B(v) vshlq_n_u16((v), VDP_BLEND_BSH)
  #else
    #define VDP_SHR_B(v) (v)
    #define VDP_SHL_B(v) (v)
  #endif
#endif

/* ------------------------------------------------------------------------- *
 *  LCD ghosting blend over a full line.
 *  For each pixel: new = pixel[*src]; for each channel, if (old-new) > 0
 *  then new += (rate * (old-new)) >> 8. 'rate' is a 0.8 fixed-point value.
 * ------------------------------------------------------------------------- */
INLINE void vdp_blend_lcd(uint8 *src, PIXEL_OUT_T *dst,
                                 PIXEL_OUT_T *pixel, int rate, int width)
{
#if defined(VDP_BLEND_SSE2)
  const __m128i mR = _mm_set1_epi16((short)VDP_BLEND_RMASK);
  const __m128i mG = _mm_set1_epi16((short)VDP_BLEND_GMASK);
  const __m128i mB = _mm_set1_epi16((short)VDP_BLEND_BMASK);
  const __m128i vr = _mm_set1_epi16((short)rate);
  const __m128i z  = _mm_setzero_si128();
  while (width >= 8)
  {
    PIXEL_OUT_T t[8];
    __m128i np, op, nr, ng, nb, orr, og, ob, rd, gd, bd, res;
    int k;
    for (k = 0; k < 8; k++) t[k] = pixel[src[k]];
    np  = _mm_loadu_si128((const __m128i *)t);
    op  = _mm_loadu_si128((const __m128i *)dst);
    nr  = VDP_SHR_R(_mm_and_si128(np, mR));
    ng  = VDP_SHR_G(_mm_and_si128(np, mG));
    nb  = VDP_SHR_B(_mm_and_si128(np, mB));
    orr = VDP_SHR_R(_mm_and_si128(op, mR));
    og  = VDP_SHR_G(_mm_and_si128(op, mG));
    ob  = VDP_SHR_B(_mm_and_si128(op, mB));
    /* decay clamped to >= 0 implements the scalar 'if (decay > 0)' */
    rd  = _mm_max_epi16(_mm_sub_epi16(orr, nr), z);
    gd  = _mm_max_epi16(_mm_sub_epi16(og,  ng), z);
    bd  = _mm_max_epi16(_mm_sub_epi16(ob,  nb), z);
    nr  = _mm_add_epi16(nr, _mm_srli_epi16(_mm_mullo_epi16(vr, rd), 8));
    ng  = _mm_add_epi16(ng, _mm_srli_epi16(_mm_mullo_epi16(vr, gd), 8));
    nb  = _mm_add_epi16(nb, _mm_srli_epi16(_mm_mullo_epi16(vr, bd), 8));
    res = _mm_or_si128(_mm_set1_epi16((short)VDP_BLEND_HIBIT),
            _mm_or_si128(VDP_SHL_R(nr),
              _mm_or_si128(VDP_SHL_G(ng),
                           VDP_SHL_B(nb))));
    _mm_storeu_si128((__m128i *)dst, res);
    src += 8; dst += 8; width -= 8;
  }
#elif defined(VDP_BLEND_NEON)
  const uint16x8_t mR = vdupq_n_u16(VDP_BLEND_RMASK);
  const uint16x8_t mG = vdupq_n_u16(VDP_BLEND_GMASK);
  const uint16x8_t mB = vdupq_n_u16(VDP_BLEND_BMASK);
  const uint16x8_t vr = vdupq_n_u16((uint16)rate);
  const uint16x8_t hi = vdupq_n_u16(VDP_BLEND_HIBIT);
  while (width >= 8)
  {
    PIXEL_OUT_T t[8];
    uint16x8_t np, op, nr, ng, nb, orr, og, ob, rd, gd, bd, res;
    int k;
    for (k = 0; k < 8; k++) t[k] = pixel[src[k]];
    np  = vld1q_u16(t);
    op  = vld1q_u16(dst);
    nr  = VDP_SHR_R(vandq_u16(np, mR));
    ng  = VDP_SHR_G(vandq_u16(np, mG));
    nb  = VDP_SHR_B(vandq_u16(np, mB));
    orr = VDP_SHR_R(vandq_u16(op, mR));
    og  = VDP_SHR_G(vandq_u16(op, mG));
    ob  = VDP_SHR_B(vandq_u16(op, mB));
    /* saturating subtract gives max(old-new,0) for the >0 test */
    rd  = vqsubq_u16(orr, nr);
    gd  = vqsubq_u16(og,  ng);
    bd  = vqsubq_u16(ob,  nb);
    nr  = vaddq_u16(nr, vshrq_n_u16(vmulq_u16(vr, rd), 8));
    ng  = vaddq_u16(ng, vshrq_n_u16(vmulq_u16(vr, gd), 8));
    nb  = vaddq_u16(nb, vshrq_n_u16(vmulq_u16(vr, bd), 8));
    res = vorrq_u16(hi,
            vorrq_u16(VDP_SHL_R(nr),
              vorrq_u16(VDP_SHL_G(ng),
                        VDP_SHL_B(nb))));
    vst1q_u16(dst, res);
    src += 8; dst += 8; width -= 8;
  }
#endif
  /* scalar remainder / fallback (also the whole loop when no SIMD) */
  while (width--)
  {
    RENDER_PIXEL_LCD(src, dst, pixel, rate);
  }
}

/* ------------------------------------------------------------------------- *
 *  Widescreen extra-column translucency over a contiguous run.
 *  For each pixel: new = pixel[*src]; channel = (channel * rate) / 100.
 * ------------------------------------------------------------------------- */
INLINE void vdp_blend_alpha(uint8 *src, PIXEL_OUT_T *dst,
                                   PIXEL_OUT_T *pixel, int rate, int width)
{
#if defined(VDP_BLEND_SSE2)
  const __m128i mR    = _mm_set1_epi16((short)VDP_BLEND_RMASK);
  const __m128i mG    = _mm_set1_epi16((short)VDP_BLEND_GMASK);
  const __m128i mB    = _mm_set1_epi16((short)VDP_BLEND_BMASK);
  const __m128i vr    = _mm_set1_epi16((short)rate);
  const __m128i magic = _mm_set1_epi16((short)VDP_BLEND_DIV100_MAGIC);
  while (width >= 8)
  {
    PIXEL_OUT_T t[8];
    __m128i np, r, g, b, res;
    int k;
    for (k = 0; k < 8; k++) t[k] = pixel[src[k]];
    np  = _mm_loadu_si128((const __m128i *)t);
    r   = VDP_SHR_R(_mm_and_si128(np, mR));
    g   = VDP_SHR_G(_mm_and_si128(np, mG));
    b   = VDP_SHR_B(_mm_and_si128(np, mB));
    /* (chan * rate) / 100, exact via mulhi(.,5243)>>3 */
    r   = _mm_srli_epi16(_mm_mulhi_epu16(_mm_mullo_epi16(r, vr), magic), 3);
    g   = _mm_srli_epi16(_mm_mulhi_epu16(_mm_mullo_epi16(g, vr), magic), 3);
    b   = _mm_srli_epi16(_mm_mulhi_epu16(_mm_mullo_epi16(b, vr), magic), 3);
    res = _mm_or_si128(_mm_set1_epi16((short)VDP_BLEND_HIBIT),
            _mm_or_si128(VDP_SHL_R(r),
              _mm_or_si128(VDP_SHL_G(g),
                           VDP_SHL_B(b))));
    _mm_storeu_si128((__m128i *)dst, res);
    src += 8; dst += 8; width -= 8;
  }
#elif defined(VDP_BLEND_NEON)
  const uint16x8_t mR    = vdupq_n_u16(VDP_BLEND_RMASK);
  const uint16x8_t mG    = vdupq_n_u16(VDP_BLEND_GMASK);
  const uint16x8_t mB    = vdupq_n_u16(VDP_BLEND_BMASK);
  const uint16x8_t vr    = vdupq_n_u16((uint16)rate);
  const uint16x8_t magic = vdupq_n_u16(VDP_BLEND_DIV100_MAGIC);
  const uint16x8_t hi    = vdupq_n_u16(VDP_BLEND_HIBIT);
  while (width >= 8)
  {
    PIXEL_OUT_T t[8];
    uint16x8_t np, r, g, b, res;
    int k;
    for (k = 0; k < 8; k++) t[k] = pixel[src[k]];
    np  = vld1q_u16(t);
    /* (chan * rate) / 100; computed as ((prod * 5243) >> 16) >> 3 using a
       16x16->32 widening multiply (NEON has no single hi-16 multiply). */
    {
      uint32x4_t lo, hi32;
      uint16x8_t pr = vmulq_u16(VDP_SHR_R(vandq_u16(np, mR)), vr);
      uint16x8_t pg = vmulq_u16(VDP_SHR_G(vandq_u16(np, mG)), vr);
      uint16x8_t pb = vmulq_u16(VDP_SHR_B(vandq_u16(np, mB)), vr);
      lo   = vmull_u16(vget_low_u16(pr),  vget_low_u16(magic));
      hi32 = vmull_u16(vget_high_u16(pr), vget_high_u16(magic));
      r    = vshrq_n_u16(vcombine_u16(vshrn_n_u32(lo, 16), vshrn_n_u32(hi32, 16)), 3);
      lo   = vmull_u16(vget_low_u16(pg),  vget_low_u16(magic));
      hi32 = vmull_u16(vget_high_u16(pg), vget_high_u16(magic));
      g    = vshrq_n_u16(vcombine_u16(vshrn_n_u32(lo, 16), vshrn_n_u32(hi32, 16)), 3);
      lo   = vmull_u16(vget_low_u16(pb),  vget_low_u16(magic));
      hi32 = vmull_u16(vget_high_u16(pb), vget_high_u16(magic));
      b    = vshrq_n_u16(vcombine_u16(vshrn_n_u32(lo, 16), vshrn_n_u32(hi32, 16)), 3);
    }
    res = vorrq_u16(hi,
            vorrq_u16(VDP_SHL_R(r),
              vorrq_u16(VDP_SHL_G(g),
                        VDP_SHL_B(b))));
    vst1q_u16(dst, res);
    src += 8; dst += 8; width -= 8;
  }
#endif
  /* scalar remainder / fallback */
  while (width--)
  {
    RENDER_PIXEL_LOWER_ALPHA(src, dst, pixel, rate);
  }
}

#endif /* _VDP_BLEND_H_ */
