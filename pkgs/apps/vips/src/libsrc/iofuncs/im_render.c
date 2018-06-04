/* render an image in the background as a set of tiles
 *
 * don't read from mask after closing out
 *
 * JC, 30 sep 03 
 *
 * 22/10/03 JC
 *	- now uses threadgroup kill system, avoiding race condition
 * 2/2/04 JC
 *	- cache failed for large images
 * 8/4/04
 *	- touch reused tiles so they don't get reused again too soon ... helps
 *	  stop thrashing when we've many pending paints and lots of threads
 * 15/4/04
 *	- added im_cache() convenience function
 * 26/1/05
 *	- added im_render_fade() ... fade tiles display for nip2
 *	- mask can now be NULL for no mask output
 * 11/2/05
 *	- tidies
 * 27/2/05
 *	- limit the number of simultaneous renders
 *	- kill threadgroups when no dirties left
 *	- max == -1 means unlimited cache size
 *	- 'priority' marks non-suspendable renders
 * 1/4/05
 *	- rewritten for a few global threads instead, and a job queue ...
 *	  should be simpler & more reliable
 * 23/4/07
 * 	- oop, race condition fixed
 * 14/3/08
 * 	- oop, still making fade threads even when not fading
 * 	- more instrumenting
 * 23/4/08
 * 	- oop, broken for mask == NULL
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

/* Turn on debugging output.
#define DEBUG
#define DEBUG_REUSE
#define DEBUG_MAKE
#define DEBUG_PAINT
#define DEBUG_TG
#define DEBUG_FADE
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /*HAVE_UNISTD_H*/

/* Need Sleep() on win32.
 */
#ifdef OS_WIN32
#include <windows.h>
#endif /*OS_WIN32*/

#include <vips/vips.h>
#include <vips/thread.h>

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif /*WITH_DMALLOC*/

#ifdef HAVE_THREADS
static const int have_threads = 1;
#else /*!HAVE_THREADS*/
static const int have_threads = 0;
#endif /*HAVE_THREADS*/

#ifdef DEBUG_TG
static int threadgroup_count = 0;
static int threadgroup_active = 0;
#endif /*DEBUG_TG*/

#ifdef DEBUG_FADE
static int fade_count = 0;
static int fade_active = 0;
#endif /*DEBUG_FADE*/

/* A manager thread. We have a fixed number of these taking jobs off the list
 * of current renders with dirty tiles, doing a tile, and putting the render 
 * back.
 */
typedef struct _RenderThread {
	GThread *gthread;
	struct _Render *render;	/* The last render we worked on */
} RenderThread;

/* Notify caller through this.
 */
typedef void (*notify_fn)( IMAGE *, Rect *, void * );

/* The states a tile can be in.
 */
typedef enum {
	TILE_DIRTY,		/* On the dirty list .. contains no pixels */
	TILE_WORKING,		/* Currently being worked on */
	TILE_PAINTED_FADING,	/* Painted, on fade list */
	TILE_PAINTED		/* Painted, ready for reuse */
} TileState;

/* A tile in our cache. 
 */
typedef struct {
	struct _Render *render;

	Rect area;		/* Place here (unclipped) */
	REGION *region;		/* REGION with the pixels */

	int access_ticks;	/* Time of last use for LRU flush */
	int time;		/* Time when we finished painting */

	TileState state;
} Tile;

/*

	FIXME ... should have an LRU queue rather than this thing with times

	FIXME ... could also hash from tile xy to tile pointer

 */

/* Per-call state.
 */
