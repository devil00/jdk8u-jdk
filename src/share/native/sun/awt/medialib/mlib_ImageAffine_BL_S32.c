/*
 * Copyright 2003 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Sun designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Sun in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */


/*
 * FUNCTION
 *   Internal functions for mlib_ImageAffine with bilinear filtering.
 */

#include "mlib_ImageAffine.h"

/***************************************************************/
#define DTYPE  mlib_s32
#define FTYPE  mlib_d64
/* handle FTYPE to DTYPE conversion like in mlib_ImageAffine_BC_S32.c */
#define STORE(res, x)  SAT_32(res, x)

#define FUN_NAME(CHAN) mlib_ImageAffine_s32_##CHAN##_bl

/***************************************************************/
mlib_status FUN_NAME(1ch)(mlib_affine_param *param)
{
  DECLAREVAR_BL();
  DTYPE *dstLineEnd;
  FTYPE scale = ONE / MLIB_PREC;
  mlib_s32 srcYStride1;

  srcYStride /= sizeof(DTYPE);
  srcYStride1 = srcYStride + 1;

  for (j = yStart; j <= yFinish; j++) {
    FTYPE t, u, k0, k1, k2, k3;
    FTYPE a00_0, a01_0, a10_0, a11_0;
    FTYPE pix0;

    CLIP(1);
    dstLineEnd = (DTYPE *) dstData + xRight;

    t = (X & MLIB_MASK) * scale;
    u = (Y & MLIB_MASK) * scale;
    ySrc = MLIB_POINTER_SHIFT(Y);
    Y += dY;
    xSrc = X >> MLIB_SHIFT;
    X += dX;
    srcPixelPtr = MLIB_POINTER_GET(lineAddr, ySrc) + xSrc;
    k3 = t * u;
    k2 = (ONE - t) * u;
    k1 = t * (ONE - u);
    k0 = (ONE - t) * (ONE - u);
    a00_0 = srcPixelPtr[0];
    a01_0 = srcPixelPtr[1];
    a10_0 = srcPixelPtr[srcYStride];
    a11_0 = srcPixelPtr[srcYStride1];

    for (; dstPixelPtr < dstLineEnd; dstPixelPtr++) {
      pix0 = k0 * a00_0 + k1 * a01_0 + k2 * a10_0 + k3 * a11_0;
      t = (X & MLIB_MASK) * scale;
      u = (Y & MLIB_MASK) * scale;
      ySrc = MLIB_POINTER_SHIFT(Y);
      Y += dY;
      xSrc = X >> MLIB_SHIFT;
      X += dX;
      srcPixelPtr = *(DTYPE **) ((mlib_u8 *) lineAddr + ySrc) + xSrc;
      k3 = t * u;
      k2 = (ONE - t) * u;
      k1 = t * (ONE - u);
      k0 = (ONE - t) * (ONE - u);
      a00_0 = srcPixelPtr[0];
      a01_0 = srcPixelPtr[1];
      a10_0 = srcPixelPtr[srcYStride];
      a11_0 = srcPixelPtr[srcYStride1];
      STORE(dstPixelPtr[0], pix0);
    }

    pix0 = k0 * a00_0 + k1 * a01_0 + k2 * a10_0 + k3 * a11_0;
    STORE(dstPixelPtr[0], pix0);
  }

  return MLIB_SUCCESS;
}

