/* @(#) Convolve an image with a DOUBLEMASK. Image can have any number of bands,
 * @(#) any non-complex type. Output is IM_BANDFMT_FLOAT for all non-complex inputs
 * @(#) except IM_BANDFMT_DOUBLE, which gives IM_BANDFMT_DOUBLE.
 * @(#)
 * @(#) int 
 * @(#) im_convf( in, out, mask )
 * @(#) IMAGE *in, *out;
 * @(#) DOUBLEMASK *mask;
 * @(#)
 * @(#) Returns either 0 (success) or -1 (fail)
 * @(#) 
 *
 * Copyright: 1990, N. Dessipris.
 *
 * Author: Nicos Dessipris & Kirk Martinez
 * Written on: 29/04/1991
 * Modified on: 19/05/1991
 * 8/7/93 JC
 *      - adapted for partial v2
 *      - memory leaks fixed
 *      - ANSIfied
 * 12/7/93 JC
 *	- adapted im_convbi() to im_convbf()
 * 7/10/94 JC
 *	- new IM_ARRAY() macro
 *	- evalend callbacks
 *	- more typedef
 * 9/3/01 JC
 *	- redone from im_conv() 
 * 27/7/01 JC
 *	- rejects masks with scale == 0
 * 7/4/04 
 *	- now uses im_embed() with edge stretching on the input, not
 *	  the output
 *	- sets Xoffset / Yoffset
 * 11/11/05
 * 	- simpler inner loop avoids gcc4 bug 
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>

#include <vips/vips.h>

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif /*WITH_DMALLOC*/

/* Our parameters ... we take a copy of the mask argument, plus we make a
 * smaller version with the zeros squeezed out. 
 */
typedef struct {
	IMAGE *in;
	IMAGE *out;
	DOUBLEMASK *mask;	/* Copy of mask arg */

	int nnz;		/* Number of non-zero mask elements */
	double *coeff;		/* Array of non-zero mask coefficients */
} Conv;

static int
conv_close( Conv *conv )
{
	if( conv->mask ) {
		(void) im_free_dmask( conv->mask );
		conv->mask = NULL;
	}

        return( 0 );
}

static Conv *
conv_new( IMAGE *in, IMAGE *out, DOUBLEMASK *mask )
{
        Conv *conv = IM_NEW( out, Conv );
	const int ne = mask->xsize * mask->ysize;
        int i;

        if( !conv )
                return( NULL );

        conv->in = in;
        conv->out = out;
        conv->mask = NULL;
        conv->nnz = 0;
        conv->coeff = NULL;

        if( im_add_close_callback( out, 
		(im_callback_fn) conv_close, conv, NULL ) ||
        	!(conv->coeff = IM_ARRAY( out, ne, double )) ||
        	!(conv->mask = im_dup_dmask( mask, "conv_mask" )) )
                return( NULL );

        /* Find non-zero mask elements.
         */
        for( i = 0; i < ne; i++ )
                if( mask->coeff[i] ) 
			conv->coeff[conv->nnz++] = mask->coeff[i];

        return( conv );
}

/* Our sequence value.
 */
typedef struct {
	Conv *conv;
	REGION *ir;		/* Input region */

	int *offsets;		/* Offsets for each non-zero matrix element */
	PEL **pts;		/* Per-non-zero mask element image pointers */
} ConvSequence;

/* Free a sequence value.
 */
static int
conv_stop( void *vseq, void *a, void *b )
{
	ConvSequence *seq = (ConvSequence *) vseq;

	IM_FREEF( im_region_free, seq->ir );

	return( 0 );
}

/* Convolution start function.
 */
static void *
conv_start( IMAGE *out, void *a, void *b )
{
	IMAGE *in = (IMAGE *) a;
	Conv *conv = (Conv *) b;
	ConvSequence *seq;

	if( !(seq = IM_NEW( out, ConvSequence )) )
		return( NULL );

	/* Init!
	 */
	seq->conv = conv;
	seq->ir = NULL;
	seq->pts = NULL;

	/* Attach region and arrays.
	 */
	seq->ir = im_region_create( in );
	seq->offsets = IM_ARRAY( out, conv->nnz, int );
	seq->pts = IM_ARRAY( out, conv->nnz, PEL * );
	if( !seq->ir || !seq->offsets || !seq->pts ) {
		conv_stop( seq, in, conv );
		return( NULL );
	}

	return( (void *) seq );
}

#define INNER sum += *t++ * (*p++)[x]

#define CONV_FLOAT( ITYPE, OTYPE ) { \
	OTYPE *q = (OTYPE *) IM_REGION_ADDR( or, le, y ); \
	\
	for( x = 0; x < sz; x++ ) {  \
		double sum = 0; \
		double *t = conv->coeff; \
		ITYPE **p = (ITYPE **) seq->pts; \
 		\
		z = 0; \
		IM_UNROLL( conv->nnz, INNER ); \
 		\
		sum = (sum / mask->scale) + mask->offset; \
 		\
		q[x] = sum;  \
	}  \
} 

