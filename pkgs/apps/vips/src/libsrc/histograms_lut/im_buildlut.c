/* @(#) Build a LUT from a set of x/y points. Eg. if input is
 * @(#) 
 * @(#)   12  100
 * @(#)   14  110
 * @(#)   18  120
 * @(#) 
 * @(#) we generate
 * @(#) 
 * @(#)   100	(12)
 * @(#)   105
 * @(#)   110
 * @(#)   112.5
 * @(#)   115
 * @(#)   117.5
 * @(#)   120	(18)
 * @(#)
 * @(#) The x/y points don't need to be sorted: we do that. You can have 
 * @(#) several Ys ... each becomes a band in the output LUT. 
 *
 * Written on: 26/9/06
 * 	- from im_invertlut()
 * 9/10/06
 * 	- don't output x values
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
#include <math.h>

#include <vips/vips.h>

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif /*WITH_DMALLOC*/

/*
#define DEBUG
 */

/* Our state.
 */
typedef struct _State {
	DOUBLEMASK *input;	/* Input mask */
	int lut_size;		/* Number of output elements to generate */
	double **data;		/* Rows of unpacked matrix */
} State;

/* Use this to sort our input rows by the first column.
 */
static int
compare( const void *a, const void *b )
{
	double **r1 = (double **) a;
	double **r2 = (double **) b;

	double diff = r1[0][0] - r2[0][0];

	if( diff > 0 )
		return( 1 );
	else if( diff == 0 )
		return( 0 );
	else 
		return( -1 );
}

/* Free our state.
 */
static void
free_state( State *state )
{
	if( state->data ) {
		int i;

		for( i = 0; i < state->input->ysize; i++ )
			if( state->data[i] ) {
				im_free( state->data[i] );
				state->data[i] = NULL;
			}

		im_free( state->data );
		state->data = NULL;
	}
}

/* Fill our state.
 */
static int
build_state( State *state, DOUBLEMASK *input )
{
	int x, y, i;
	int xlow, xhigh;

	state->input = input;
	state->data = NULL;

	/* Need xlow and xhigh to get the size of the LUT we build.
	 */
	xlow = xhigh = input->coeff[0];
	for( y = 0; y < input->ysize; y++ ) {
		double v = input->coeff[y * input->xsize];

		if( floor( v ) != v ) {
			im_error( "im_buildlut", _( "x value not an int" ) );
			return( -1 );
		}

		if( v < xlow )
			xlow = v;
		if( v > xhigh )
			xhigh = v;
	}
	state->lut_size = xhigh - xlow;

	if( state->lut_size < 1 ) {
		im_error( "im_buildlut", _( "x range too small" ) );
		return( -1 );
	}

	if( !(state->data = IM_ARRAY( NULL, input->ysize, double * )) )
		return( -1 );
	for( y = 0; y < input->ysize; y++ ) 
		state->data[y] = NULL;

	for( y = 0; y < input->ysize; y++ ) 
		if( !(state->data[y] = IM_ARRAY( NULL, input->xsize, double )) )
			return( -1 );

	for( i = 0, y = 0; y < input->ysize; y++ ) 
		for( x = 0; x < input->xsize; x++, i++ ) 
			state->data[y][x] = input->coeff[i];

	/* Sort by 1st column in input.
	 */
	qsort( state->data, input->ysize, sizeof( double * ), compare );

#ifdef DEBUG
	printf( "Input table, sorted by 1st column\n" );
	for( y = 0; y < input->ysize; y++ ) {
		printf( "%.4d ", y );

		for( x = 0; x < input->xsize; x++ )
			printf( "%.9f ", state->data[y][x] );

		printf( "\n" );
	}
#endif /*DEBUG*/

	return( 0 );
}

static int
buildlut( State *state, IMAGE *output )
{
	const DOUBLEMASK *input = state->input;
	const int ysize = input->ysize;
	const int xsize = input->xsize;

	double *odata = (double *) output->data;

	int b, y, i, x;

	/* Do each output channel separately.
	 */
	for( b = 0; b < xsize - 1; b++ ) 
		for( x = b, y = 0; y < ysize - 1; y++ ) {
			const int x1 = state->data[y][0];
			const int x2 = state->data[y + 1][0];
			const double y1 = state->data[y][b + 1];
			const double y2 = state->data[y + 1][b + 1];
			const int dx = x2 - x1;
			const double dy = y2 - y1;
			
			for( i = 0; i < dx; i++, x += xsize - 1 )
				odata[x] = y1 + i * dy / dx;
		}

	return( 0 );
}

int
im_buildlut( DOUBLEMASK *input, IMAGE *output )
{
	State state;

	if( !input || input->xsize < 2 || input->ysize < 1 ) {
		im_error( "im_buildlut", _( "bad input matrix size" ) );
		return( -1 );
	}

	if( build_state( &state, input ) ) {
		free_state( &state );
                return( -1 );
	}

        im_initdesc( output,
                state.lut_size, 1, input->xsize - 1, 
		IM_BBITS_DOUBLE, IM_BANDFMT_DOUBLE,
                IM_CODING_NONE, IM_TYPE_HISTOGRAM, 1.0, 1.0, 0, 0 );
        if( im_setupout( output ) )
                return( -1 );

	if( buildlut( &state, output ) ) {
		free_state( &state );
		return( -1 );
	}

	free_state( &state );

	return( 0 );
}