/***************************************************************/
mlib_status FUN_NAME(2ch)(mlib_affine_param *param)
{
  DECLAREVAR_BL();
  DTYPE *dstLineEnd;
  FTYPE scale = ONE / MLIB_PREC;

  for (j = yStart; j <= yFinish; j++) {
    DTYPE *srcPixelPtr2;
    FTYPE t, u, k0, k1, k2, k3;
    FTYPE a00_0, a01_0, a10_0, a11_0;
    FTYPE a00_1, a01_1, a10_1, a11_1;
    FTYPE pix0, pix1;

    CLIP(2);
    dstLineEnd = (DTYPE *) dstData + 2 * xRight;

    t = (X & MLIB_MASK) * scale;
    u = (Y & MLIB_MASK) * scale;
    ySrc = MLIB_POINTER_SHIFT(Y);
    Y += dY;
    xSrc = X >> MLIB_SHIFT;
    X += dX;
    srcPixelPtr = MLIB_POINTER_GET(lineAddr, ySrc) + 2 * xSrc;
    srcPixelPtr2 = (DTYPE *) ((mlib_u8 *) srcPixelPtr + srcYStride);
    k3 = t * u;
    k2 = (ONE - t) * u;
    k1 = t * (ONE - u);
    k0 = (ONE - t) * (ONE - u);
    a00_0 = srcPixelPtr[0];
    a00_1 = srcPixelPtr[1];
    a01_0 = srcPixelPtr[2];
    a01_1 = srcPixelPtr[3];
    a10_0 = srcPixelPtr2[0];
    a10_1 = srcPixelPtr2[1];
    a11_0 = srcPixelPtr2[2];
    a11_1 = srcPixelPtr2[3];

    for (; dstPixelPtr < dstLineEnd; dstPixelPtr += 2) {
      pix0 = k0 * a00_0 + k1 * a01_0 + k2 * a10_0 + k3 * a11_0;
      pix1 = k0 * a00_1 + k1 * a01_1 + k2 * a10_1 + k3 * a11_1;
      t = (X & MLIB_MASK) * scale;
      u = (Y & MLIB_MASK) * scale;
      ySrc = MLIB_POINTER_SHIFT(Y);
      Y += dY;
      xSrc = X >> MLIB_SHIFT;
      X += dX;
      srcPixelPtr = MLIB_POINTER_GET(lineAddr, ySrc) + 2 * xSrc;
      srcPixelPtr2 = (DTYPE *) ((mlib_u8 *) srcPixelPtr + srcYStride);
      k3 = t * u;
      k2 = (ONE - t) * u;
      k1 = t * (ONE - u);
      k0 = (ONE - t) * (ONE - u);
      a01_0 = srcPixelPtr[2];
      a01_1 = srcPixelPtr[3];
      a00_0 = srcPixelPtr[0];
      a00_1 = srcPixelPtr[1];
      a10_0 = srcPixelPtr2[0];
      a10_1 = srcPixelPtr2[1];
      a11_0 = srcPixelPtr2[2];
      a11_1 = srcPixelPtr2[3];
      STORE(dstPixelPtr[0], pix0);
      STORE(dstPixelPtr[1], pix1);
    }

    pix0 = k0 * a00_0 + k1 * a01_0 + k2 * a10_0 + k3 * a11_0;
    pix1 = k0 * a00_1 + k1 * a01_1 + k2 * a10_1 + k3 * a11_1;
    STORE(dstPixelPtr[0], pix0);
    STORE(dstPixelPtr[1], pix1);
  }

  return MLIB_SUCCESS;
}

