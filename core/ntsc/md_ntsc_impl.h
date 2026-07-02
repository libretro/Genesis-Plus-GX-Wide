/* md_ntsc 0.1.2. http://www.slack.net/~ant/ */

/* Common implementation of NTSC filters */

#include <assert.h>

/* Copyright (C) 2006-2007 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#define DISABLE_CORRECTION 0

#undef PI
#define PI 3.14159265358979323846f

#include "ntsc_fixed.h"

#ifndef LUMA_CUTOFF
  #define LUMA_CUTOFF 0.20
#endif
#ifndef gamma_size
  #define gamma_size 1
#endif
#ifndef rgb_bits
  #define rgb_bits 8
#endif
#ifndef artifacts_max
  #define artifacts_max (artifacts_mid * 1.5f)
#endif
#ifndef fringing_max
  #define fringing_max (fringing_mid * 2)
#endif
#ifndef STD_HUE_CONDITION
  #define STD_HUE_CONDITION( setup ) 1
#endif

#define ext_decoder_hue     (std_decoder_hue + 15)
#define rgb_unit            (1 << rgb_bits)
#define rgb_offset          (rgb_unit * 2 + 0.5f)

enum { burst_size  = md_ntsc_entry_size / burst_count };
enum { kernel_half = 16 };
enum { kernel_size = kernel_half * 2 + 1 };

typedef struct init_t
{
  ntsc_fx to_rgb [burst_count * 6];   /* Q16 */
  ntsc_fx to_float [gamma_size];      /* Q16 */
  ntsc_fx contrast;                   /* Q16 */
  ntsc_fx brightness;                 /* Q16 */
  ntsc_fx artifacts;                  /* Q16 */
  ntsc_fx fringing;                   /* Q16 */
  ntsc_fx kernel [rescale_out * kernel_size * 2]; /* Q16 */
} init_t;

#define ROTATE_IQ( i, q, sin_b, cos_b ) {\
  ntsc_fx t;\
  t = ((i) * (cos_b) - (q) * (sin_b)) >> NTSC_FXB;\
  q = ((i) * (sin_b) + (q) * (cos_b)) >> NTSC_FXB;\
  i = t;\
}

