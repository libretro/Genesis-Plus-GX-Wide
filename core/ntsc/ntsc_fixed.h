/* Fixed-point helpers for the NTSC video filter (md_ntsc / sms_ntsc).
 *
 * The filter's kernel tables were generated once per setup change with
 * sin()/cos()/pow()/exp() and float arithmetic. Genesis Plus GX targets many
 * platforms and this file lets the generation run entirely in integer math:
 * Q16 external values, a signed 64-bit intermediate, and polynomial
 * approximations of the transcendentals (validated against libm to <=2e-4 over
 * the ranges the generator uses). Output is not bit-identical to the former
 * float path but tracks it to within ~1 code level after 8-bit quantization.
 *
 * MSVC C89: uses __int64; block-top declarations; C-style comments. */

#ifndef NTSC_FIXED_H
#define NTSC_FIXED_H

#if defined(_MSC_VER)
typedef __int64 ntsc_fx;
#else
typedef long long ntsc_fx;
#endif

#define NTSC_FXB   16                        /* external fractional bits (Q16) */
#define NTSC_FXONE ((ntsc_fx)1 << NTSC_FXB)
#define NTSC_Q     30                        /* internal precision */
#define NTSC_Q1    ((ntsc_fx)1 << NTSC_Q)

/* Q30 constants */
#define NTSC_PI_Q   ((ntsc_fx)3373259426)    /* pi   * 2^30 */
#define NTSC_2PI_Q  ((ntsc_fx)6746518852)    /* 2pi  * 2^30 */
#define NTSC_HPI_Q  ((ntsc_fx)1686629713)    /* pi/2 * 2^30 */
#define NTSC_LN2_Q  ((ntsc_fx)744261118)     /* ln2  * 2^30 */

/* Q30 * Q30 -> Q30 (operands are kept small enough to fit signed 64-bit) */
#define NTSC_MULQ(a, b) (((a) * (b)) >> NTSC_Q)

/* convert a double literal/config value to Q16. Constant arguments fold at
   compile time; only the handful of setup-field reads convert at runtime. */
#define NTSC_F(d) ((ntsc_fx)((d) * 65536.0 + ((d) < 0 ? -0.5 : 0.5)))

/* cos(theta), theta in Q16 radians, result Q16 in [-1,1] */
static ntsc_fx ntsc_cos(ntsc_fx a16)
{
  ntsc_fx x = a16 << (NTSC_Q - NTSC_FXB);
  ntsc_fx x2, t;
  int s = 1;
  x %= NTSC_2PI_Q; if (x < 0) x += NTSC_2PI_Q;
  if (x > NTSC_PI_Q) x = NTSC_2PI_Q - x;
  if (x > NTSC_HPI_Q) { x = NTSC_PI_Q - x; s = -1; }
  x2 = NTSC_MULQ(x, x);
  /* 1 - x2*(1/2 - x2*(1/24 - x2*(1/720 - x2/40320))) */
  t = NTSC_Q1 / 40320;
  t = NTSC_Q1 / 720 - NTSC_MULQ(x2, t);
  t = NTSC_Q1 / 24  - NTSC_MULQ(x2, t);
  t = NTSC_Q1 / 2   - NTSC_MULQ(x2, t);
  t = NTSC_Q1       - NTSC_MULQ(x2, t);
  return s > 0 ? (t >> (NTSC_Q - NTSC_FXB)) : -(t >> (NTSC_Q - NTSC_FXB));
}

static ntsc_fx ntsc_sin(ntsc_fx a16)
{
  return ntsc_cos(a16 - (NTSC_HPI_Q >> (NTSC_Q - NTSC_FXB)));
}