/***************************************************************/
mlib_status FUN_NAME(3ch)(mlib_affine_param *param)
{
  DECLAREVAR_BL();
  DTYPE *dstLineEnd;
  FTYPE scale = ONE / MLIB_PREC;

  for (j = yStart; j <= yFinish; j++) {
    DTYPE *srcPixelPtr2;
    FTYPE t, u, k0, k1, k2, k3;
    FTYPE a00_0, a01_0, a10_0, a11_0;
    FTYPE a00_1, a01_1, a10_1, a11_1;
    FTYPE a00_2, a01_2, a10_2, a11_2;
    FTYPE pix0, pix1, pix2;

    CLIP(3);
    dstLineEnd = (DTYPE *) dstData + 3 * xRight;

    t = (X & MLIB_MASK) * scale;
    u = (Y & MLIB_MASK) * scale;
    ySrc = MLIB_POINTER_SHIFT(Y);
    Y += dY;
    xSrc = X >> MLIB_SHIFT;
    X += dX;
    srcPixelPtr = MLIB_POINTER_GET(lineAddr, ySrc) + 3 * xSrc;
    srcPixelPtr2 = (DTYPE *) ((mlib_u8 *) srcPixelPtr + srcYStride);
    k3 = t * u;
    k2 = (ONE - t) * u;
    k1 = t * (ONE - u);
    k0 = (ONE - t) * (ONE - u);
    a00_0 = srcPixelPtr[0];
    a00_1 = srcPixelPtr[1];
    a00_2 = srcPixelPtr[2];
    a01_0 = srcPixelPtr[3];
    a01_1 = srcPixelPtr[4];
    a01_2 = srcPixelPtr[5];
    a10_0 = srcPixelPtr2[0];
    a10_1 = srcPixelPtr2[1];
    a10_2 = srcPixelPtr2[2];
    a11_0 = srcPixelPtr2[3];
    a11_1 = srcPixelPtr2[4];
    a11_2 = srcPixelPtr2[5];

    for (; dstPixelPtr < dstLineEnd; dstPixelPtr += 3) {
      pix0 = k0 * a00_0 + k1 * a01_0 + k2 * a10_0 + k3 * a11_0;
      pix1 = k0 * a00_1 + k1 * a01_1 + k2 * a10_1 + k3 * a11_1;
      pix2 = k0 * a00_2 + k1 * a01_2 + k2 * a10_2 + k3 * a11_2;
      t = (X & MLIB_MASK) * scale;
      u = (Y & MLIB_MASK) * scale;
      ySrc = MLIB_POINTER_SHIFT(Y);
      Y += dY;
      xSrc = X >> MLIB_SHIFT;
      X += dX;
      srcPixelPtr = MLIB_POINTER_GET(lineAddr, ySrc) + 3 * xSrc;
      srcPixelPtr2 = (DTYPE *) ((mlib_u8 *) srcPixelPtr + srcYStride);
      k3 = t * u;
      k2 = (ONE - t) * u;
      k1 = t * (ONE - u);
      k0 = (ONE - t) * (ONE - u);
      a01_0 = srcPixelPtr[3];
      a01_1 = srcPixelPtr[4];
      a01_2 = srcPixelPtr[5];
      a00_0 = srcPixelPtr[0];
      a00_1 = srcPixelPtr[1];
      a00_2 = srcPixelPtr[2];
      a10_0 = srcPixelPtr2[0];
      a10_1 = srcPixelPtr2[1];
      a10_2 = srcPixelPtr2[2];
      a11_0 = srcPixelPtr2[3];
      a11_1 = srcPixelPtr2[4];
      a11_2 = srcPixelPtr2[5];
      STORE(dstPixelPtr[0], pix0);
      STORE(dstPixelPtr[1], pix1);
      STORE(dstPixelPtr[2], pix2);
    }

    pix0 = k0 * a00_0 + k1 * a01_0 + k2 * a10_0 + k3 * a11_0;
    pix1 = k0 * a00_1 + k1 * a01_1 + k2 * a10_1 + k3 * a11_1;
    pix2 = k0 * a00_2 + k1 * a01_2 + k2 * a10_2 + k3 * a11_2;
    STORE(dstPixelPtr[0], pix0);
    STORE(dstPixelPtr[1], pix1);
    STORE(dstPixelPtr[2], pix2);
  }

  return MLIB_SUCCESS;
}