typedef struct _Render {
	/* Reference count this, since we use these things from several
	 * threads. Can't easily use the gobject ref count system, since we
	 * need a lock around operations.
	 */
	int ref_count;
	GMutex *ref_count_lock;	

	/* Parameters.
	 */
	IMAGE *in;		/* Image we render */
	IMAGE *out;		/* Write tiles here on demand */
	IMAGE *mask;		/* Set valid pixels here */
	int width, height;	/* Tile size */
	int max;		/* Maximum number of tiles */
	int fps;		/* FPS for fade in */
	int steps;		/* Steps fade occurs in */
	int priority;		/* Larger numbers done sooner */
	notify_fn notify;	/* Tell caller about paints here */
	void *client;

	/* Make readers single thread with this. No point allowing
	 * multi-thread read.
	 */
	GMutex *read_lock;	

	/* Tile cache.
	 */
	GSList *cache;		/* List of all our tiles */
	int ntiles;		/* Number of cache tiles */
	int access_ticks;	/* Inc. on each access ... used for LRU */

	/* List of tiles which are to be painted.
	 */
	GMutex *dirty_lock;	/* Lock before we read/write the dirty list */
	GSList *dirty;		/* Tiles which need painting */

	/* List of painted tiles being faded in. Plus a fade-in thread.
	 */
	GMutex *fade_lock;	/* Lock before we read/write the fade list */
	GSList *fade;		/* Tiles which are being faded */
	GThread *fade_gthread;	/* Fade tiles in thread */
	int time;		/* Increment each frame */
	int fade_kill;		/* Set to ask fade thread to exit */

	/* Render thread stuff.
	 */
	im_threadgroup_t *tg;	/* Render with this threadgroup */
	int render_kill;	/* This render is dying */
} Render;

/* Number of RenderThread we create.
 */
static const int render_thread_max = 1;

static GSList *render_thread_all = NULL;

/* Number of renders with dirty tiles. RenderThreads queue up on this.
 */
static im_semaphore_t render_dirty_sem;

/* All the renders with dirty tiles.
 */
static GMutex *render_dirty_lock = NULL;
static GSList *render_dirty_all = NULL;

static void
render_dirty_remove( Render *render )
{
	g_mutex_lock( render_dirty_lock );

	if( g_slist_find( render_dirty_all, render ) ) {
		render_dirty_all = g_slist_remove( render_dirty_all, render );

		/* We know this can't block, since there is at least 1 item in
		 * the render_dirty_all list (us). Except that
		 * render_dirty_get() does a _down() before it locks, so this
		 * could block if we run inbetween :-( possible deadlock.
		 */
		im_semaphore_down( &render_dirty_sem );
	}

	g_mutex_unlock( render_dirty_lock );
}

static void *
tile_free( Tile *tile )
{
#ifdef DEBUG_MAKE
	printf( "tile_free\n" );
#endif /*DEBUG_MAKE*/

	if( tile->region ) {
		(void) im_region_free( tile->region );
		tile->region = NULL;
	}

	im_free( tile );

	return( NULL );
}

static int
render_free( Render *render )
{
#ifdef DEBUG_MAKE
	printf( "render_free: %p\n", render );
#endif /*DEBUG_MAKE*/

	assert( render->ref_count == 0 );

	render_dirty_remove( render );

	/* Kill fade thread.
	 */
	if( render->fade_gthread ) {
		render->fade_kill = 1;
		(void) g_thread_join( render->fade_gthread );
		render->fade_gthread = NULL;
		render->fade_kill = 0;

#ifdef DEBUG_FADE
		fade_active -= 1;
		printf( "render_free: %d fade active\n", fade_active );
#endif /*DEBUG_FADE*/
	}

	IM_FREEF( im_threadgroup_free, render->tg );

	/* Free cache.
	 */
	im_slist_map2( render->cache,
		(VSListMap2Fn) tile_free, NULL, NULL );
	IM_FREEF( g_slist_free, render->cache );
	render->ntiles = 0;
	IM_FREEF( g_slist_free, render->dirty );
	IM_FREEF( g_slist_free, render->fade );

	g_mutex_free( render->ref_count_lock );
	g_mutex_free( render->dirty_lock );
	g_mutex_free( render->read_lock );
	g_mutex_free( render->fade_lock );

	im_free( render );

	return( 0 );
}

/* Ref and unref a Render ... free on last unref.
 */
static int
render_ref( Render *render )
{
	g_mutex_lock( render->ref_count_lock );
	assert( render->ref_count != 0 );
	render->ref_count += 1;
	g_mutex_unlock( render->ref_count_lock );

	return( 0 );
}

static int
render_unref( Render *render )
{
	int kill;

	g_mutex_lock( render->ref_count_lock );
	assert( render->ref_count > 0 );
	render->ref_count -= 1;
	kill = render->ref_count == 0;
	g_mutex_unlock( render->ref_count_lock );

	if( kill )
		render_free( render );

	return( 0 );
}

