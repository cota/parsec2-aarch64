/*
  Copyright 1999-2007 ImageMagick Studio LLC, a non-profit organization
  dedicated to making software imaging solutions freely available.

  You may not use this file except in compliance with the License.
  obtain a copy of the License at

    http://www.imagemagick.org/script/license.php

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  MagickCore quantum inline methods.
*/
#ifndef _MAGICKCORE_QUANTUM_PRIVATE_H
#define _MAGICKCORE_QUANTUM_PRIVATE_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

typedef struct _QuantumState
{
  EndianType
    endian;

  double
    minimum,
    scale;

  unsigned long
    pixel,
    bits;

  const unsigned long
    *mask;
} QuantumState;

static inline void InitializeQuantumState(const QuantumInfo *quantum_info,
  const EndianType endian,QuantumState *quantum_state)
{
  static const unsigned long mask[32] =
  {
    0x00000000UL, 0x00000001UL, 0x00000003UL, 0x00000007UL, 0x0000000fUL,
    0x0000001fUL, 0x0000003fUL, 0x0000007fUL, 0x000000ffUL, 0x000001ffUL,
    0x000003ffUL, 0x000007ffUL, 0x00000fffUL, 0x00001fffUL, 0x00003fffUL,
    0x00007fffUL, 0x0000ffffUL, 0x0001ffffUL, 0x0003ffffUL, 0x0007ffffUL,
    0x000fffffUL, 0x001fffffUL, 0x003fffffUL, 0x007fffffUL, 0x00ffffffUL,
    0x01ffffffUL, 0x03ffffffUL, 0x07ffffffUL, 0x0fffffffUL, 0x1fffffffUL,
    0x3fffffffUL, 0x7fffffffUL
  };

  (void) ResetMagickMemory(quantum_state,0,sizeof(&quantum_state));
  quantum_state->endian=endian;
  quantum_state->minimum=quantum_info->minimum;
  quantum_state->scale=quantum_info->scale;
  quantum_state->bits=0;
  quantum_state->mask=mask;
}

static inline void PopCharPixel(const unsigned char pixel,
  unsigned char **pixels)
{
  *(*pixels)++=(unsigned char) (pixel);
}

static inline void PopDoublePixel(const QuantumState *quantum_state,
  const double pixel,unsigned char **pixels)
{
  unsigned char
    quantum[8];

  *((double *) quantum)=(double) (pixel*quantum_state->scale+
    quantum_state->minimum); 
  if (quantum_state->endian != LSBEndian)
    {
      *(*pixels)++=quantum[7];
      *(*pixels)++=quantum[6];
      *(*pixels)++=quantum[5];
      *(*pixels)++=quantum[4];
      *(*pixels)++=quantum[3];
      *(*pixels)++=quantum[2];
      *(*pixels)++=quantum[1];
      *(*pixels)++=quantum[0];
      return;
    }
  *(*pixels)++=quantum[0];
  *(*pixels)++=quantum[1];
  *(*pixels)++=quantum[2];
  *(*pixels)++=quantum[3];
  *(*pixels)++=quantum[4];
  *(*pixels)++=quantum[5];
  *(*pixels)++=quantum[6];
  *(*pixels)++=quantum[7];
}

static inline void PopFloatPixel(const QuantumState *quantum_state,
  const float pixel,unsigned char **pixels)
{
  unsigned char
    quantum[4];

  *((float *) quantum)=(float) ((double) pixel*quantum_state->scale+
    quantum_state->minimum); 
  if (quantum_state->endian != LSBEndian)
    {
      *(*pixels)++=quantum[3];
      *(*pixels)++=quantum[2];
      *(*pixels)++=quantum[1];
      *(*pixels)++=quantum[0];
      return;
    }
  *(*pixels)++=quantum[0];
  *(*pixels)++=quantum[1];
  *(*pixels)++=quantum[2];
  *(*pixels)++=quantum[3];
}

