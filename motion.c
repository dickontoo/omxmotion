/* motion.h */
/*
 * (c) 2015 Dickon Hood <dickon@fluff.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Motion detection.
 *
 * This is the fun bit.  The basic principle is simple: take the motion
 * vector map from the encoder, and see if it exceeds our thresholds,
 * triggering recording.  To do this, we can either take a static
 * sensitivity (across the whole frame), or the filename of an 8bpp
 * greyscale bitmap; either way, transform this into a 16b/mb 'heatmap',
 * which we then test against.  If more than mctx.threshold macroblocks
 * exceed their sensitivity ratings, trigger a recording.
 *
 * Simple.
 *
 * To speed things up a bit, we pre-square the heatmap, and don't bother
 * sqrt()ing the magnitude vectors to find the actual hypoteneuse.  This
 * turns the critical section into 14,400 byte loads and multiplies, 7,200
 * half-word loads, and a bit of trivial maths.  Or would be, if I wrote it
 * in assembly; the compiler does a very poor job.
 *
 */

#include "omxmotion.h"
#include "motion.h"
#include <png.h>

static void *motionstart(void *);



struct motvec {
	int8_t			dx;
	int8_t			dy;
	uint16_t		sad;
};



static struct {
	pthread_cond_t		cond;
	pthread_mutex_t		lock;
	int			width, height;
	uint16_t		*map;
	struct motvec		*vectors;
	int			threshold;
	pthread_t		detectionthread;
#define FLAGS_MOVEMENT		(1<<0)
	int			flags;
	void 			(*eventcb)(void *, enum movementevents);
	void			*eventcbp;
	char			*pngfn;
} mctx;



static int readmap(char *mf)
{
	FILE *fd;
	uint8_t header[8];
	png_structp png;
	png_infop info, end;
	png_bytep *rows;
	int i, j;

	fd = fopen(mf, "rb");
	if (!fd)
		return -1;
	fread(header, 1, sizeof(header), fd);
	if (png_sig_cmp(header, 0, sizeof(header)))
		return -1;
	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png)
		return -1;
	info = png_create_info_struct(png);
	if (!info)
		return -1;
	end = png_create_info_struct(png);
	if (!end)
		return -1;
	png_init_io(png, fd);
	png_set_sig_bytes(png, sizeof(header));
	png_read_png(png, info, PNG_TRANSFORM_STRIP_16 |
		PNG_TRANSFORM_STRIP_ALPHA | PNG_TRANSFORM_PACKING, NULL);
	rows = png_get_rows(png, info);
	for (i = 0; i < mctx.height; i++) {
		uint8_t *r = rows[i];
		for (j = 0; j < mctx.width; j++) {
			mctx.map[i*(mctx.width + 1) + j] =
				(uint16_t) (r[j] * r[j]);
		}
		mctx.map[i*(mctx.width + 1) + mctx.width] = 65535;
	}
	png_destroy_read_struct(&png, &info, &end);
	fclose(fd);

	return 0;
}



int initmotion(struct context *ctx, char *map, int sens, int thresh,
	void(*eventcb)(void *, enum movementevents), void *cbp)
{
	int rows, cols;
	int x, y;
	pthread_attr_t detach;

	memset(&mctx, 0, sizeof(mctx));

	mctx.height = rows = (ctx->height + 15) / 16;
	mctx.width = cols = ((ctx->width + 15) / 16) + 1;
	mctx.map = (uint16_t *) malloc((sizeof(uint16_t)) * (cols+1)*rows);
	mctx.threshold = thresh; //(rows * cols * thresh) / 100;
	mctx.pngfn = ctx->dumppattern;

	if (map) {
		printf("Reading mapfile %s\n", map);
		if (readmap(map) != 0) {
			printf("Failed to read mapfile: %s\n",
				strerror(errno));
			return -1;
		}
	} else {
		sens = sens * sens;
		if (sens > 65535)
			sens = 65535;
		for (y = 0; y < rows; y++) {
			for (x = 0; x < cols-1; x++) {
				mctx.map[y*cols + x] = (uint16_t) sens;
			}
			mctx.map[y*cols + cols] = 65535;
		}
	}

	mctx.eventcb = eventcb;
	mctx.eventcbp = cbp;

	pthread_mutex_init(&mctx.lock, NULL);
	pthread_cond_init(&mctx.cond, NULL);
	pthread_attr_init(&detach);
	pthread_attr_setdetachstate(&detach, PTHREAD_CREATE_DETACHED);	
	pthread_create(&mctx.detectionthread, &detach, motionstart, NULL);

	return 0;
}