/* Wait for a render with dirty tiles.
 */
static Render *
render_dirty_get( void )
{
	Render *render;

	/* Wait for a render with dirty tiles.
	 */
	im_semaphore_down( &render_dirty_sem );

	g_mutex_lock( render_dirty_lock );

	/* Just take the head of the jobs list ... we sort when we add. If
	 * render_dirty_remove() is called between our semaphore letting us in
	 * and the _lock(), render_dirty_all can be NULL.
	 */
	render = NULL;
	if( render_dirty_all ) {
		render = (Render *) render_dirty_all->data;

		assert( render->ref_count == 1 );

		/* Ref the render to make sure it can't die while we're
		 * working on it.
		 */
		render_ref( render );

		render_dirty_all = g_slist_remove( render_dirty_all, render );
	}

	g_mutex_unlock( render_dirty_lock );

	return( render );
}

/* Do a single tile. Take a dirty tile from the dirty list, fill with pixels
 * and add to the fade list.
 */
static void
render_dirty_process( Render *render )
{
	Tile *tile;

	/* Take a tile off the dirty list.
	 */
	g_mutex_lock( render->dirty_lock );
	if( render->dirty ) { 
		tile = (Tile *) render->dirty->data;
		assert( tile->state == TILE_DIRTY );
		render->dirty = g_slist_remove( render->dirty, tile );
		tile->state = TILE_WORKING;
	}
	else
		tile = NULL;
	g_mutex_unlock( render->dirty_lock );

	if( tile ) {
		int result;

		result = -1;
		if( !render->tg ) {
			render->tg = im_threadgroup_create( render->in );

#ifdef DEBUG_TG
			printf( "render_paint_tile: "
				"%p starting threadgroup\n",
				render );
			threadgroup_count += 1;
			printf( "render_paint_tile: %d\n", threadgroup_count );
			threadgroup_active += 1;
			printf( "render_dirty_put: %d active\n", 
				threadgroup_active );
#endif /*DEBUG_TG*/
		}

		if( render->tg ) {
#ifdef DEBUG_PAINT
			printf( "render_fill_tile: "
				"%p paint of tile %dx%d\n",
				render,
				tile->area.left, tile->area.top );
#endif /*DEBUG_PAINT*/

			/* We're writing to the tile region, but we didn't 
			 * make it.
			 */
			im__region_take_ownership( tile->region );

			result = im_prepare_thread( render->tg, 
				tile->region, &tile->area );
		}

		/*

			FIXME ... nice if we did something with the error return

		 */
#ifdef DEBUG_PAINT
		if( result )
			printf( "render_fill_tile: "
				"im_prepare_thread() failed!\n\t%s\n", 
				im_error_buffer() );
#endif /*DEBUG_PAINT*/

		/* Are we fading? Hand the tile over to the fade thread.
		 */
		if( render->fade_gthread ) {
			g_mutex_lock( render->fade_lock );

			render->fade = g_slist_prepend( render->fade, tile );
			tile->time = render->time;
			tile->state = TILE_PAINTED_FADING;

			/* Hand tile over to another thread.
			 */
			im__region_no_ownership( tile->region );

			g_mutex_unlock( render->fade_lock );
		}
		else {
			/* Not fading. Tell the user we're done ourselves.
			 */
			tile->time = render->time;
			tile->state = TILE_PAINTED;
			im__region_no_ownership( tile->region );
			if( render->notify ) 
				render->notify( render->out, 
					&tile->area, render->client );
		}
	}
}

static int       
render_dirty_sort( Render *a, Render *b )
{
	return( b->priority - a->priority );
}

/* Add to the jobs list, if it has work to be done.
 */