/* Convolve!
 */
static int
conv_gen( REGION *or, void *vseq, void *a, void *b )
{
	ConvSequence *seq = (ConvSequence *) vseq;
	IMAGE *in = (IMAGE *) a;
	Conv *conv = (Conv *) b;
	REGION *ir = seq->ir;
	DOUBLEMASK *mask = conv->mask;

	Rect *r = &or->valid;
	Rect s;
	int le = r->left;
	int to = r->top;
	int bo = IM_RECT_BOTTOM(r);
	int sz = IM_REGION_N_ELEMENTS( or );

	int x, y, z, i;

	/* Prepare the section of the input image we need. A little larger
	 * than the section of the output image we are producing.
	 */
	s = *r;
	s.width += mask->xsize - 1;
	s.height += mask->ysize - 1;
	if( im_prepare( ir, &s ) )
		return( -1 );

        /* Fill offset array.
         */
	z = 0;
        for( i = 0, y = 0; y < mask->ysize; y++ )
                for( x = 0; x < mask->xsize; x++, i++ )
                        if( mask->coeff[i] )
                                seq->offsets[z++] = 
					IM_REGION_ADDR( ir, x + le, y + to ) -
                                        IM_REGION_ADDR( ir, le, to );

	for( y = to; y < bo; y++ ) { 
		/* Init pts for this line of PELs.
		 */
                for( z = 0; z < conv->nnz; z++ ) 
                        seq->pts[z] = seq->offsets[z] +  
                                (PEL *) IM_REGION_ADDR( ir, le, y ); 

		switch( in->BandFmt ) {
		case IM_BANDFMT_UCHAR: 	
			CONV_FLOAT( unsigned char, float ); break;
		case IM_BANDFMT_CHAR:   
			CONV_FLOAT( signed char, float ); break;
		case IM_BANDFMT_USHORT: 
			CONV_FLOAT( unsigned short, float ); break;
		case IM_BANDFMT_SHORT:  
			CONV_FLOAT( signed short, float ); break;
		case IM_BANDFMT_UINT:   
			CONV_FLOAT( unsigned int, float ); break;
		case IM_BANDFMT_INT:    
			CONV_FLOAT( signed int, float ); break;
		case IM_BANDFMT_FLOAT:  
			CONV_FLOAT( float, float ); break;
		case IM_BANDFMT_DOUBLE: 
			CONV_FLOAT( double, double ); break;

		default:
			assert( 0 );
		}
	}

	return( 0 );
}

int
im_convf_raw( IMAGE *in, IMAGE *out, DOUBLEMASK *mask )
{
	Conv *conv;

	/* Check parameters.
	 */
	if( !in || in->Coding != IM_CODING_NONE || im_iscomplex( in ) ) {
		im_error( "im_convf", 
			_( "non-complex uncoded only" ) );
		return( -1 );
	}
	if( !mask || mask->xsize > 1000 || mask->ysize > 1000 || 
		mask->xsize <= 0 || mask->ysize <= 0 || !mask->coeff ||
		mask->scale == 0 ) {
		im_error( "im_convf", _( "nonsense mask parameters" ) );
		return( -1 );
	}
	if( im_piocheck( in, out ) )
		return( -1 );
	if( !(conv = conv_new( in, out, mask )) )
		return( -1 );

	/* Prepare output. Consider a 7x7 mask and a 7x7 image --- the output
	 * would be 1x1.
	 */
	if( im_cp_desc( out, in ) )
		return( -1 );
	if( im_isint( in ) ) {
		out->Bbits = IM_BBITS_FLOAT;
		out->BandFmt = IM_BANDFMT_FLOAT;
	}
	out->Xsize -= mask->xsize - 1;
	out->Ysize -= mask->ysize - 1;
	if( out->Xsize <= 0 || out->Ysize <= 0 ) {
		im_error( "im_convf", _( "image too small for mask" ) );
		return( -1 );
	}

	/* Set demand hints. FATSTRIP is good for us, as THINSTRIP will cause
	 * too many recalculations on overlaps.
	 */
	if( im_demand_hint( out, IM_FATSTRIP, in, NULL ) )
		return( -1 );

	if( im_generate( out, conv_start, conv_gen, conv_stop, in, conv ) )
		return( -1 );

	out->Xoffset = -mask->xsize / 2;
	out->Yoffset = -mask->ysize / 2;

	return( 0 );
}

/* The above, with a border to make out the same size as in.
 */
int 
im_convf( IMAGE *in, IMAGE *out, DOUBLEMASK *mask )
{
	IMAGE *t1 = im_open_local( out, "im_convf intermediate", "p" );

	if( !t1 || 
		im_embed( in, t1, 1, mask->xsize / 2, mask->ysize / 2, 
			in->Xsize + mask->xsize - 1, 
			in->Ysize + mask->ysize - 1 ) ||
		im_convf_raw( t1, out, mask ) )
		return( -1 );

	out->Xoffset = 0;
	out->Yoffset = 0;

	return( 0 );
}