static void init_filters( init_t* impl, md_ntsc_setup_t const* setup )
{
#if rescale_out > 1
  ntsc_fx kernels [kernel_size * 2];
#else
  ntsc_fx* const kernels = impl->kernel;
#endif

  /* generate luma (y) filter using sinc kernel */
  {
    ntsc_fx const rolloff = NTSC_FXONE + ((NTSC_F(setup->sharpness) * NTSC_F(0.032)) >> NTSC_FXB);
    ntsc_fx const pow_a_n = ntsc_powi( rolloff, 32 );
    ntsc_fx sum;
    int i;
    /* quadratic mapping to reduce negative (blurring) range */
    ntsc_fx res1 = NTSC_F(setup->resolution) + NTSC_FXONE;
    ntsc_fx to_angle = ((((res1 * res1) >> NTSC_FXB) + NTSC_FXONE) * NTSC_F(PI / 32.0 * LUMA_CUTOFF)) >> NTSC_FXB;

    kernels [kernel_size * 3 / 2] = NTSC_F(32.0); /* default center value */
    for ( i = 0; i < kernel_half * 2 + 1; i++ )
    {
      int x = i - kernel_half;
      ntsc_fx angle = (ntsc_fx) x * to_angle;
      if ( x || pow_a_n > NTSC_F(1.056) || pow_a_n < NTSC_F(0.981) )
      {
        ntsc_fx rca = (rolloff * ntsc_cos( angle )) >> NTSC_FXB;
        ntsc_fx num = NTSC_FXONE - rca
            - ((pow_a_n * ntsc_cos( (ntsc_fx)32 * angle )) >> NTSC_FXB)
            + (((( pow_a_n * rolloff ) >> NTSC_FXB) * ntsc_cos( (ntsc_fx)31 * angle )) >> NTSC_FXB);
        ntsc_fx den = NTSC_FXONE - rca - rca + ((rolloff * rolloff) >> NTSC_FXB);
        ntsc_fx dsf = (num << NTSC_FXB) / den;
        kernels [kernel_size * 3 / 2 - kernel_half + i] = dsf - (NTSC_FXONE >> 1);
      }
    }

    /* apply blackman window and find sum */
    sum = 0;
    for ( i = 0; i < kernel_half * 2 + 1; i++ )
    {
      ntsc_fx x = NTSC_F(PI * 2 / (kernel_half * 2)) * i;
      ntsc_fx blackman = NTSC_F(0.42) - ((NTSC_F(0.5) * ntsc_cos( x )) >> NTSC_FXB)
                       + ((NTSC_F(0.08) * ntsc_cos( x * 2 )) >> NTSC_FXB);
      {
        int idx = kernel_size * 3 / 2 - kernel_half + i;
        kernels [idx] = (kernels [idx] * blackman) >> NTSC_FXB;
        sum += kernels [idx];
      }
    }

    /* normalize kernel */
    {
      ntsc_fx inv = (NTSC_FXONE << NTSC_FXB) / sum;
      for ( i = 0; i < kernel_half * 2 + 1; i++ )
      {
        int x = kernel_size * 3 / 2 - kernel_half + i;
        kernels [x] = (kernels [x] * inv) >> NTSC_FXB;
      }
    }
  }

  /* generate chroma (iq) filter using gaussian kernel */
  {
    ntsc_fx const cutoff_factor = NTSC_F(-0.03125);
    ntsc_fx cutoff = NTSC_F(setup->bleed);
    int i;

    if ( cutoff < 0 )
    {
      /* keep extreme value accessible only near upper end of scale (1.0) */
      cutoff = (cutoff * cutoff) >> NTSC_FXB;
      cutoff = (cutoff * cutoff) >> NTSC_FXB;
      cutoff = (cutoff * cutoff) >> NTSC_FXB;
      cutoff = -((cutoff * NTSC_F(30.0 / 0.65)) >> NTSC_FXB);
    }
    cutoff = cutoff_factor - ((((NTSC_F(0.65) * cutoff_factor) >> NTSC_FXB) * cutoff) >> NTSC_FXB);

    for ( i = -kernel_half; i <= kernel_half; i++ )
      kernels [kernel_size / 2 + i] = ntsc_exp_neg( (ntsc_fx)(i * i) * cutoff );

    /* normalize even and odd phases separately */
    for ( i = 0; i < 2; i++ )
    {
      ntsc_fx sum = 0;
      int x;
      for ( x = i; x < kernel_size; x += 2 )
        sum += kernels [x];

      {
        ntsc_fx inv = (NTSC_FXONE << NTSC_FXB) / sum;
        for ( x = i; x < kernel_size; x += 2 )
          kernels [x] = (kernels [x] * inv) >> NTSC_FXB;
      }
    }
  }

  /* generate linear rescale kernels */
  #if rescale_out > 1
  {
    ntsc_fx weight = NTSC_FXONE;
    ntsc_fx* out = impl->kernel;
    int n = rescale_out;
    do
    {
      ntsc_fx remain = 0;
      int i;
      weight -= NTSC_FXONE / rescale_in;
      for ( i = 0; i < kernel_size * 2; i++ )
      {
        ntsc_fx cur = kernels [i];
        ntsc_fx m = (cur * weight) >> NTSC_FXB;
        *out++ = m + remain;
        remain = cur - m;
      }
    }
    while ( --n );
  }
  #endif
}

static ntsc_fx const default_decoder [6] =
  { NTSC_F(0.956), NTSC_F(0.621), NTSC_F(-0.272), NTSC_F(-0.647), NTSC_F(-1.105), NTSC_F(1.702) };