static void
render_dirty_put( Render *render )
{
	g_mutex_lock( render_dirty_lock );

	if( render->dirty && !render->render_kill ) {
		if( !g_slist_find( render_dirty_all, render ) ) {
			render_dirty_all = g_slist_prepend( render_dirty_all, 
				render );
			render_dirty_all = g_slist_sort( render_dirty_all,
				(GCompareFunc) render_dirty_sort );
			im_semaphore_up( &render_dirty_sem );
		}
	}
	else {
		/* Coming off the jobs list ... shut down the render pipeline.
		 */
#ifdef DEBUG_TG
		printf( "render_dirty_put: %p stopping threadgroup\n", render );
		threadgroup_active -= 1;
		printf( "render_dirty_put: %d active\n", threadgroup_active );
#endif /*DEBUG_TG*/
		IM_FREEF( im_threadgroup_free, render->tg );
	}

	g_mutex_unlock( render_dirty_lock );
}

/* Main loop for RenderThreads.
 */
static void *
render_thread_main( void *client )
{
	/* Could use this if we want per-thread state in the future.
	RenderThread *thread = (RenderThread *) client;
	 */

	for(;;) {
		Render *render;

		if( (render = render_dirty_get()) ) {
			render_dirty_process( render );
			render_dirty_put( render );

			assert( render->ref_count == 1 ||
				render->ref_count == 2 );

			/* _get() does a ref to make sure we keep the render
			 * alive during processing ... unref before we loop.
			 * This can kill off the render.
			 */
			render_unref( render );
		}
	}
}

/* Create our set of RenderThread. Assume we're single-threaded here.
 */
static int
render_thread_create( void )
{
	int len = g_slist_length( render_thread_all );
	int i;

	if( !have_threads )
		return( 0 );

	/* 1st time through only.
	 */
	if( !render_dirty_lock ) {
		render_dirty_lock = g_mutex_new();
		im_semaphore_init( &render_dirty_sem, 0, "render_dirty_sem" );
	}

	for( i = len; i < render_thread_max; i++ ) {
		RenderThread *thread = IM_NEW( NULL, RenderThread );

		thread->gthread = NULL;
		thread->render = NULL;

		if( !(thread->gthread = g_thread_create_full( 
			render_thread_main, thread, 
			IM__DEFAULT_STACK_SIZE, TRUE, FALSE, 
			G_THREAD_PRIORITY_NORMAL, NULL )) ) {
			im_free( thread );
			im_error( "im_render", _( "unable to create thread" ) );
			return( -1 );
		}

		render_thread_all = g_slist_prepend( render_thread_all, 
			thread );
	}

	return( 0 );
}

#ifdef DEBUG
static char *
tile_state_name( TileState state )
{
	switch( state ) {
	case TILE_DIRTY: 		return( "TILE_DIRTY" );
	case TILE_WORKING: 		return( "TILE_WORKING" );
	case TILE_PAINTED_FADING: 	return( "TILE_PAINTED_FADING" );
	case TILE_PAINTED: 		return( "TILE_PAINTED" );

	default:
		assert( FALSE );
	}
}

static void *
tile_print( Tile *tile )
{
	printf( "tile: %dx%d, access_ticks = %d, time = %d,\n"
		"\tstate = %s\n",
		tile->area.left, tile->area.top,
		tile->access_ticks, tile->time,
		tile_state_name( tile->state ) );

	return( NULL );
}
#endif /*DEBUG*/

/* Come here for the fade work thread.
 */
static void *
render_fade( void *client )
{
	Render *render = (Render *) client;

	for(;;) {
		GSList *p;
		GSList *next;

		if( render->fade_kill )
			break;

		/* Increment frame counter. Used to set bg/fg ratio for fade.
		 */
		render->time += 1;

		g_mutex_lock( render->fade_lock );
		for( p = render->fade; p; p = next ) {
			Tile *tile = (Tile *) p->data;

			assert( tile->state == TILE_PAINTED_FADING );

			/* We remove the current element as we scan :-( so get
			 * next beforehand.
			 */
			next = p->next;

			if( render->time - tile->time > render->steps ) {
				/* Tile is solid ... comes off the fade list.
				 */
				render->fade = g_slist_remove( render->fade,
					tile );
				tile->state = TILE_PAINTED;
			}

			/* Ask client to update.
			 */
			if( render->notify ) 
				render->notify( render->out, 
					&tile->area, render->client );
		}
		g_mutex_unlock( render->fade_lock );

		/* Sleep at the end of the loop, so if we have fading turned
		 * off there's no wait.
		 */
#ifdef OS_WIN32
		Sleep( 1000 / render->fps );
#else /*!OS_WIN32*/
		usleep( 1000000 / render->fps );
#endif /*!OS_WIN32*/
	}

	return( NULL );
}

