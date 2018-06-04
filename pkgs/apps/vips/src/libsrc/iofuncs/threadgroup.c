/* Support for thread groups.
 * 
 * 29/9/99 JC
 *	- from thread.c
 * 23/10/03 JC
 * 	- threadgroup now has a kill flag as well
 * 9/6/06
 * 	- use an idle queue instead of flags
 * 24/11/06
 * 	- double-buffer file writes
 * 27/11/06
 * 	- break stuff out to generate / iterate
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

/* 
#define TIME_THREAD
#define DEBUG_CREATE
#define DEBUG_HIGHWATER
#define DEBUG_IO
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /*HAVE_UNISTD_H*/
#include <errno.h>

#include <vips/vips.h>
#include <vips/internal.h>
#include <vips/thread.h>

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif /*WITH_DMALLOC*/

#ifdef TIME_THREAD
/* Size of time buffers.
 */
#define IM_TBUF_SIZE (20000)
#endif /*TIME_THREAD*/

/* Maximum number of concurrent threads we allow. No reason for the limit,
 * it's just there to stop mad values for IM_CONCURRENCY killing the system.
 */
#define IM_MAX_THREADS (1024)

/* Name of environment variable we get concurrency level from.
 */
#define IM_CONCURRENCY "IM_CONCURRENCY"

/* Default tile geometry ... can be set by init_world.
 */
int im__tile_width = IM__TILE_WIDTH;
int im__tile_height = IM__TILE_HEIGHT;
int im__fatstrip_height = IM__FATSTRIP_HEIGHT;
int im__thinstrip_height = IM__THINSTRIP_HEIGHT;

/* Default n threads ... 0 means get from environment.
 */
int im__concurrency = 0;

#ifndef HAVE_THREADS
/* If we're building without gthread, we need stubs for the g_thread_*() and
 * g_mutex_*() functions. <vips/thread.h> has #defines which point the g_
 * names here.
 */

void im__g_thread_init( GThreadFunctions *vtable ) {}
gpointer im__g_thread_join( GThread *dummy ) { return( NULL ); }
gpointer im__g_thread_self( void ) { return( NULL ); }
GThread *im__g_thread_create_full( GThreadFunc d1, 
	gpointer d2, gulong d3, gboolean d4, gboolean d5, GThreadPriority d6,
	GError **d7 )
	{ return( NULL ); }

GMutex *im__g_mutex_new( void ) { return( NULL ); }
void im__g_mutex_free( GMutex *d ) {}
void im__g_mutex_lock( GMutex *d ) {}
void im__g_mutex_unlock( GMutex *d ) {}
#endif /*!HAVE_THREADS*/

void
im_concurrency_set( int concurrency )
{
	im__concurrency = concurrency;
}

/* Set (p)thr_concurrency() from IM_CONCURRENCY environment variable. Return 
 * the number of regions we should pass over the image.
 */
int
im_concurrency_get( void )
{
	const char *str;
	int nthr;

	/* Tell the threads system how much concurrency we expect.
	 */
	if( im__concurrency > 0 )
		nthr = im__concurrency;
	else if( (str = g_getenv( IM_CONCURRENCY )) ) 
		nthr = atoi( str );
	else
		/* Stick to minimum.
		 */
		nthr = 1;

	if( nthr < 1 || nthr > IM_MAX_THREADS ) {
		nthr = IM_CLIP( 1, nthr, IM_MAX_THREADS );

		im_warn( "im_concurrency_get", 
			_( "threads clipped to %d" ), nthr );
	}

	/* 

		FIXME .. hmm

#ifdef SOLARIS_THREADS
	if( thr_setconcurrency( nthr + 1 ) ) {
		im_error( "im_concurrency_get", _( "unable to set "
			"concurrency level to %d" ), nthr + 1 );
		return( -1 );
	}
#ifdef DEBUG_IO
	printf( "im_generate: using thr_setconcurrency(%d)\n", nthr+1 );
#endif 
#endif

#ifdef HAVE_PTHREAD
#ifdef HAVE_PTHREAD_SETCONCURRENCY
	if( pthread_setconcurrency( nthr + 1 ) ) {
		im_error( "im_concurrency_get", _( "unable to set "
			"concurrency level to %d" ), nthr + 1 );
		return( -1 );
	}
#ifdef DEBUG_IO
	printf( "im_generate: using pthread_setconcurrency(%d)\n", nthr+1 );
#endif 
#endif 
#endif 
	 */

	return( nthr );
}

#ifdef TIME_THREAD
/* Save time buffers.
 */
