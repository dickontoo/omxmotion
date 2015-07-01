/* omxmotion.h */
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

#ifndef __OMXMOTION_H
#define __OMXMOTION_H

#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif
#define FF_API_CODEC_ID 1

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <time.h>
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 1
#endif

#include "bcm_host.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavcodec/avcodec.h"
#include "libavutil/mathematics.h"
#include "libavformat/avio.h"
#include <error.h>

#include "OMX_Video.h"
#include "OMX_Types.h"
#include "OMX_Component.h"
#include "OMX_Core.h"
#include "OMX_Broadcom.h"
#include "OMX_Index.h"

#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <fcntl.h>

#include <errno.h>

#include <unistd.h>

/* For htonl(): */
#include <arpa/inet.h>


#define INMEMFRAMES	(128)
#define IFRAMEAFTER	(64)
#define DEBOUNCE	(12)


enum recstate {
	waiting,
	triggered,
	recording,
	stopping,
};



struct frame {
	uint8_t		*buf;
	int		len;
	OMX_TICKS	tick;
	int		flags;
	time_t		time;
};



struct context {
	AVFormatContext *coc;
	int		cocvidindex;
	volatile int	flags;
	OMX_BUFFERHEADERTYPE *encbufs, *bufhead;
	OMX_HANDLETYPE	clk, cam, enc, nul;
	pthread_mutex_t	lock;
	pthread_cond_t	cond;
	pthread_cond_t	framecond;
	AVBitStreamFilterContext *bsfc;
	char		*subs;
	enum recstate	recstate;
	pthread_t	recthread;
	int		width, height;
	int		bitrate;
	int		framerate;
	int		verbosity;
	int64_t		ptsoff;
	char		*outdir;
	uint8_t		*sps;
	int		spslen;
	uint8_t		*pps;
	int		ppslen;
	int		waiting;
	unsigned int	framenum;
	unsigned int	lastiframe;
	unsigned int	previframe;
	char		*dumppattern;
	int		debounce;
	int		outro;
	unsigned int	lastevent;
	struct frame	frames[INMEMFRAMES];
	int		fd;
	char		*command;
};
#define FLAGS_VERBOSE		(1<<0)
#define FLAGS_RECORDING		(1<<1)
#define FLAGS_MONITOR		(1<<2)
#define FLAGS_RAW		(1<<4)
#define FLAGS_NOSUBS		(1<<5)


extern struct context ctx;


#endif /* __OMXMOTION_H */