/* exp(x) for x <= 0, result Q16 >= 0 */
static ntsc_fx ntsc_exp_neg(ntsc_fx x16)
{
  ntsc_fx x = x16 << (NTSC_Q - NTSC_FXB);
  ntsc_fx f, t;
  int n = 0;
  if (x > 0) x = 0;
  if (x < -((ntsc_fx)24 << NTSC_Q)) return 0;
  while (x <= -NTSC_LN2_Q) { x += NTSC_LN2_Q; n++; }
  f = -x;
  /* exp(-f), f in [0,ln2) */
  t = NTSC_Q1 - f / 6;
  t = NTSC_Q1 - NTSC_MULQ(f / 5, t);
  t = NTSC_Q1 - NTSC_MULQ(f / 4, t);
  t = NTSC_Q1 - NTSC_MULQ(f / 3, t);
  t = NTSC_Q1 - NTSC_MULQ(f / 2, t);
  t = NTSC_Q1 - NTSC_MULQ(f, t);
  return (t >> n) >> (NTSC_Q - NTSC_FXB);
}

/* ln(b) for b > 0, result Q16 */
static ntsc_fx ntsc_ln(ntsc_fx b16)
{
  ntsc_fx x = b16 << (NTSC_Q - NTSC_FXB);
  ntsc_fx y, y2, t, num, den;
  int k = 0;
  if (x <= 0) return -((ntsc_fx)40 << NTSC_FXB);
  while (x < NTSC_Q1)        { x <<= 1; k++; }
  while (x >= (NTSC_Q1 << 1)){ x >>= 1; k--; }
  num = x - NTSC_Q1; den = x + NTSC_Q1;
  y = (num << NTSC_Q) / den;         /* |y| < 1/3 */
  y2 = NTSC_MULQ(y, y);
  t = NTSC_Q1 / 9;
  t = NTSC_Q1 / 7 + NTSC_MULQ(y2, t);
  t = NTSC_Q1 / 5 + NTSC_MULQ(y2, t);
  t = NTSC_Q1 / 3 + NTSC_MULQ(y2, t);
  t = NTSC_Q1     + NTSC_MULQ(y2, t);
  t = NTSC_MULQ(y, t) << 1;
  t -= (ntsc_fx)k * NTSC_LN2_Q;
  return t >> (NTSC_Q - NTSC_FXB);
}

/* base^exp, base in (0,1] domain (exp*ln(base) <= 0), result Q16 */
static ntsc_fx ntsc_pow(ntsc_fx base16, ntsc_fx exp16)
{
  ntsc_fx l, e;
  if (base16 <= 0) return 0;
  l = ntsc_ln(base16);
  e = (exp16 * l) >> NTSC_FXB;
  return e > 0 ? 0 : ntsc_exp_neg(e);
}

/* base^n, integer n, result Q16 (Q30 internal for accuracy) */
static ntsc_fx ntsc_powi(ntsc_fx base16, int n)
{
  ntsc_fx r = NTSC_Q1, b = base16 << (NTSC_Q - NTSC_FXB);
  while (n) { if (n & 1) r = NTSC_MULQ(r, b); b = NTSC_MULQ(b, b); n >>= 1; }
  return r >> (NTSC_Q - NTSC_FXB);
}


/* truncate a Q16 value toward zero to an integer (matches (int)float cast) */
static int ntsc_fxint(ntsc_fx v)
{
  return (int)(v >= 0 ? (v >> NTSC_FXB) : -((-v) >> NTSC_FXB));
}

/* RGB<->YIQ matrix coefficients in Q16 (round(coeff * 65536)) */
#define NTSC_C_RY  19595  /* 0.299 */
#define NTSC_C_GY  38470  /* 0.587 */
#define NTSC_C_BY   7471  /* 0.114 */
#define NTSC_C_RI  39059  /* 0.596 */
#define NTSC_C_GI  18022  /* 0.275 */
#define NTSC_C_BI  21037  /* 0.321 */
#define NTSC_C_RQ  13894  /* 0.212 */
#define NTSC_C_GQ  34275  /* 0.523 */
#define NTSC_C_BQ  20382  /* 0.311 */

#endif /* NTSC_FIXED_H */