static void init( init_t* impl, md_ntsc_setup_t const* setup )
{
  impl->brightness = NTSC_F(setup->brightness) * (rgb_unit / 2) + NTSC_F(rgb_unit * 2 + 0.5);
  impl->contrast   = NTSC_F(setup->contrast)   * (rgb_unit / 2) + NTSC_F(rgb_unit);
  #ifdef default_palette_contrast
    if ( !setup->palette )
      impl->contrast = (impl->contrast * NTSC_F(default_palette_contrast)) >> NTSC_FXB;
  #endif

  impl->artifacts = NTSC_F(setup->artifacts);
  if ( impl->artifacts > 0 )
    impl->artifacts = (impl->artifacts * NTSC_F(artifacts_max - artifacts_mid)) >> NTSC_FXB;
  impl->artifacts = ((impl->artifacts * NTSC_F(artifacts_mid)) >> NTSC_FXB) + NTSC_F(artifacts_mid);

  impl->fringing = NTSC_F(setup->fringing);
  if ( impl->fringing > 0 )
    impl->fringing = (impl->fringing * NTSC_F(fringing_max - fringing_mid)) >> NTSC_FXB;
  impl->fringing = ((impl->fringing * NTSC_F(fringing_mid)) >> NTSC_FXB) + NTSC_F(fringing_mid);

  init_filters( impl, setup );

  /* generate gamma table */
  if ( gamma_size > 1 )
  {
    ntsc_fx const gamma = NTSC_F(1.1333) - (NTSC_F(setup->gamma) >> 1);
    /* match common PC's 2.2 gamma to TV's 2.65 gamma */
    int i;
    for ( i = 0; i < gamma_size; i++ )
    {
      ntsc_fx base = ((ntsc_fx) i << NTSC_FXB) / (gamma_size - 1);
      impl->to_float [i] =
          ((ntsc_pow( base, gamma ) * impl->contrast) >> NTSC_FXB) + impl->brightness;
    }
  }

  /* setup decoder matricies */
  {
    ntsc_fx hue = ((NTSC_F(setup->hue) * NTSC_F(PI)) >> NTSC_FXB) + NTSC_F(PI / 180 * ext_decoder_hue);
    ntsc_fx sat = NTSC_F(setup->saturation) + NTSC_FXONE;
    ntsc_fx dec_buf [6];
    ntsc_fx const* decoder;
    if ( setup->decoder_matrix )
    {
      int z;
      for ( z = 0; z < 6; z++ )
        dec_buf [z] = NTSC_F(setup->decoder_matrix [z]);
      decoder = dec_buf;
    }
    else
    {
      decoder = default_decoder;
      if ( STD_HUE_CONDITION( setup ) )
        hue += NTSC_F(PI / 180 * (std_decoder_hue - ext_decoder_hue));
    }

    {
      ntsc_fx s = (ntsc_sin( hue ) * sat) >> NTSC_FXB;
      ntsc_fx c = (ntsc_cos( hue ) * sat) >> NTSC_FXB;
      ntsc_fx* out = impl->to_rgb;
      int nb;

      nb = burst_count;
      do
      {
        ntsc_fx const* in = decoder;
        int n = 3;
        do
        {
          ntsc_fx iv = *in++;
          ntsc_fx qv = *in++;
          *out++ = ((iv * c) >> NTSC_FXB) - ((qv * s) >> NTSC_FXB);
          *out++ = ((iv * s) >> NTSC_FXB) + ((qv * c) >> NTSC_FXB);
        }
        while ( --n );
        if ( burst_count <= 1 )
          break;
        ROTATE_IQ( s, c, NTSC_F(0.866025), NTSC_F(-0.5) ); /* +120 degrees */
      }
      while ( --nb );
    }
  }
}

/* kernel generation */

#define RGB_TO_YIQ( r, g, b, y, i ) (\
  (y = ((r) * NTSC_C_RY + (g) * NTSC_C_GY + (b) * NTSC_C_BY) >> NTSC_FXB),\
  (i = ((r) * NTSC_C_RI - (g) * NTSC_C_GI - (b) * NTSC_C_BI) >> NTSC_FXB),\
  (((r) * NTSC_C_RQ - (g) * NTSC_C_GQ + (b) * NTSC_C_BQ) >> NTSC_FXB)\
)