static Render *
render_new( IMAGE *in, IMAGE *out, IMAGE *mask, 
	int width, int height, int max, 
	int fps, int steps,
	int priority,
	notify_fn notify, void *client )
{
	Render *render;

	/* Don't use auto-free for render, we do our own lifetime management
	 * with _ref() and _unref().
	 */
	if( !(render = IM_NEW( NULL, Render )) )
		return( NULL );

	render->ref_count = 1;
	render->ref_count_lock = g_mutex_new();

	render->in = in;
	render->out = out;
	render->mask = mask;
	render->width = width;
	render->height = height;
	render->max = max;
	render->fps = fps;
	render->steps = steps;
	render->priority = priority;
	render->notify = notify;
	render->client = client;

	render->read_lock = g_mutex_new();

	render->cache = NULL;
	render->ntiles = 0;
	render->access_ticks = 0;

	render->dirty_lock = g_mutex_new();
	render->dirty = NULL;

	render->fade_lock = g_mutex_new();
	render->fade = NULL;
	render->fade_gthread = NULL;
	render->time = 0;
	render->fade_kill = 0;

	render->tg = NULL;
	render->render_kill = FALSE;

	if( im_add_close_callback( out, 
                (im_callback_fn) render_unref, render, NULL ) ) {
                (void) render_unref( render );
                return( NULL );
        }

	/* Only need the fade thread if we're going to fade: ie. steps > 1.
	 */
	if( have_threads && steps > 1 ) {
		if( !(render->fade_gthread = g_thread_create_full( 
			render_fade, render, 
			IM__DEFAULT_STACK_SIZE, TRUE, FALSE, 
			G_THREAD_PRIORITY_NORMAL, NULL )) ) {
			im_error( "im_render", _( "unable to create thread" ) );
			return( NULL );
		}

#ifdef DEBUG_FADE
		fade_count += 1;
		fade_active += 1;
		printf( "render_new: creating fade thread %d\n", fade_count );
		printf( "render_new: %d active\n", fade_active );
#endif /*DEBUG_FADE*/
	}

	return( render );
}

/* Make a Tile.
 */
static Tile *
tile_new( Render *render )
{
	Tile *tile;

#ifdef DEBUG_MAKE
	printf( "tile_new\n" );
#endif /*DEBUG_MAKE*/

	/* Don't use auto-free: we need to make sure we free the tile after
	 * Render.
	 */
	if( !(tile = IM_NEW( NULL, Tile )) )
		return( NULL );

	tile->render = render;
	tile->region = NULL;
	tile->area.left = 0;
	tile->area.top = 0;
	tile->area.width = 0;
	tile->area.height = 0;
	tile->access_ticks = render->access_ticks;
	tile->time = 0;
	tile->state = TILE_WORKING;

	if( !(tile->region = im_region_create( render->in )) ) {
		(void) tile_free( tile );
		return( NULL );
	}

	render->cache = g_slist_prepend( render->cache, tile );
	render->ntiles += 1;

	return( tile );
}

static void *
tile_test_area( Tile *tile, Rect *area )
{
	if( im_rect_equalsrect( &tile->area, area ) )
		return( tile );

	return( NULL );
}

/* Search the cache for a tile, NULL if not there. Could be *much* faster. If
 * we find a tile, bump to front.
 */
static Tile *
render_tile_lookup( Render *render, Rect *area )
{
	Tile *tile;

	tile = (Tile *) im_slist_map2( render->cache,
		(VSListMap2Fn) tile_test_area, area, NULL );

	/* We've looked at a tile ... bump to end of LRU and front of dirty.
	 */
	if( tile ) {
		tile->access_ticks = render->access_ticks;
		render->access_ticks += 1;

		g_mutex_lock( render->dirty_lock );
		if( tile->state == TILE_DIRTY ) {
#ifdef DEBUG
			printf( "tile_bump_dirty: bumping tile %dx%d\n",
				tile->area.left, tile->area.top );
#endif /*DEBUG*/

			render->dirty = g_slist_remove( render->dirty, tile );
			render->dirty = g_slist_prepend( render->dirty, tile );
		}
		g_mutex_unlock( render->dirty_lock );
	}

	return( tile );
}