static int
save_time_buffers( REGION *reg )
{
	int i;
	static int rn = 1;
	FILE *fp;
	char name[ 256 ];

	im_snprintf( name, 256, "time%d", rn++ );
	if( !(fp = fopen( name, "w" )) )
		error_exit( "unable to write to \"%s\"", name );
	for( i = 0; i < reg->tpos; i++ )
		fprintf( fp, "%lld\n%lld\n", reg->btime[i], reg->etime[i] );
	fclose( fp );

	return( 0 );
}
#endif /*TIME_THREAD*/

/* Handle the idle list.
 */

/* Make sure thr is not on the idle list ... eg. on thread_free().
 */
static void
threadgroup_idle_remove( im_thread_t *thr )
{
	im_threadgroup_t *tg = thr->tg;

	g_mutex_lock( tg->idle_lock );

	tg->idle = g_slist_remove( tg->idle, thr );

#ifdef DEBUG_HIGHWATER
	tg->nidle -= 1;
	assert( tg->nidle >= 0 && tg->nidle <= tg->nthr );
#endif /*DEBUG_HIGHWATER*/

	g_mutex_unlock( tg->idle_lock );
}

/* Add to idle.
 */
static void
threadgroup_idle_add( im_thread_t *thr )
{
	im_threadgroup_t *tg = thr->tg;

	g_mutex_lock( tg->idle_lock );

	assert( !g_slist_find( tg->idle, thr ) );
	tg->idle = g_slist_prepend( tg->idle, thr );

#ifdef DEBUG_HIGHWATER
	tg->nidle += 1;
	assert( tg->nidle >= 0 && tg->nidle <= tg->nthr );
#endif /*DEBUG_HIGHWATER*/

	im_semaphore_up( &tg->idle_sem );

	g_mutex_unlock( tg->idle_lock );
}

/* Take the first thread off idle.
 */
im_thread_t *
im_threadgroup_get( im_threadgroup_t *tg )
{
	im_thread_t *thr;

	/* Wait for a thread to be added to idle.
	 */
	im_semaphore_down( &tg->idle_sem );

	g_mutex_lock( tg->idle_lock );

	assert( tg->idle );
	assert( tg->idle->data );
	thr = (im_thread_t *) tg->idle->data;
	tg->idle = g_slist_remove( tg->idle, thr );

#ifdef DEBUG_HIGHWATER
	tg->nidle -= 1;
	assert( tg->nidle >= 0 && tg->nidle <= tg->nthr );
	if( tg->nidle < tg->min_idle )
		tg->min_idle = tg->nidle;
#endif /*DEBUG_HIGHWATER*/

	g_mutex_unlock( tg->idle_lock );

	return( thr );
}

/* Wait for all threads to be idle.
 */
void
im_threadgroup_wait( im_threadgroup_t *tg )
{
	/* Wait for all threads to signal idle.
	 */
	im_semaphore_downn( &tg->idle_sem, tg->nthr );

	g_mutex_lock( tg->idle_lock );

	assert( tg->idle );
	assert( g_slist_length( tg->idle ) == tg->nthr );

	g_mutex_unlock( tg->idle_lock );

	/* All threads are now blocked on their 'go' semaphores, and idle is 
	 * zero. Up idle by the number of threads, ready for the next loop.
	 */
	im_semaphore_upn( &tg->idle_sem, tg->nthr );
}

/* Junk a thread.
 */
static void
thread_free( im_thread_t *thr )
{
        /* Is there a thread running this region? Kill it!
         */
        if( thr->thread ) {
                thr->kill = -1;
		im_semaphore_up( &thr->go );

		/* Return value is always NULL (see thread_main_loop).
		 */
		(void) g_thread_join( thr->thread );
#ifdef DEBUG_CREATE
		printf( "thread_free: g_thread_join()\n" );
#endif /*DEBUG_CREATE*/

		thr->thread = NULL;
        }
	im_semaphore_destroy( &thr->go );

	threadgroup_idle_remove( thr );
	IM_FREEF( im_region_free, thr->reg );
	thr->oreg = NULL;
	thr->tg = NULL;

#ifdef TIME_THREAD
	if( thr->btime )
		(void) save_time_buffers( thr );
#endif /*TIME_THREAD*/
}

/* The work we do in one loop ... fill a region, and call a function. Either
 * called from the main thread (if no threading), or from worker.
 */