void dumppng(struct motvec *v)
{
	int i, j;
	static int fnum = 0;
	static png_bytep *rows;
	FILE *fd;
	png_structp png;
	png_infop info;
	static char fn[256];

	if (fnum == 0) {
		rows = malloc(mctx.height * sizeof(png_bytep));
		for (i = 0; i < mctx.height; i++) {
			rows[i] = (png_bytep) malloc(mctx.width);
		}
	}

	for (i = 0; i < mctx.height; i++) {
		for (j = 0; j < mctx.width; j++) {
			uint8_t *t;
			struct motvec *tv;
			t = (uint8_t *) rows[i];
			tv = &v[(i * mctx.width) + j];
			t[j] = sqrt((double)
				((tv->dx * tv->dx) + (tv->dy * tv->dy)));
		}
	}

	png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info = png_create_info_struct(png);
	sprintf(fn, mctx.pngfn, fnum);
printf("Writing vector frame %s\n", fn);
	fd = fopen(fn, "wb");
	png_init_io(png, fd);
	png_set_compression_level(png, 0);
	png_set_IHDR(png, info, mctx.width-1, mctx.height, 8,
		PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_set_rows(png, info, rows);
	png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	fclose(fd);
//	sprintf(fn, "vo/img%05d.raw", fnum);
//	fd = fopen(fn, "wb");
//	fwrite(v, mctx.width * mctx.height, sizeof(struct motvec), fd);
//	fclose(fd);
	fnum++;
}



static void lookformotion(struct motvec *v)
{
	int i;
	int n;
	int t;

	n = mctx.width * mctx.height;

	for (i = t = 0; i < n /* && t < mctx.threshold */; i++) {
		if (mctx.map[i] <
			((v[i].dx * v[i].dx) + (v[i].dy * v[i].dy))) {
			t++;
		}
	}

	if (mctx.pngfn)
		dumppng(v);

printf("\r%5d / %d.", t, mctx.threshold);
fflush(stdout);
	if (t >= mctx.threshold) {
		if (mctx.flags & FLAGS_MOVEMENT) {
			/* Do nothing */
			return;
		}
		mctx.flags |= FLAGS_MOVEMENT;
		mctx.eventcb(mctx.eventcbp, movement);
	} else {
		if ((mctx.flags & FLAGS_MOVEMENT) == 0) {
			return;
		}
		mctx.flags &= ~FLAGS_MOVEMENT;
		mctx.eventcb(mctx.eventcbp, quiescent);
	}
}



static void *motionstart(void *args)
{
	struct motvec *tv;

	while (1) {
		pthread_mutex_lock(&mctx.lock);
		pthread_cond_wait(&mctx.cond, &mctx.lock);
		tv = mctx.vectors;
		mctx.vectors = NULL;
		pthread_mutex_unlock(&mctx.lock);
		lookformotion(tv);
		av_free(tv);
	}

	return NULL; /* to shut the compiler up */
}



void findmotion(uint8_t *b)
{
	pthread_mutex_lock(&mctx.lock);
	if (mctx.vectors)
		av_free(mctx.vectors);
	mctx.vectors = (struct motvec *) b;
	pthread_cond_signal(&mctx.cond);
	pthread_mutex_unlock(&mctx.lock);
}