/* Add a tile to the dirty list.
 */
static void
tile_set_dirty( Tile *tile, Rect *area )
{
	Render *render = tile->render;

#ifdef DEBUG_PAINT
	printf( "tile_set_dirty: adding tile %dx%d to dirty\n",
		area->left, area->top );
#endif /*DEBUG_PAINT*/

	assert( tile->state == TILE_WORKING );

	/* Touch the ticks ... we want to make sure this tile will not be
	 * reused too soon, so it gets a chance to get painted.
	 */
	tile->access_ticks = render->access_ticks;
	render->access_ticks += 1;

	g_mutex_lock( render->dirty_lock );

	tile->state = TILE_DIRTY;
	tile->area = *area;
	render->dirty = g_slist_prepend( render->dirty, tile );

	/* Someone else will write to it now.
	 */
	im__region_no_ownership( tile->region );

	/* Can't unlock render->dirty_lock here, we need to render_dirty_put()
	 * before the tile is processed.
	 */

	if( render->notify && have_threads ) 
		/* Add to the list of renders with dirty tiles. One of our bg 
		 * threads will pick it up and paint it.
		 */
		render_dirty_put( render );
	else {
		/* No threads, or no notify ... paint the tile ourselves, 
		 * sychronously. No need to notify the client, since they'll 
		 * never see black tiles.
		 */
#ifdef DEBUG_PAINT
		printf( "tile_set_dirty: painting tile %dx%d synchronously\n",
			area->left, area->top );
#endif /*DEBUG_PAINT*/

		if( !render->tg )
			render->tg = im_threadgroup_create( render->in );

		/* We're writing to the tile region, but we didn't make it.
		 */
		im__region_take_ownership( tile->region );

		if( render->tg )
			im_prepare_thread( render->tg, 
				tile->region, &tile->area );

		/* Can't fade in synchronous mode .. straight to painted.
		 */
		tile->state = TILE_PAINTED;
		render->dirty = g_slist_remove( render->dirty, tile );
	}

	g_mutex_unlock( render->dirty_lock );
}

static void *
tile_test_clean_ticks( Tile *this, Tile **best )
{
	if( this->state == TILE_PAINTED )
		if( !*best || this->access_ticks < (*best)->access_ticks )
			*best = this;

	return( NULL );
}

/* Pick a painted tile to reuse. Search for LRU (slow!).
 */
static Tile *
render_tile_get_painted( Render *render )
{
	Tile *tile;

	tile = NULL;
	im_slist_map2( render->cache,
		(VSListMap2Fn) tile_test_clean_ticks, &tile, NULL );

	if( tile ) {
		assert( tile->state == TILE_PAINTED );

#ifdef DEBUG_REUSE
		printf( "render_tile_get_painted: reusing painted %p\n", tile );

		g_mutex_lock( render->dirty_lock );
		assert( !g_slist_find( render->dirty, tile ) );
		g_mutex_unlock( render->dirty_lock );

		g_mutex_lock( render->fade_lock );
		assert( !g_slist_find( render->fade, tile ) );
		g_mutex_unlock( render->fade_lock );
#endif /*DEBUG_REUSE*/

		tile->state = TILE_WORKING;
	}

	return( tile );
}

/* Take a tile off the end of the dirty list.
 */
static Tile *
render_tile_get_dirty( Render *render )
{
	Tile *tile;

	g_mutex_lock( render->dirty_lock );
	if( !render->dirty )
		tile = NULL;
	else {
		tile = (Tile *) g_slist_last( render->dirty )->data;
		render->dirty = g_slist_remove( render->dirty, tile );
		tile->state = TILE_WORKING;
	}
	g_mutex_unlock( render->dirty_lock );

#ifdef DEBUG_REUSE
	if( tile )
		printf( "render_tile_get_dirty: reusing dirty %p\n", tile );
#endif /*DEBUG_REUSE*/

	return( tile );
}

