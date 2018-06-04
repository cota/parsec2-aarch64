/* @(#) Function to read a binary file with no header to vasari file
 * @(#) Usage:
 * @(#)
 * @(#) IMAGE *
 * @(#) im_binfile(in, xs, ys, bands, offset)
 * @(#) char *in;
 * @(#) int xs, ys, bands, offset;
 * @(#)
 * @(#) The function returns NULL on error.
 * @(#) Works for uchar input only.
 * Author: N. Dessipris
 * Written on: 31/7/91
 * Modified on:
 * 15/6/93 JC
 *	- includes fixed
 *	- externs fixed
 * 7/6/94 JC
 *	- im_outcheck() added
 * 13/12/94 JC
 *	- now uses im_mapfile(), rather than read() and copy
 * 	- hence returns a new IMAGE descriptor
 * 25/5/95 JC
 *	- oops! mess up with fd on errors
 * 5/2/04 JC
 *	- added offset param
 * 26/5/04
 *	- data init was broken by im_mapfile() change
 * 30/9/05
 * 	- use int64 for size calcs so we can map >31 bit files on 64 bit
 * 	  machines
 * 	- delay mmap() for large files, cf. im_openin()
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
#include <string.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#include <vips/vips.h>
#include <vips/internal.h>

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif /*WITH_DMALLOC*/

IMAGE *
im_binfile( const char *name, int xs, int ys, int bands, int offset )
{
	IMAGE *im;
	gint64 length;
	gint64 psize;

	/* Check parameters.
	 */
	if( xs <= 0 || ys <= 0 || bands <=0 ) {
		im_error( "im_binfile", _( "bad parameters" ) );
		return( NULL );
	}

	/* Make new output image for us.
	 */
	if( !(im = im_init( name )) )
		return( NULL );
	if( (im->fd = im__open_image_file( name )) == -1 ) {
		im_close( im );
		return( NULL );
	}
	im->dtype = IM_OPENIN;
	im->sizeof_header = offset;

	/* Predict file size.
	 */
	psize = (gint64) xs * ys * bands + offset;

	/* Read the file length and check against what we think 
	 * the size should be.
	 */
	if( (length = im_file_length( im->fd )) == -1 ) {
		im_close( im );
		return( NULL );
	}

	/* Very common, so special message.
	 */
	if( psize > length ) {
		im_error( "im_binfile", _( "unable to open %s: "
			"file has been truncated" ), im->filename );
		im_close( im );
		return( NULL );
	}

	/* Just wierd. Only print a warning for this, since we should
	 * still be able to process it without coredumps.
	 */
	if( psize < length )
		im_warn( "im_binfile", _( "%s is longer than expected" ),
			im->filename );

	/* If the predicted size is under our mmap threshold, mmap the whole
	 * thing now. Otherwise, delay the map until region create and we'll
	 * use a rolling window. See also im_openin().
	 */
	if( psize < im__mmap_limit ) {
		if( im_mapfile( im ) ) {
			im_close( im );
			return( NULL );
		}
		im->data = im->baseaddr + offset;
		im->dtype = IM_MMAPIN;
	}

	/* Set header fields.
	 */
	im->Xsize = xs;
	im->Ysize = ys;
	im->Bands = bands;

	/* Set others to standard values.
	 */
	im->Bbits = IM_BBITS_BYTE;
	im->BandFmt = IM_BANDFMT_UCHAR;
	im->Coding = IM_CODING_NONE;

	if( bands == 1 )
		im->Type = IM_TYPE_B_W;
	else if( bands == 3 )
		im->Type = IM_TYPE_RGB;
	else 
		im->Type = IM_TYPE_MULTIBAND;

	im->Xres = 1.0;
	im->Yres = 1.0;

	im->Length = 0;
	im->Compression = 0;
	im->Level = 0;

	im->Xoffset = 0;
	im->Yoffset = 0;

	/* Init others too.
	 */
	im->dhint = IM_THINSTRIP;

	return( im );
}