/***************************************************************/
mlib_status FUN_NAME(4ch)(mlib_affine_param *param)
{
  DECLAREVAR_BL();
  DTYPE *dstLineEnd;
  FTYPE scale = ONE / MLIB_PREC;

  for (j = yStart; j <= yFinish; j++) {
    DTYPE *srcPixelPtr2;
    FTYPE t, u, k0, k1, k2, k3;
    FTYPE a00_0, a01_0, a10_0, a11_0;
    FTYPE a00_1, a01_1, a10_1, a11_1;
    FTYPE a00_2, a01_2, a10_2, a11_2;
    FTYPE a00_3, a01_3, a10_3, a11_3;
    FTYPE pix0, pix1, pix2, pix3;

    CLIP(4);
    dstLineEnd = (DTYPE *) dstData + 4 * xRight;

    t = (X & MLIB_MASK) * scale;
    u = (Y & MLIB_MASK) * scale;
    ySrc = MLIB_POINTER_SHIFT(Y);
    Y += dY;
    xSrc = X >> MLIB_SHIFT;
    X += dX;
    srcPixelPtr = MLIB_POINTER_GET(lineAddr, ySrc) + 4 * xSrc;
    srcPixelPtr2 = (DTYPE *) ((mlib_u8 *) srcPixelPtr + srcYStride);
    k3 = t * u;
    k2 = (ONE - t) * u;
    k1 = t * (ONE - u);
    k0 = (ONE - t) * (ONE - u);
    a00_0 = srcPixelPtr[0];
    a00_1 = srcPixelPtr[1];
    a00_2 = srcPixelPtr[2];
    a00_3 = srcPixelPtr[3];
    a01_0 = srcPixelPtr[4];
    a01_1 = srcPixelPtr[5];
    a01_2 = srcPixelPtr[6];
    a01_3 = srcPixelPtr[7];
    a10_0 = srcPixelPtr2[0];
    a10_1 = srcPixelPtr2[1];
    a10_2 = srcPixelPtr2[2];
    a10_3 = srcPixelPtr2[3];
    a11_0 = srcPixelPtr2[4];
    a11_1 = srcPixelPtr2[5];
    a11_2 = srcPixelPtr2[6];
    a11_3 = srcPixelPtr2[7];

    for (; dstPixelPtr < dstLineEnd; dstPixelPtr += 4) {
      pix0 = k0 * a00_0 + k1 * a01_0 + k2 * a10_0 + k3 * a11_0;
      pix1 = k0 * a00_1 + k1 * a01_1 + k2 * a10_1 + k3 * a11_1;
      pix2 = k0 * a00_2 + k1 * a01_2 + k2 * a10_2 + k3 * a11_2;
      pix3 = k0 * a00_3 + k1 * a01_3 + k2 * a10_3 + k3 * a11_3;
      t = (X & MLIB_MASK) * scale;
      u = (Y & MLIB_MASK) * scale;
      ySrc = MLIB_POINTER_SHIFT(Y);
      Y += dY;
      xSrc = X >> (MLIB_SHIFT - 2);
      X += dX;
      srcPixelPtr = MLIB_POINTER_GET(lineAddr, ySrc) + (xSrc & ~3);
      srcPixelPtr2 = (DTYPE *) ((mlib_u8 *) srcPixelPtr + srcYStride);
      k3 = t * u;
      k2 = (ONE - t) * u;
      k1 = t * (ONE - u);
      k0 = (ONE - t) * (ONE - u);
      a00_3 = srcPixelPtr[3];
      a01_3 = srcPixelPtr[7];
      a10_3 = srcPixelPtr2[3];
      a11_3 = srcPixelPtr2[7];
      a00_0 = srcPixelPtr[0];
      a00_1 = srcPixelPtr[1];
      a00_2 = srcPixelPtr[2];
      a01_0 = srcPixelPtr[4];
      a01_1 = srcPixelPtr[5];
      a01_2 = srcPixelPtr[6];
      a10_0 = srcPixelPtr2[0];
      a10_1 = srcPixelPtr2[1];
      a10_2 = srcPixelPtr2[2];
      a11_0 = srcPixelPtr2[4];
      a11_1 = srcPixelPtr2[5];
      a11_2 = srcPixelPtr2[6];
      STORE(dstPixelPtr[0], pix0);
      STORE(dstPixelPtr[1], pix1);
      STORE(dstPixelPtr[2], pix2);
      STORE(dstPixelPtr[3], pix3);
    }

    pix0 = k0 * a00_0 + k1 * a01_0 + k2 * a10_0 + k3 * a11_0;
    pix1 = k0 * a00_1 + k1 * a01_1 + k2 * a10_1 + k3 * a11_1;
    pix2 = k0 * a00_2 + k1 * a01_2 + k2 * a10_2 + k3 * a11_2;
    pix3 = k0 * a00_3 + k1 * a01_3 + k2 * a10_3 + k3 * a11_3;
    STORE(dstPixelPtr[0], pix0);
    STORE(dstPixelPtr[1], pix1);
    STORE(dstPixelPtr[2], pix2);
    STORE(dstPixelPtr[3], pix3);
  }

  return MLIB_SUCCESS;
}

/***************************************************************/