static Tile *
render_tile_get( Render *render, Rect *area )
{
	Tile *tile;

	/* Got this tile already?
	 */
	if( (tile = render_tile_lookup( render, area )) ) {
#ifdef DEBUG_PAINT
		printf( "render_tile_get: found %dx%d in cache\n",
			area->left, area->top );
#endif /*DEBUG_PAINT*/

		return( tile );
	}

	/* Have we fewer tiles than teh max? Can just make a new tile.
	 */
	if( render->ntiles < render->max || render->max == -1 ) {
		if( !(tile = tile_new( render )) ) 
			return( NULL );
	}
	else {
		/* Need to reuse a tile. Try for an old painted tile first, 
		 * then if that fails, reuse a dirty tile. Don't search the 
		 * fading list, though we could maybe scavenge there.
		 */
		if( !(tile = render_tile_get_painted( render )) &&
			!(tile = render_tile_get_dirty( render )) ) 
			return( NULL );

#ifdef DEBUG_REUSE
		printf( "(render_tile_get: was at %dx%d, moving to %dx%d)\n",
			tile->area.left, tile->area.top,
			area->left, area->top );
#endif /*DEBUG_REUSE*/
	}

#ifdef DEBUG_PAINT
	printf( "render_tile_get: sending %dx%d for calc\n",
		area->left, area->top );
#endif /*DEBUG_PAINT*/

	tile_set_dirty( tile, area );

	return( tile );
}

/* Copy what we can from the tile into the region.
 */
static void
tile_copy( Tile *tile, REGION *to )
{
	Rect ovlap;
	int y;
	int len;

	/* Find common pixels.
	 */
	im_rect_intersectrect( &tile->area, &to->valid, &ovlap );
	assert( !im_rect_isempty( &ovlap ) );
	len = IM_IMAGE_SIZEOF_PEL( to->im ) * ovlap.width;

	/* If the tile is painted, copy over the pixels. Otherwise, fill with
	 * zero. 
	 */
	if( tile->state == TILE_PAINTED || 
		tile->state == TILE_PAINTED_FADING ) {
#ifdef DEBUG_PAINT
		printf( "tile_copy: copying calculated pixels for %dx%d\n",
			tile->area.left, tile->area.top ); 
#endif /*DEBUG_PAINT*/

		for( y = ovlap.top; y < IM_RECT_BOTTOM( &ovlap ); y++ ) {
			PEL *p = (PEL *) IM_REGION_ADDR( tile->region, 
				ovlap.left, y );
			PEL *q = (PEL *) IM_REGION_ADDR( to, ovlap.left, y );

			memcpy( q, p, len );
		}
	}
	else {
#ifdef DEBUG_PAINT
		printf( "tile_copy: zero filling for %dx%d\n",
			tile->area.left, tile->area.top ); 
#endif /*DEBUG_PAINT*/

		for( y = ovlap.top; 
			y < IM_RECT_BOTTOM( &ovlap ); y++ ) {
			PEL *q = (PEL *) 
				IM_REGION_ADDR( to, ovlap.left, y );

			memset( q, 0, len );
		}
	}
}

/* Loop over the output region, filling with data from cache.
 */
static int
region_fill( REGION *out, void *seq, void *a, void *b )
{
	Render *render = (Render *) a;
	Rect *r = &out->valid;
	int x, y;

	/* Find top left of tiles we need.
	 */
	int xs = (r->left / render->width) * render->width;
	int ys = (r->top / render->height) * render->height;

	/* Only allow one reader. No point threading this, calculation is
	 * decoupled anyway.
	 */
	g_mutex_lock( render->read_lock );

	/* 

		FIXME ... if r fits inside a single tile, could skip the copy.

	 */

	for( y = ys; y < IM_RECT_BOTTOM( r ); y += render->height )
		for( x = xs; x < IM_RECT_RIGHT( r ); x += render->width ) {
			Rect area;
			Tile *tile;

			area.left = x;
			area.top = y;
			area.width = render->width;
			area.height = render->height;

			if( (tile = render_tile_get( render, &area )) )
				tile_copy( tile, out );
		}

	g_mutex_unlock( render->read_lock );

	return( 0 );
}

/* Paint the state of the 'painted' flag for a tile. Fade too.
 */