static inline void PopLongPixel(const QuantumState *quantum_state,
  const unsigned long pixel,unsigned char **pixels)
{
  if (quantum_state->endian != LSBEndian)
    {
      *(*pixels)++=(unsigned char) ((pixel) >> 24);
      *(*pixels)++=(unsigned char) ((pixel) >> 16);
      *(*pixels)++=(unsigned char) ((pixel) >> 8);
      *(*pixels)++=(unsigned char) (pixel);
      return;
    }
  *(*pixels)++=(unsigned char) (pixel);
  *(*pixels)++=(unsigned char) ((pixel) >> 8);
  *(*pixels)++=(unsigned char) ((pixel) >> 16);
  *(*pixels)++=(unsigned char) ((pixel) >> 24);
}

static inline void PopQuantumPixel(QuantumState *quantum_state,
  const unsigned long depth,const unsigned long pixel,unsigned char **pixels)
{
  register long
    i;

  register unsigned long
    quantum_bits;

  if (quantum_state->bits == 0UL)
    quantum_state->bits=8UL;
  for (i=(long) depth; i > 0L; )
  {
    quantum_bits=(unsigned long) i;
    if (quantum_bits > quantum_state->bits)
      quantum_bits=quantum_state->bits;
    i-=quantum_bits;
    if (quantum_state->bits == 8)
      *(*pixels)='\0';
    quantum_state->bits-=quantum_bits;
    *(*pixels)|=(((pixel >> i) &~ ((~0UL) << quantum_bits)) <<
      quantum_state->bits);
    if (quantum_state->bits == 0UL)
      {
        (*pixels)++;
        quantum_state->bits=8UL;
      }
  }
}

static void PopQuantumLongPixel(QuantumState *quantum_state,
  const unsigned long depth,const unsigned long pixel,unsigned char **pixels)
{
  register long
    i;

  unsigned long
    quantum_bits;

  if (quantum_state->bits == 0UL)
    quantum_state->bits=32UL;
  for (i=(long) depth; i > 0; )
  {
    quantum_bits=(unsigned long) i;
    if (quantum_bits > quantum_state->bits)
      quantum_bits=quantum_state->bits;
    quantum_state->pixel|=(((pixel >> (depth-i)) &
      quantum_state->mask[quantum_bits]) << (32UL-quantum_state->bits));
    i-=quantum_bits;
    quantum_state->bits-=quantum_bits;
    if (quantum_state->bits == 0U)
      {
        PopLongPixel(quantum_state,quantum_state->pixel,pixels);
        quantum_state->pixel=0U;
        quantum_state->bits=32UL;
      }
  }
}

static inline void PopShortPixel(const QuantumState *quantum_state,
  const unsigned short pixel,unsigned char **pixels)
{
  if (quantum_state->endian != LSBEndian)
    {
      *(*pixels)++=(unsigned char) ((pixel) >> 8);
      *(*pixels)++=(unsigned char) (pixel);
      return;
    }
  *(*pixels)++=(unsigned char) (pixel);
  *(*pixels)++=(unsigned char) ((pixel) >> 8);
}

static inline unsigned char PushCharPixel(const unsigned char **pixels)
{
  unsigned char
    pixel;

  pixel=(unsigned char) *(*pixels)++;
  return(pixel);
}

static inline IndexPacket PushColormapIndex(Image *image,
  const unsigned long index)
{
  assert(image != (Image *) NULL);
  assert(image->signature == MagickSignature);
  if (index < image->colors)
    return((IndexPacket) index);
  (void) ThrowMagickException(&image->exception,GetMagickModule(),
    CorruptImageError,"InvalidColormapIndex","`%s'",image->filename);
  return((IndexPacket) 0);
}

static inline double PushDoublePixel(const QuantumState *quantum_state,
  const unsigned char **pixels)
{
  double
    pixel;

  unsigned char
    quantum[8];

  if (quantum_state->endian != LSBEndian)
    {
      quantum[7]=(*(*pixels)++);
      quantum[6]=(*(*pixels)++);
      quantum[5]=(*(*pixels)++);
      quantum[5]=(*(*pixels)++);
      quantum[3]=(*(*pixels)++);
      quantum[2]=(*(*pixels)++);
      quantum[1]=(*(*pixels)++);
      quantum[0]=(*(*pixels)++);
      pixel=(*((double *) quantum));
      pixel-=quantum_state->minimum;
      pixel*=quantum_state->scale;
      return(pixel);
    }
  quantum[0]=(*(*pixels)++);
  quantum[1]=(*(*pixels)++);
  quantum[2]=(*(*pixels)++);
  quantum[3]=(*(*pixels)++);
  quantum[4]=(*(*pixels)++);
  quantum[5]=(*(*pixels)++);
  quantum[6]=(*(*pixels)++);
  quantum[7]=(*(*pixels)++);
  pixel=(*((double *) quantum));
  pixel-=quantum_state->minimum;
  pixel*=quantum_state->scale;
  return(pixel);
}