static void
work_fn( im_thread_t *thr )
{
	/* Doublecheck only one thread per region.
	 */
	assert( thr->thread == g_thread_self() );

	/* Prepare this area.
	 */
	if( thr->tg->inplace ) {
		if( im_prepare_to( thr->reg, thr->oreg, 
			&thr->pos, thr->x, thr->y ) )
			thr->error = -1;
	}
	else {
		/* Attach to this position.
		 */
		if( im_prepare( thr->reg, &thr->pos ) )
			thr->error = -1;
	}

	/* Call our work function.
	 */
	if( !thr->error && thr->tg->work && 
		thr->tg->work( thr->reg, thr->a, thr->b, thr->c ) )
		thr->error = -1;

	/* Back on the idle queue again.
	 */
	threadgroup_idle_add( thr );
}

#ifdef HAVE_THREADS
/* What runs as a thread ... loop, waiting to be told to fill our region.
 */
static void *
thread_main_loop( void *a )
{
        im_thread_t *thr = (im_thread_t *) a;
	im_threadgroup_t *tg = thr->tg;

	/* We now control the region (it was created by tg when we were
	 * built).
	 */
	im__region_take_ownership( thr->reg );

	for(;;) {
		assert( tg == thr->tg );

		/* Signal the main thread that we are idle, and block.
		 */
		im_semaphore_down( &thr->go );

		/* Asked to exit?
		 */
		if( thr->kill )
			break;

#ifdef TIME_THREAD
		/* Note start time.
		 */
		if( thr->btime )
			thr->btime[thr->tpos] = gethrtime();
#endif /*TIME_THREAD*/

		/* Loop once.
		 */
		work_fn( thr ); 

#ifdef TIME_THREAD
		/* Note stop time.
		 */
		if( thr->etime ) {
			thr->etime[thr->tpos] = gethrtime();
			thr->tpos++;
		}
#endif /*TIME_THREAD*/
	}

        return( NULL );
}
#endif /*HAVE_THREADS*/

/* Attach another thread to a threadgroup.
 */
static im_thread_t *
threadgroup_thread_new( im_threadgroup_t *tg )
{
	im_thread_t *thr;

	if( !(thr = IM_NEW( tg->im, im_thread_t )) )
		return( NULL );
	thr->tg = tg;
	thr->reg = NULL;
	thr->thread = NULL;
	im_semaphore_init( &thr->go, 0, "go" );
	thr->kill = 0;
	thr->error = 0;
	thr->oreg = NULL;
	thr->a = thr->b = thr->c = NULL;
#ifdef TIME_THREAD
	thr->btime = NULL;
	thr->etime = NULL;
	thr->tpos = 0;
#endif /*TIME_THREAD*/

	/* Attach stuff. 
	 */
	if( !(thr->reg = im_region_create( tg->im )) ) {
		thread_free( thr );
		return( NULL );
	}

	/* Get ready to hand the region over to the thread.
	 */
	im__region_no_ownership( thr->reg );

#ifdef TIME_THREAD
	thr->btime = IM_ARRAY( tg->im, IM_TBUF_SIZE, hrtime_t );
	thr->etime = IM_ARRAY( tg->im, IM_TBUF_SIZE, hrtime_t );
	if( !thr->btime || !thr->etime ) {
		thread_free( thr );
		return( NULL );
	}
#endif /*TIME_THREAD*/

#ifdef HAVE_THREADS
	/* Make a worker thread. We have to use g_thread_create_full() because
	 * we need to insist on a non-tiny stack. Some platforms default to
	 * very small values (eg. various BSDs).
	 */
	if( !(thr->thread = g_thread_create_full( thread_main_loop, thr, 
		IM__DEFAULT_STACK_SIZE, TRUE, FALSE, 
		G_THREAD_PRIORITY_NORMAL, NULL )) ) {
		im_error( "threadgroup_thread_new", 
			_( "unable to create thread" ) );
		thread_free( thr );
		return( NULL );
	}

#ifdef DEBUG_CREATE
	printf( "threadgroup_thread_new: g_thread_create_full()\n" );
#endif /*DEBUG_CREATE*/
#endif /*HAVE_THREADS*/

	/* It's idle.
	 */
	threadgroup_idle_add( thr );

	return( thr );
}

/* Kill all threads in a threadgroup, if there are any.
 */
static void
threadgroup_kill_threads( im_threadgroup_t *tg )
{
	if( tg->thr ) {
		int i;

		for( i = 0; i < tg->nthr; i++ ) 
			thread_free( tg->thr[i] );
		tg->thr = NULL;

		assert( !tg->idle );

		/* Reset the idle semaphore.
		 */
		im_semaphore_destroy( &tg->idle_sem );
		im_semaphore_init( &tg->idle_sem, 0, "idle" );

#ifdef DEBUG_IO
		printf( "threadgroup_kill_threads: killed %d threads\n", 
			tg->nthr );
#endif /*DEBUG_IO*/
	}
}

/* Free a threadgroup. Can be called multiple times.
 */