#define YIQ_TO_RGB( y, i, q, to_rgb, type, r, g ) (\
  r = (type) ntsc_fxint( (y) + (((to_rgb) [0] * (i)) >> NTSC_FXB) + (((to_rgb) [1] * (q)) >> NTSC_FXB) ),\
  g = (type) ntsc_fxint( (y) + (((to_rgb) [2] * (i)) >> NTSC_FXB) + (((to_rgb) [3] * (q)) >> NTSC_FXB) ),\
  (type) ntsc_fxint( (y) + (((to_rgb) [4] * (i)) >> NTSC_FXB) + (((to_rgb) [5] * (q)) >> NTSC_FXB) )\
)

#define PACK_RGB( r, g, b ) ((r) << 21 | (g) << 11 | (b) << 1)

enum { rgb_kernel_size = burst_size / alignment_count };
enum { rgb_bias = rgb_unit * 2 * md_ntsc_rgb_builder };

typedef struct pixel_info_t
{
  int offset;
  ntsc_fx negate;
  ntsc_fx kernel [4];
} pixel_info_t;

#if rescale_in > 1
  #define PIXEL_OFFSET_( ntsc, scaled ) \
    (kernel_size / 2 + ntsc + (scaled != 0) + (rescale_out - scaled) % rescale_out + \
        (kernel_size * 2 * scaled))

  #define PIXEL_OFFSET( ntsc, scaled ) \
    PIXEL_OFFSET_( ((ntsc) - (scaled) / rescale_out * rescale_in),\
        (((scaled) + rescale_out * 10) % rescale_out) ),\
    NTSC_F(1.0 - (((ntsc) + 100) & 2))
#else
  #define PIXEL_OFFSET( ntsc, scaled ) \
    (kernel_size / 2 + (ntsc) - (scaled)),\
    NTSC_F(1.0 - (((ntsc) + 100) & 2))
#endif

extern pixel_info_t const md_ntsc_pixels [alignment_count];

/* Generate pixel at all burst phases and column alignments */
static void gen_kernel( init_t* impl, ntsc_fx y, ntsc_fx i, ntsc_fx q, md_ntsc_rgb_t* out )
{
  /* generate for each scanline burst phase */
  ntsc_fx const* to_rgb = impl->to_rgb;
  int burst_remain = burst_count;
  y -= NTSC_F(rgb_offset);
  do
  {
    /* Encode yiq into *two* composite signals (to allow control over artifacting).
    Convolve these with kernels which: filter respective components, apply
    sharpening, and rescale horizontally. Convert resulting yiq to rgb and pack
    into integer. Based on algorithm by NewRisingSun. */
    pixel_info_t const* pixel = md_ntsc_pixels;
    int alignment_remain = alignment_count;
    do
    {
      /* negate is -1 when composite starts at odd multiple of 2 */
      ntsc_fx const yy = (((y * impl->fringing) >> NTSC_FXB) * pixel->negate) >> NTSC_FXB;
      ntsc_fx const ic0 = ((i + yy) * pixel->kernel [0]) >> NTSC_FXB;
      ntsc_fx const qc1 = ((q + yy) * pixel->kernel [1]) >> NTSC_FXB;
      ntsc_fx const ic2 = ((i - yy) * pixel->kernel [2]) >> NTSC_FXB;
      ntsc_fx const qc3 = ((q - yy) * pixel->kernel [3]) >> NTSC_FXB;

      ntsc_fx const factor = (impl->artifacts * pixel->negate) >> NTSC_FXB;
      ntsc_fx const ii = (i * factor) >> NTSC_FXB;
      ntsc_fx const yc0 = ((y + ii) * pixel->kernel [0]) >> NTSC_FXB;
      ntsc_fx const yc2 = ((y - ii) * pixel->kernel [2]) >> NTSC_FXB;

      ntsc_fx const qq = (q * factor) >> NTSC_FXB;
      ntsc_fx const yc1 = ((y + qq) * pixel->kernel [1]) >> NTSC_FXB;
      ntsc_fx const yc3 = ((y - qq) * pixel->kernel [3]) >> NTSC_FXB;

      ntsc_fx const* k = &impl->kernel [pixel->offset];
      int n;
      ++pixel;
      for ( n = rgb_kernel_size; n; --n )
      {
        ntsc_fx fi = ((k[0] * ic0) >> NTSC_FXB) + ((k[2] * ic2) >> NTSC_FXB);
        ntsc_fx fq = ((k[1] * qc1) >> NTSC_FXB) + ((k[3] * qc3) >> NTSC_FXB);
        ntsc_fx fy = ((k[kernel_size+0] * yc0) >> NTSC_FXB) + ((k[kernel_size+1] * yc1) >> NTSC_FXB) +
                     ((k[kernel_size+2] * yc2) >> NTSC_FXB) + ((k[kernel_size+3] * yc3) >> NTSC_FXB) + NTSC_F(rgb_offset);
        if ( rescale_out <= 1 )
          k--;
        else if ( k < &impl->kernel [kernel_size * 2 * (rescale_out - 1)] )
          k += kernel_size * 2 - 1;
        else
          k -= kernel_size * 2 * (rescale_out - 1) + 2;
        {
          int r, g, b = YIQ_TO_RGB( fy, fi, fq, to_rgb, int, r, g );
          *out++ = PACK_RGB( r, g, b ) - rgb_bias;
        }
      }
    }
    while ( alignment_count > 1 && --alignment_remain );

    if ( burst_count <= 1 )
      break;

    to_rgb += 6;

    ROTATE_IQ( i, q, NTSC_F(-0.866025), NTSC_F(-0.5) ); /* -120 degrees */
  }
  while ( --burst_remain );
}