static void
tile_paint_mask( Tile *tile, REGION *to )
{
	int mask;

	Rect ovlap;
	int y;
	int len;

	/* Find common pixels.
	 */
	im_rect_intersectrect( &tile->area, &to->valid, &ovlap );
	assert( !im_rect_isempty( &ovlap ) );
	len = IM_IMAGE_SIZEOF_PEL( to->im ) * ovlap.width;

	if( tile->state == TILE_PAINTED_FADING ) {
		Render *render = tile->render;
		int age = render->time - tile->time;
		double opacity = IM_CLIP( 0, (double) age / render->steps, 1 );

		mask = 255 * pow( opacity, 4 );
	}
	else if( tile->state == TILE_PAINTED )
		mask = 255;
	else 
		mask = 0;

	for( y = ovlap.top; y < IM_RECT_BOTTOM( &ovlap ); y++ ) {
		PEL *q = (PEL *) IM_REGION_ADDR( to, ovlap.left, y );

		memset( q, mask, len );
	}
}

/* The mask image is 255 .. 0 for the state of painted for each tile.
 */
static int
mask_fill( REGION *out, void *seq, void *a, void *b )
{
	Render *render = (Render *) a;
	Rect *r = &out->valid;
	int x, y;

	/* Find top left of tiles we need.
	 */
	int xs = (r->left / render->width) * render->width;
	int ys = (r->top / render->height) * render->height;

	g_mutex_lock( render->read_lock );

	for( y = ys; y < IM_RECT_BOTTOM( r ); y += render->height )
		for( x = xs; x < IM_RECT_RIGHT( r ); x += render->width ) {
			Rect area;
			Tile *tile;

			area.left = x;
			area.top = y;
			area.width = render->width;
			area.height = render->height;

			if( (tile = render_tile_get( render, &area )) )
				tile_paint_mask( tile, out );
		}

	g_mutex_unlock( render->read_lock );

	return( 0 );
}

int
im_render_fade( IMAGE *in, IMAGE *out, IMAGE *mask, 
	int width, int height, int max, 
	int fps, int steps,
	int priority,
	notify_fn notify, void *client )
{
	Render *render;

	/* Make sure the bg work threads are ready.
	 */
	if( render_thread_create() )
		return( -1 );

	/* Don't allow fps == 0, since we divide by this value to get wait
	 * time.
	 */
	if( width <= 0 || height <= 0 || max < -1 || fps <= 0 || steps < 0 ) {
		im_error( "im_render", _( "bad parameters" ) );
		return( -1 );
	}
	if( im_pincheck( in ) || im_poutcheck( out ) )
		return( -1 );
	if( mask ) {
		if( im_poutcheck( mask ) ||
			im_cp_desc( mask, in ) )
			return( -1 );

		mask->Bands = 1;
		mask->BandFmt = IM_BANDFMT_UCHAR;
		mask->Bbits = IM_BBITS_BYTE;
		mask->Type = IM_TYPE_B_W;
		mask->Coding = IM_CODING_NONE;
	}
	if( im_cp_desc( out, in ) )
		return( -1 );
	if( im_demand_hint( out, IM_SMALLTILE, in, NULL ) )
		return( -1 );
	if( mask && 
		im_demand_hint( mask, IM_SMALLTILE, in, NULL ) )
		return( -1 );

	if( !(render = render_new( in, out, mask, 
		width, height, max, fps, steps, priority, notify, client )) )
		return( -1 );

#ifdef DEBUG_MAKE
	printf( "im_render: max = %d, %p\n", max, render );
#endif /*DEBUG_MAKE*/

	if( im_generate( out, NULL, region_fill, NULL, 
		render, NULL ) )
		return( -1 );
	if( mask && 
		im_generate( mask, NULL, mask_fill, NULL, 
		render, NULL ) )
		return( -1 );

	return( 0 );
}

int
im_render( IMAGE *in, IMAGE *out, IMAGE *mask, 
	int width, int height, int max, notify_fn notify, void *client )
{
	return( im_render_fade( in, out, mask, 
		width, height, max, 10, 0, 0, notify, client ) );
}

int 
im_cache( IMAGE *in, IMAGE *out, int width, int height, int max )
{
	if( im_render( in, out, NULL, width, height, max, NULL, NULL ) )
		return( -1 );

	return( 0 );
}