int
im_threadgroup_free( im_threadgroup_t *tg )
{
#ifdef DEBUG_IO
	printf( "im_threadgroup_free: \"%s\" (%p)\n", tg->im->filename, tg );
#endif /*DEBUG_IO*/

	if( !tg || tg->zombie )
		return( 0 );

	threadgroup_kill_threads( tg );

	im_semaphore_destroy( &tg->idle_sem );
	IM_FREEF( g_mutex_free, tg->idle_lock );
	tg->zombie = -1;

#ifdef DEBUG_HIGHWATER
	printf( "im_threadgroup_free %p: max busy workers = %d\n", 
		tg, tg->nthr - tg->min_idle );
#endif /*DEBUG_HIGHWATER*/

	return( 0 );
}

/* Attach a threadgroup to an image.
 */
im_threadgroup_t *
im_threadgroup_create( IMAGE *im )
{
	im_threadgroup_t *tg;
	int i;

	/* Allocate and init new thread block.
	 */
	if( !(tg = IM_NEW( im, im_threadgroup_t )) )
		return( NULL );
	tg->zombie = 0;
	tg->im = im;
	tg->work = NULL;
	tg->inplace = 0;
	if( (tg->nthr = im_concurrency_get()) < 0 )
		return( NULL );
	tg->thr = NULL;
	tg->kill = 0;

	/* Pick a render geometry.
	 */
	switch( tg->im->dhint ) {
	case IM_SMALLTILE:
		tg->pw = im__tile_width;
		tg->ph = im__tile_height;
		tg->nlines = tg->ph;
		break;

	case IM_FATSTRIP:
		tg->pw = tg->im->Xsize;
		tg->ph = im__fatstrip_height;
		tg->nlines = tg->ph * tg->nthr * 2;
		break;

	case IM_ANY:
	case IM_THINSTRIP:
		tg->pw = tg->im->Xsize;
		tg->ph = im__thinstrip_height;
		tg->nlines = tg->ph * tg->nthr * 2;
		break;

	default:
		error_exit( "panic: internal error #98i245983425" );
	}

	/* Attach tidy-up callback.
	 */
	if( im_add_close_callback( im, 
		(im_callback_fn) im_threadgroup_free, tg, NULL ) ) {
		(void) im_threadgroup_free( tg );
		return( NULL );
	}

#ifdef DEBUG_IO
	printf( "im_threadgroup_create: %d by %d patches, "
		"groups of %d scanlines\n", tg->pw, tg->ph, tg->nlines );
#endif /*DEBUG_IO*/

	/* Init locks.
	 */
	im_semaphore_init( &tg->idle_sem, 0, "idle" );
	tg->idle = NULL;
	tg->idle_lock = g_mutex_new();

#ifdef DEBUG_HIGHWATER
	tg->nidle = 0;
	tg->min_idle = tg->nthr;
#endif /*DEBUG_HIGHWATER*/

	/* Make thread array.
	 */
	if( !(tg->thr = IM_ARRAY( im, tg->nthr + 1, im_thread_t * )) )
		return( NULL );
	for( i = 0; i < tg->nthr + 1; i++ )
		tg->thr[i] = NULL;

	/* Attach threads.
	 */
	for( i = 0; i < tg->nthr; i++ )
		if( !(tg->thr[i] = threadgroup_thread_new( tg )) )
			return( NULL );

#ifdef DEBUG_IO
	printf( "im_threadgroup_create: \"%s\" (%p), with %d threads\n", 
		im->filename, tg, tg->nthr );
#endif /*DEBUG_IO*/

	return( tg );
}

/* Trigger work. If not threading, just call fn directly.
 */
void
im_threadgroup_trigger( im_thread_t *thr )
{
	im_threadgroup_t *tg = thr->tg;

	/* thr pos needs to be set before coming here ... check.
	 */
{
	Rect image;

	image.left = 0;
	image.top = 0;
	image.width = tg->im->Xsize;
	image.height = tg->im->Ysize;

	assert( im_rect_includesrect( &image, &thr->pos ) );
}

	/* Start worker going.
	 */
	im_semaphore_up( &thr->go );

#ifndef HAVE_THREADS
	/* No threading ... just eval directly.
	 */
	work_fn( thr );
#endif /*HAVE_THREADS*/
}

/* Test all threads for error.
 */
int
im_threadgroup_iserror( im_threadgroup_t *tg )
{
	int i;

	if( tg->kill )
		return( -1 );
	if( tg->im->kill )
		return( -1 );

	for( i = 0; i < tg->nthr; i++ ) 
		if( tg->thr[i]->error )
			return( -1 );

	return( 0 );
}