static inline float PushFloatPixel(const QuantumState *quantum_state,
  const unsigned char **pixels)
{
  float
    pixel;

  unsigned char
    quantum[4];

  if (quantum_state->endian != LSBEndian)
    {
      quantum[3]=(*(*pixels)++);
      quantum[2]=(*(*pixels)++);
      quantum[1]=(*(*pixels)++);
      quantum[0]=(*(*pixels)++);
      pixel=(*((float *) quantum));
      pixel-=quantum_state->minimum;
      pixel*=quantum_state->scale;
      return(pixel);
    }
  quantum[0]=(*(*pixels)++);
  quantum[1]=(*(*pixels)++);
  quantum[2]=(*(*pixels)++);
  quantum[3]=(*(*pixels)++);
  pixel=(*((float *) quantum));
  pixel-=quantum_state->minimum;
  pixel*=quantum_state->scale;
  return(pixel);
}

static inline unsigned long PushLongPixel(const QuantumState *quantum_state,
  const unsigned char **pixels)
{
  unsigned long
    pixel;

  if (quantum_state->endian != LSBEndian)
    {
      pixel=(unsigned long) (*(*pixels)++ << 24);
      pixel|=(unsigned long) (*(*pixels)++ << 16);
      pixel|=(unsigned long) (*(*pixels)++ << 8);
      pixel|=(unsigned long) (*(*pixels)++);
      return(pixel);
    }
  pixel=(unsigned long) (*(*pixels)++);
  pixel|=(unsigned long) (*(*pixels)++ << 8);
  pixel|=(unsigned long) (*(*pixels)++ << 16);
  pixel|=(unsigned long) (*(*pixels)++ << 24);
  return(pixel);
}

static inline unsigned long PushQuantumPixel(QuantumState *quantum_state,
  const unsigned long depth,const unsigned char **pixels)
{
  register long
    i;

  register unsigned long
    quantum_bits,
    quantum;

  quantum=0UL;
  for (i=(long) depth; i > 0L; )
  {
    if (quantum_state->bits == 0UL)
      {
        quantum_state->pixel=(*(*pixels)++);
        quantum_state->bits=8UL;
      }
    quantum_bits=(unsigned long) i;
    if (quantum_bits > quantum_state->bits)
      quantum_bits=quantum_state->bits;
    i-=quantum_bits;
    quantum_state->bits-=quantum_bits;
    quantum=(quantum << quantum_bits) | ((quantum_state->pixel >>
      quantum_state->bits) &~ ((~0UL) << quantum_bits));
  }
  return(quantum);
}

static unsigned long PushQuantumLongPixel(QuantumState *quantum_state,
  const unsigned long depth,const unsigned char **pixels)
{
  register long
    i;

  register unsigned long
    quantum,
    quantum_bits;
      
  quantum=0UL;
  for (i=(long) depth; i > 0; )
  {
    if (quantum_state->bits == 0)
      {
        quantum_state->pixel=PushLongPixel(quantum_state,pixels);
        quantum_state->bits=32UL;
      }
    quantum_bits=(unsigned long) i;
    if (quantum_bits > quantum_state->bits)
      quantum_bits=quantum_state->bits;
    quantum|=(((quantum_state->pixel >> (32UL-quantum_state->bits)) &
      quantum_state->mask[quantum_bits]) << (depth-i));
    i-=quantum_bits;
    quantum_state->bits-=quantum_bits;
  }
  return(quantum);
}

static inline unsigned short PushShortPixel(const QuantumState *quantum_state,
  const unsigned char **pixels)
{
  unsigned short
    pixel;

  if (quantum_state->endian != LSBEndian)
    {
      pixel=(unsigned short) (*(*pixels)++ << 8);
      pixel|=(unsigned short) (*(*pixels)++);
      return(pixel);
    }
  pixel=(unsigned short) (*(*pixels)++);
  pixel|=(unsigned short) (*(*pixels)++ << 8);
  return(pixel);
}

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