static void correct_errors( md_ntsc_rgb_t color, md_ntsc_rgb_t* out );

#if DISABLE_CORRECTION
  #define CORRECT_ERROR( a ) { out [i] += rgb_bias; }
  #define DISTRIBUTE_ERROR( a, b, c ) { out [i] += rgb_bias; }
#else
  #define CORRECT_ERROR( a ) { out [a] += error; }
  #define DISTRIBUTE_ERROR( a, b, c ) {\
    md_ntsc_rgb_t fourth = (error + 2 * md_ntsc_rgb_builder) >> 2;\
    fourth &= (rgb_bias >> 1) - md_ntsc_rgb_builder;\
    fourth -= rgb_bias >> 2;\
    out [a] += fourth;\
    out [b] += fourth;\
    out [c] += fourth;\
    out [i] += error - (fourth * 3);\
  }
#endif

#define RGB_PALETTE_OUT( rgb, out_ )\
{\
  unsigned char* out = (out_);\
  md_ntsc_rgb_t clamped = (rgb);\
  MD_NTSC_CLAMP_( clamped, (8 - rgb_bits) );\
  out [0] = (unsigned char) (clamped >> 21);\
  out [1] = (unsigned char) (clamped >> 11);\
  out [2] = (unsigned char) (clamped >>  1);\
}

/* blitter related */

#ifndef restrict
  #if defined (__GNUC__)
    #define restrict __restrict__
  #elif defined (_MSC_VER) && _MSC_VER > 1300
    #define restrict
  #else
    /* no support for restricted pointers */
    #define restrict
  #endif
#endif

#include <limits.h>

#if MD_NTSC_OUT_DEPTH <= 16
  #if USHRT_MAX == 0xFFFF
    typedef unsigned short md_ntsc_out_t;
  #else
    #error "Need 16-bit int type"
  #endif

#else
  #if UINT_MAX == 0xFFFFFFFF
    typedef unsigned int  md_ntsc_out_t;
  #elif ULONG_MAX == 0xFFFFFFFF
    typedef unsigned long md_ntsc_out_t;
  #else
    #error "Need 32-bit int type"
  #endif

#endif
