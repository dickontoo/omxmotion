/* omxmotion.c
 *
 * (c) 2012, 2013, 2015 Dickon Hood <dickon@fluff.org>
 *
 * Usage: ./omxmotion -d outputdir -s [0..255] -t [0..100] -r rate
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

/* To do:
 *
 *  *  Move to an event-driven setup, rather than waiting.
 */

/*
 * Usage: see usage()
 *
 * Additional notes: see README.md
 */


/* To shut color_coded / clang up: */
#ifndef OMX_SKIP64BIT
#define OMX_SKIP64BIT
#endif

#include <time.h>
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 1
#endif
#include "omxmotion.h"
#include "motion.h"
#include <unistd.h>
#include <signal.h>

extern char *optarg;

static OMX_VERSIONTYPE SpecificationVersion = {
	.s.nVersionMajor = 1,
	.s.nVersionMinor = 1,
	.s.nRevision     = 2,
	.s.nStep         = 0
};

/* Hateful things: */
#define MAKEME(y, x)	do {						\
				y = calloc(1, sizeof(x));		\
				y->nSize = sizeof(x);			\
				y->nVersion = SpecificationVersion;	\
			} while (0)


#define OERR(cmd)	do {						\
				OMX_ERRORTYPE oerr = cmd;		\
				ctx.waiting = 0; \
				if (oerr != OMX_ErrorNone) {		\
					fprintf(stderr, #cmd		\
						" failed on line %d: %x\n", \
						__LINE__, oerr);	\
					exit(1);			\
				} else {				\
					fprintf(stderr, #cmd		\
						" completed at %d.\n",	\
						__LINE__);		\
				}					\
			} while (0)

#define OERRw(cmd)	do {						\
				pthread_mutex_lock(&ctx.lock);		\
				ctx.waiting = 0; \
				OERR(cmd);				\
				printf("Waiting: %d\n", ctx.waiting);	\
				if (ctx.waiting) {			\
					ctx.waiting = 1;		\
					pthread_cond_wait(&ctx.cond, &ctx.lock);\
				}					\
				ctx.waiting = 0;			\
				pthread_mutex_unlock(&ctx.lock);	\
			} while (0)

#define OERRq(cmd)	do {	oerr = cmd;				\
				if (oerr != OMX_ErrorNone) {		\
					fprintf(stderr, #cmd		\
						" failed: %x\n", oerr);	\
					exit(1);			\
				}					\
			} while (0)

#define WAIT usleep(500000)
/* ... but damn useful.*/

#define V_ALWAYS	0
#define V_INFO		1
#define V_LOTS		2

/* Not vprintf(); there's already one of those in libc... */
#define logprintf(v, ...) \
    do { \
        if ((v) <= ctx.verbosity) { \
            printf( __VA_ARGS__); \
        } \
    } while (0)


/* Hardware component names: */
#define CLKNAME "OMX.broadcom.clock"
#define CAMNAME "OMX.broadcom.camera"
#define ENCNAME "OMX.broadcom.video_encode"
#define NULNAME "OMX.broadcom.null_sink"

/*
 * Portbase for the modules, could also be queried, but as the components
 * are broadcom/raspberry specific anyway...
 */
#define PORT_CLK  80
#define PORT_CAM  70
#define PORT_ENC 200
#define PORT_NUL 240



struct packetentry {
	TAILQ_ENTRY(packetentry) link;
	AVPacket packet;
};

TAILQ_HEAD(packetqueue, packetentry);

static struct packetqueue packetq;

struct context ctx;



static OMX_BUFFERHEADERTYPE *allocbufs(OMX_HANDLETYPE h, int port, int enable);


/* Print some useful information about the state of the port: */
static void dumpport(OMX_HANDLETYPE handle, int port)
{
	OMX_VIDEO_PORTDEFINITIONTYPE	*viddef;
	OMX_PARAM_PORTDEFINITIONTYPE	*portdef;

	MAKEME(portdef, OMX_PARAM_PORTDEFINITIONTYPE);
	portdef->nPortIndex = port;
	OERR(OMX_GetParameter(handle, OMX_IndexParamPortDefinition, portdef));

	printf("Port %d is %s, %s\n", portdef->nPortIndex,
		(portdef->eDir == 0 ? "input" : "output"),
		(portdef->bEnabled == 0 ? "disabled" : "enabled"));
	printf("Wants %d bufs, needs %d, size %d, enabled: %d, pop: %d, "
		"aligned %d, domain %d\n", portdef->nBufferCountActual,
		portdef->nBufferCountMin, portdef->nBufferSize,
		portdef->bEnabled, portdef->bPopulated,
		portdef->nBufferAlignment,portdef->eDomain);
	viddef = &portdef->format.video;

	switch (portdef->eDomain) {
	case OMX_PortDomainVideo:
		printf("Video type is currently:\n"
			"\tMIME:\t\t%s\n"
			"\tNative:\t\t%p\n"
			"\tWidth:\t\t%d\n"
			"\tHeight:\t\t%d\n"
			"\tStride:\t\t%d\n"
			"\tSliceHeight:\t%d\n"
			"\tBitrate:\t%d\n"
			"\tFramerate:\t%d (%x); (%f)\n"
			"\tError hiding:\t%d\n"
			"\tCodec:\t\t%d\n"
			"\tColour:\t\t%d\n",
			viddef->cMIMEType, viddef->pNativeRender,
			viddef->nFrameWidth, viddef->nFrameHeight,
			viddef->nStride, viddef->nSliceHeight,
			viddef->nBitrate,
			viddef->xFramerate, viddef->xFramerate,
			((float)viddef->xFramerate/(float)65536),
			viddef->bFlagErrorConcealment,
			viddef->eCompressionFormat, viddef->eColorFormat);
		break;
	case OMX_PortDomainImage:
		printf("Image type is currently:\n"
			"\tMIME:\t\t%s\n"
			"\tNative:\t\t%p\n"
			"\tWidth:\t\t%d\n"
			"\tHeight:\t\t%d\n"
			"\tStride:\t\t%d\n"
			"\tSliceHeight:\t%d\n"
			"\tError hiding:\t%d\n"
			"\tCodec:\t\t%d\n"
			"\tColour:\t\t%d\n",
			portdef->format.image.cMIMEType,
			portdef->format.image.pNativeRender,
			portdef->format.image.nFrameWidth,
			portdef->format.image.nFrameHeight,
			portdef->format.image.nStride,
			portdef->format.image.nSliceHeight,
			portdef->format.image.bFlagErrorConcealment,
			portdef->format.image.eCompressionFormat,
			portdef->format.image.eColorFormat); 		
		break;
/* Feel free to add others. */
	default:
		break;
	}

	free(portdef);
}



static const char *mapcomponent(struct context *ctx, OMX_HANDLETYPE h)
{
	if (h == ctx->enc)
		return "Encoder";
	if (h == ctx->clk)
		return "Clock";
	if (h == ctx->cam)
		return "Camera";
	if (h == ctx->nul)
		return "NULL sink";
	return "Unknown!";
}



static void writeframe(AVFormatContext *oc, struct frame *f, int index)
{
	AVPacket pkt;
	int r;
	AVRational omxtimebase = { 1, 1000000 };

	if (ctx.fd != -1) {
		write(ctx.fd, f->buf, f->len);
		free(f->buf);
		f->buf = NULL;
		return;
	}

	memset(&pkt, 0, sizeof(pkt));
	av_init_packet(&pkt);

	if (f->flags & OMX_BUFFERFLAG_SYNCFRAME)
		pkt.flags |= AV_PKT_FLAG_KEY;

	pkt.pts = av_rescale_q(((((uint64_t) f->tick.nHighPart)<<32) |
		f->tick.nLowPart), omxtimebase, oc->streams[0]->time_base);

	pkt.dts = pkt.pts; // AV_NOPTS_VALUE; // dts;
	pkt.stream_index = index;
	pkt.data = f->buf;
	pkt.size = f->len;

	r = av_write_frame(oc, &pkt);
	if (r != 0) {
		char err[256];
		av_strerror(r, err, sizeof(err));
		printf("Failed to write a video frame: %s\n", err);
	}
	av_write_frame(oc, NULL);
}



static AVFormatContext *openoutput(char *url, int *index)
{
	int			r;
	AVFormatContext		*oc;
	char			err[256];
	AVOutputFormat		*fmt;
	int			i;
	AVStream		*oflow;
	AVCodec			*c;
	AVCodecContext		*cc;
	AVRational		omxtimebase = { 1, 1000000 };
	AVRational		framerate;
	struct frame		*f;

//	ctx.fd = open(err, O_CREAT|O_LARGEFILE|O_RDWR, 0666);
	if (ctx.fd != -1) {
		write(ctx.fd, ctx.sps, ctx.spslen);
		write(ctx.fd, ctx.pps, ctx.ppslen);
	}

//	fmt = av_guess_format("matroska", err, "video/x-matroska");
//	fmt = av_guess_format("mp4", err, "video/mp4");
//	fmt = av_guess_format("mpegts", url, "video/MP2TS");
	if (strstr(url, "://")) {
		avformat_network_init();
		fmt = av_guess_format("mpegts", url, "video/MP2TS");
	} else {
		fmt = av_guess_format(NULL, url, NULL);
	}

	if (!fmt) {
		fprintf(stderr, "Failed.  Bye bye.\n");
		exit(1);
	}

	ctx.oc = oc = avformat_alloc_context();
	if (!oc) {
		fprintf(stderr, "Failed to alloc outputcontext\n");
		exit(1);
	}
	oc->oformat = fmt;
	strcpy(oc->filename, url);
	oc->debug = 1;
	oc->duration = 0;
	oc->start_time = 0;
	oc->start_time_realtime = time(NULL) * 1000000;
	oc->bit_rate = ctx.bitrate;

	c = avcodec_find_encoder(CODEC_ID_H264);
	oflow = avformat_new_stream(oc, c);
	cc = oflow->codec;
	cc->width = ctx.width;
	cc->height = ctx.height;
	cc->codec_id = CODEC_ID_H264;
	cc->codec_type = AVMEDIA_TYPE_VIDEO;
	cc->bit_rate = ctx.bitrate;
	cc->profile = FF_PROFILE_H264_HIGH;
	cc->level = 41;
	cc->time_base = omxtimebase;
	oflow->time_base = omxtimebase;
	*index = oflow->index;

	if (ctx.spslen + ctx.ppslen > 0) {
		if (cc->extradata) {
			av_free(cc->extradata);
		}
		cc->extradata_size = ctx.spslen + ctx.ppslen;
		cc->extradata = av_malloc(ctx.spslen + ctx.ppslen);
		memcpy(cc->extradata, ctx.sps, ctx.spslen);
		memcpy(&cc->extradata[ctx.spslen], ctx.pps, ctx.ppslen);
	}

	framerate.num = ctx.framerate;
	framerate.den = 1;
	oflow->avg_frame_rate = framerate;
	oflow->r_frame_rate = framerate;
	f = &ctx.frames[ctx.previframe & (INMEMFRAMES-1)];
	oflow->start_time = av_rescale_q(((((uint64_t) f->tick.nHighPart)<<32) | 
		f->tick.nLowPart), omxtimebase, oflow->time_base);
	for (i = 0; i < oc->nb_streams; i++) {
		if (oc->oformat->flags & AVFMT_GLOBALHEADER)
			oc->streams[i]->codec->flags
				|= CODEC_FLAG_GLOBAL_HEADER;
		if (oc->streams[i]->codec->sample_rate == 0)
			oc->streams[i]->codec->sample_rate = 48000; /* ish */
	}

/* At some point they changed the API: */
#ifndef URL_WRONLY
#define URL_WRONLY AVIO_FLAG_WRITE
#endif
	avio_open(&oc->pb, url, URL_WRONLY);

	printf("\n");
	r = avformat_write_header(oc, NULL);
	if (r < 0) {
		av_strerror(r, err, sizeof(err));
		fprintf(stderr, "Failed to write header: %s\n", err);
		return NULL;
	}
	av_dump_format(oc, 0, url, 1);


	return oc;
}



static void motioncallback(void *context, enum movementevents state)
{
	context = context;	/* Shush */
	pthread_mutex_lock(&ctx.lock);
	printf("\nmotioncallback(%d) called at frame %d\n", state, ctx.framenum);
	ctx.lastevent = ctx.framenum;
	if (state == quiescent) {
		switch (ctx.recstate) {
		case waiting:
			break;
		case triggered:
			ctx.recstate = waiting;
			break;
		case recording:
			ctx.recstate = stopping;
			break;
		case stopping:
			break;
		}
	} else if (state == movement) {
		switch (ctx.recstate) {
		case waiting:
			ctx.recstate = triggered;
			break;
		case triggered:
			break;
		case recording:
			break;
		case stopping:
			ctx.recstate = recording;
			break;
		}
	}
	pthread_mutex_unlock(&ctx.lock);
}



OMX_ERRORTYPE genericeventhandler(OMX_HANDLETYPE component,
				struct context *ctx,
				OMX_EVENTTYPE event,
				OMX_U32 data1,
				OMX_U32 data2,
				OMX_PTR eventdata)
{
	printf("Event %d on %p\n", event, component);
	fflush(stdout);

	if (ctx->waiting) {
		pthread_mutex_lock(&ctx->lock);
		pthread_cond_signal(&ctx->cond);
	}
	ctx->waiting = 0;

	switch (event) {
	case OMX_EventError:
		if (ctx->flags & FLAGS_VERBOSE)
			printf("%s %p has errored: %x\n",
				mapcomponent(ctx, component),
				component, data1);
		return data1;
		break;
	case OMX_EventCmdComplete:
		if (ctx->flags & FLAGS_VERBOSE)
			printf("%s %p has completed the last command.\n",
				mapcomponent(ctx, component), component);
		break;
	case OMX_EventPortSettingsChanged: {
//	if (ctx->flags & FLAGS_VERBOSE)
		printf("%s %p port %d settings changed.\n",
			mapcomponent(ctx, component), component, data1);
		dumpport(component, data1);
	}
		break;
	default:
		if (ctx->flags & FLAGS_VERBOSE)
			printf("Got an event of type %x on %s %p "
				"(d1: %x, d2 %x)\n", event,
				mapcomponent(ctx, component), component,
				data1, data2);
	}
	
	return OMX_ErrorNone;
}



OMX_ERRORTYPE enceventhandler(OMX_HANDLETYPE component,
				struct context *ctx,
				OMX_EVENTTYPE event, 
				OMX_U32 data1,
				OMX_U32 data2,
				OMX_PTR eventdata)
{
	return genericeventhandler(component, ctx, event, data1, data2,
		eventdata);
}



OMX_ERRORTYPE filled(OMX_HANDLETYPE component,
				struct context *ctx,
				OMX_BUFFERHEADERTYPE *buf)
{
	OMX_BUFFERHEADERTYPE *spare;

	if (ctx->flags & FLAGS_VERBOSE)
		printf("Got buffer %p filled (len %d)\n", buf,
			buf->nFilledLen);

/*
 * Don't call OMX_FillThisBuffer() here, as the hardware craps out after
 * a short while.  I don't know why.  Reentrancy, or the like, I suspect.
 * Queue the packet(s) and deal with them in main().
 *
 * It only ever seems to ask for the one buffer, but better safe than sorry...
 */

	pthread_mutex_lock(&ctx->lock);
	if (ctx->bufhead == NULL) {
		buf->pAppPrivate = NULL;
		ctx->bufhead = buf;
		pthread_mutex_unlock(&ctx->lock);
		return OMX_ErrorNone;
	}

	spare = ctx->bufhead;
	while (spare->pAppPrivate != NULL)
		spare = spare->pAppPrivate;

	spare->pAppPrivate = buf;
	buf->pAppPrivate = NULL;
	pthread_mutex_unlock(&ctx->lock);

	return OMX_ErrorNone;
}



OMX_CALLBACKTYPE encevents = {
	(void (*)) enceventhandler,
	(void (*)) NULL,
	(void (*)) filled
};

OMX_CALLBACKTYPE genevents = {
	(void (*)) genericeventhandler,
	(void (*)) NULL,
	(void (*)) filled
};



static OMX_BUFFERHEADERTYPE *allocbufs(OMX_HANDLETYPE h, int port, int enable)
{
	int i;
	OMX_BUFFERHEADERTYPE *list = NULL, **end = &list;
	OMX_PARAM_PORTDEFINITIONTYPE *portdef;

	MAKEME(portdef, OMX_PARAM_PORTDEFINITIONTYPE);
	portdef->nPortIndex = port;
	OERR(OMX_GetParameter(h, OMX_IndexParamPortDefinition, portdef));

	if (enable)
		OERRw(OMX_SendCommand(h, OMX_CommandPortEnable, port, NULL));

	for (i = 0; i < portdef->nBufferCountActual; i++) {
		OMX_U8 *buf;

		buf = vcos_malloc_aligned(portdef->nBufferSize,
			portdef->nBufferAlignment, "buffer");
		printf("Allocated a buffer of %d bytes\n",
			portdef->nBufferSize);
		OERRw(OMX_UseBuffer(h, end, port, NULL, portdef->nBufferSize,
			buf));
		end = (OMX_BUFFERHEADERTYPE **) &((*end)->pAppPrivate);
	}

	free(portdef);

	return list;
}



static void usage(const char *name)
{
	fprintf(stderr, "Usage: %s [-b bitrate] [-d outputdir] [-r framerate ]\n"
		"\t<[-s 0..255] | [-m mapfile.png]> [-t 0..100]\n\n"
		"Where:\n"
	"\t-b bitrate\tTarget bitrate (Mb/s)\n"
	"\t-c url\tContinuous streaming URL\n"
	"\t-d outputdir\tRecordings directory\n"
	"\t-e command\tExecute $command on state change\n"
	"\t-h\t\tThis help\n"
	"\t-m mapfile.png\tHeatmap image\n"
	"\t\tOR:\n"
	"\t-s 0..255\tMacroblock sensitivity\n"
	"\t-o outro\tFrames to record after motion has ceased\n"
	"\t-r rate\t\tEncoding framerate\n"
	"\t-t 0..100\tMacroblocks over threshold to trigger (raw)\n"
	"\t-v\t\tVerbose\n"
	"\t-z pattern\tDump motion vector images (debug)\n"
	"\n", name);
	exit(1);
}



static void run(enum recstate state, char *url)
{
	pid_t pid;

	if (!ctx.command)
		return;
	pid = fork();
	if (pid)
		return;

	execlp(ctx.command, ctx.command,
		(state == recording) ? "start" : "stop",
		url,
		NULL);
	exit(0);
}



static void startrecording(void)
{
	struct tm		tm;
	time_t			t;
	char			url[256];
	AVFormatContext		*oc;
	unsigned int		ftw;
	int			i;

	t = time(NULL);
	localtime_r(&t, &tm);
	snprintf(url, sizeof(url), "%s/%d-%02d-%02dT%02d:%02d:%02d.mkv",
		ctx.outdir, tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec);

	if ((oc = openoutput(url, &ctx.vidindex)) == NULL) {
		ctx.recstate = waiting;
		return;
	}

	ctx.oc = oc;

	ftw = ctx.framenum - ctx.previframe;
	printf("Writing initial %d frames out...\n", ftw);

	for (i = 0; i < ftw; i++) {
		writeframe(oc, &ctx.frames[(ctx.previframe + i) & (INMEMFRAMES-1)],
				ctx.vidindex);
	}

	run(recording, url);
	printf("done.\n");
}



static void stoprecording(void)
{
	printf("\nStopping recording\n");
	if (ctx.fd == -1) {
		av_write_trailer(ctx.oc);
		avcodec_close(ctx.oc->streams[ctx.vidindex]->codec);
		avio_close(ctx.oc->pb);
		run(waiting, ctx.oc->filename);
		avformat_free_context(ctx.oc);
		ctx.oc = NULL;
	} else {
		run(waiting, "");
		close(ctx.fd);
		ctx.fd = -1;
	}

	printf("Done.\n");
}



static void checkstate(struct frame *f)
{
	pthread_mutex_lock(&ctx.lock);
	switch (ctx.recstate) {
	case waiting:
		break;
	case triggered:
		if ((ctx.framenum - ctx.lastevent) > ctx.debounce) {
			ctx.recstate = recording;
			pthread_mutex_unlock(&ctx.lock);
			startrecording();
			return;
		}
		break;
	case recording:
		writeframe(ctx.oc, f, ctx.vidindex);
		break;
	case stopping:
		writeframe(ctx.oc, f, ctx.vidindex);
		if ((ctx.framenum - ctx.lastevent) > ctx.outro &&
			(f->flags & OMX_BUFFERFLAG_SYNCFRAME)) {
			ctx.recstate = waiting;
			pthread_mutex_unlock(&ctx.lock);
			stoprecording();
			return;
		}		
		break;
	}
	pthread_mutex_unlock(&ctx.lock);
}



int main(int argc, char *argv[])
{
	AVFormatContext	*oc;
	int		i;
	OMX_ERRORTYPE	oerr;
	OMX_HANDLETYPE	clk = NULL, cam = NULL, enc = NULL, nul = NULL;
	int		opt;
	uint8_t		*tmpbuf;
	off_t		tmpbufoff;
	int		fd;
	char		*mapfile = NULL;
	int		threshold, sensitivity;
	char		*url = NULL;

/* Various OpenMAX configuration parameters: */
	OMX_VIDEO_PARAM_AVCTYPE		*avc;
	OMX_VIDEO_PARAM_BITRATETYPE	*bitrate;
	OMX_CONFIG_BOOLEANTYPE		*cbool;
	OMX_TIME_CONFIG_CLOCKSTATETYPE	*cstate;
	OMX_CONFIG_FRAMERATETYPE	*frt;
	OMX_PARAM_U32TYPE		*hu32;
	OMX_VIDEO_PARAM_PORTFORMATTYPE	*pfmt;
	OMX_PARAM_PORTDEFINITIONTYPE	*portdef;
	OMX_PORT_PARAM_TYPE		*porttype;
	OMX_CONFIG_PORTBOOLEANTYPE	*obool;
	OMX_CONFIG_REQUESTCALLBACKTYPE	*rcbt;
	OMX_TIME_CONFIG_ACTIVEREFCLOCKTYPE *refclock;
	OMX_PARAM_SENSORMODETYPE	*smt;
	OMX_TIME_CONFIG_SCALETYPE	*timescale;
	OMX_VIDEO_PORTDEFINITIONTYPE	*viddef;
	OMX_BUFFERHEADERTYPE		*spare;

	MAKEME(avc, OMX_VIDEO_PARAM_AVCTYPE);
	MAKEME(bitrate, OMX_VIDEO_PARAM_BITRATETYPE);
	MAKEME(cbool, OMX_CONFIG_BOOLEANTYPE);
	MAKEME(cstate, OMX_TIME_CONFIG_CLOCKSTATETYPE);
	MAKEME(frt, OMX_CONFIG_FRAMERATETYPE);
	MAKEME(hu32, OMX_PARAM_U32TYPE);
	MAKEME(pfmt, OMX_VIDEO_PARAM_PORTFORMATTYPE);
	MAKEME(portdef, OMX_PARAM_PORTDEFINITIONTYPE);
	MAKEME(porttype, OMX_PORT_PARAM_TYPE);
	MAKEME(obool, OMX_CONFIG_PORTBOOLEANTYPE);
	MAKEME(rcbt, OMX_CONFIG_REQUESTCALLBACKTYPE);
	MAKEME(refclock, OMX_TIME_CONFIG_ACTIVEREFCLOCKTYPE);
	MAKEME(smt, OMX_PARAM_SENSORMODETYPE);
	MAKEME(timescale, OMX_TIME_CONFIG_SCALETYPE);

	if (argc < 2)
		usage(argv[0]);

	signal(SIGCHLD, SIG_IGN);

	memset(&ctx, 0, sizeof(ctx));
	ctx.bitrate = 2*1024*1024;
	ctx.framerate = 25;
	ctx.flags = 0; //FLAGS_VERBOSE;
	ctx.width = 1920;
	ctx.height = 1080;
	ctx.outdir = ".";
	ctx.recstate = waiting;
	ctx.debounce = DEBOUNCE;
	ctx.fd = -1;
	ctx.outro = -1;
	threshold = 20;
	sensitivity = 40;
	pthread_cond_init(&ctx.cond, NULL);
	TAILQ_INIT(&packetq);

	while ((opt = getopt(argc, argv, "b:c:d:e:hm:o:r:s:t:vz")) != -1) {
		switch (opt) {
		int l;
		case 'b':
			ctx.bitrate = atoi(optarg)*1024*1024;
			break;
		case 'c':
			url = optarg;
			break;
		case 'd':
			l = strlen(optarg)+1;
			ctx.outdir = malloc(l);
			memcpy(ctx.outdir, optarg, l);
			break;
		case 'e':
			ctx.command = optarg;
			break;
		case 'h':
			usage(argv[0]);
			break;
		case 'm':
			mapfile = optarg;
			break;
		case 'o':
			ctx.outro = atoi(optarg);
			break;
		case 'r':
			ctx.framerate = atoi(optarg);
			break;
		case 's':
			sensitivity = atoi(optarg);
			break;
		case 't':
			threshold = atoi(optarg);
			break;
		case 'v':
			ctx.flags |= FLAGS_VERBOSE;
			break;
		case 'z':
			ctx.flags |= FLAGS_DUMPVECTORIMAGES;
			ctx.dumppattern = optarg;
			break;
		default:
			usage(argv[0]);
		}
	}

	if (ctx.outro == -1)
		ctx.outro = ctx.framerate * 2;

	initmotion(&ctx, mapfile, sensitivity, threshold, motioncallback,
		NULL);

	av_register_all();

	pthread_mutex_init(&ctx.lock, NULL);

/* Initialise OMX: */
	bcm_host_init();
	OERR(OMX_Init());
	OERR(OMX_GetHandle(&clk, CLKNAME, &ctx, &genevents));
	OERR(OMX_GetHandle(&cam, CAMNAME, &ctx, &genevents));
	OERR(OMX_GetHandle(&enc, ENCNAME, &ctx, &encevents));
	OERR(OMX_GetHandle(&nul, NULNAME, &ctx, &genevents));
	ctx.clk = clk;
	ctx.cam = cam;
	ctx.enc = enc;
	ctx.nul = nul;

/* Disable all ports.  Why this isn't the default I don't know... */
	for (i = 0; i < 6; i++)
		OERRw(OMX_SendCommand(clk, OMX_CommandPortDisable, PORT_CLK+i,
			NULL));
	for (i = 0; i < 4; i++)
		OERRw(OMX_SendCommand(cam, OMX_CommandPortDisable, PORT_CAM+i,
			NULL));
	OERRw(OMX_SendCommand(enc, OMX_CommandPortDisable, PORT_ENC,   NULL));
	OERRw(OMX_SendCommand(enc, OMX_CommandPortDisable, PORT_ENC+1, NULL));
	OERRw(OMX_SendCommand(nul, OMX_CommandPortDisable, PORT_NUL,   NULL));
	OERRw(OMX_SendCommand(nul, OMX_CommandPortDisable, PORT_NUL+1, NULL));
	OERRw(OMX_SendCommand(nul, OMX_CommandPortDisable, PORT_NUL+2, NULL));

/* Configure the camera: */
	rcbt->nPortIndex = OMX_ALL;
	rcbt->nIndex = OMX_IndexParamCameraDeviceNumber;
	rcbt->bEnable = OMX_TRUE;
	OERR(OMX_SetConfig(cam, OMX_IndexConfigRequestCallback, rcbt));
	hu32->nPortIndex = OMX_ALL;
	hu32->nU32 = 0;
	OERR(OMX_SetParameter(cam, OMX_IndexParamCameraDeviceNumber, hu32));

	obool->nPortIndex = PORT_CAM + 1;
	obool->bEnabled = OMX_TRUE;
	OERR(OMX_SetConfig(cam, OMX_IndexConfigPortCapturing, obool));

	portdef->nPortIndex = PORT_CAM+1;
	OERR(OMX_GetParameter(cam, OMX_IndexParamPortDefinition, portdef));
	viddef = &portdef->format.video;
	viddef->nFrameWidth = ctx.width;
	viddef->nFrameHeight = ctx.height;
	viddef->nStride = 0;
	viddef->xFramerate = (ctx.framerate<<16);
	viddef->nSliceHeight = (ctx.height + 15) & ~15;
	OERR(OMX_SetParameter(cam, OMX_IndexParamPortDefinition, portdef));
/* (To set the stride correctly:) */
	OERR(OMX_GetParameter(cam, OMX_IndexParamPortDefinition, portdef));
	portdef->nPortIndex = PORT_CAM + 0;
	OERR(OMX_SetParameter(cam, OMX_IndexParamPortDefinition, portdef));
	smt->nPortIndex = OMX_ALL;
	smt->sFrameSize.nPortIndex = OMX_ALL;
	OERR(OMX_GetParameter(cam, OMX_IndexParamCommonSensorMode, smt));
	printf("Sensor mode: framerate %d (%x), oneshot: %d\n",
		smt->nFrameRate, smt->nFrameRate, smt->bOneShot);
	smt->bOneShot = 0;
	smt->nFrameRate = ctx.framerate<<16;
	OERR(OMX_SetParameter(cam, OMX_IndexParamCommonSensorMode, smt));
	frt->nPortIndex = PORT_CAM + 1;
	OERR(OMX_GetConfig(cam, OMX_IndexConfigVideoFramerate, frt));
	printf("Alleged framerate: %d (%x)\n",
		frt->xEncodeFramerate, frt->xEncodeFramerate);
	frt->xEncodeFramerate = ctx.framerate << 16;
	OERR(OMX_SetConfig(cam, OMX_IndexConfigVideoFramerate, frt));

/* Initialise the sinks: encoder and null device: */
	portdef->nPortIndex = PORT_ENC;
	viddef->nBitrate = ctx.bitrate;
	viddef->nFrameHeight = ctx.height;
	OERRw(OMX_SetParameter(enc, OMX_IndexParamPortDefinition, portdef));
	portdef->nPortIndex = PORT_NUL;
	OERRw(OMX_SetParameter(nul, OMX_IndexParamPortDefinition, portdef));
	viddef->eCompressionFormat = OMX_VIDEO_CodingAVC;
	viddef->eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
	portdef->nPortIndex = PORT_ENC+1;
	OERRw(OMX_SetParameter(enc, OMX_IndexParamPortDefinition, portdef));
	bitrate->nPortIndex = PORT_ENC + 1;
	bitrate->eControlRate = OMX_Video_ControlRateVariable;
	bitrate->nTargetBitrate = ctx.bitrate;
	hu32->nPortIndex = PORT_ENC + 1;
	hu32->nU32 = IFRAMEAFTER;
	OERRw(OMX_SetConfig(enc, OMX_IndexConfigBrcmVideoIntraPeriod, hu32));
	cbool->bEnabled = 1;
	OERRw(OMX_SetConfig(enc, OMX_IndexParamBrcmNALSSeparate, cbool));
	obool->bEnabled = OMX_TRUE;
	obool->nPortIndex = PORT_ENC + 1;
	OERRw(OMX_SetParameter(enc, OMX_IndexParamBrcmVideoAVCInlineHeaderEnable, obool));
	OERRw(OMX_SetParameter(enc,
		OMX_IndexParamBrcmVideoAVCInlineVectorsEnable, obool));
	avc->nPortIndex = PORT_ENC + 1;
	OERRw(OMX_GetParameter(enc, OMX_IndexParamVideoAvc, avc));
	avc->nPFrames = IFRAMEAFTER-1;
	avc->nBFrames = 0;
	avc->nRefFrames = 1;
	avc->nAllowedPictureTypes = OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;
	OERRw(OMX_SetParameter(enc, OMX_IndexParamVideoAvc, avc));

/* The clock doesn't bloody work for some reason.  Ignore this bit. */
#if 0
	timescale->xScale = 1<<16;
	OERR(OMX_SetConfig(clk, OMX_IndexConfigTimeScale, timescale));
	cstate->eState = OMX_TIME_ClockStateWaitingForStartTime;
	cstate->nWaitMask = OMX_CLOCKPORT0;
	OERR(OMX_SetConfig(clk, OMX_IndexConfigTimeClockState, cstate));
	refclock->eClock = OMX_TIME_RefClockVideo;
	OERR(OMX_SetConfig(clk, OMX_IndexConfigTimeActiveRefClock, refclock));
#endif

/* Start the TBMs... */
//	OERR(OMX_SetupTunnel(clk, PORT_CLK,   cam, PORT_CAM+3));
	OERRw(OMX_SetupTunnel(cam, PORT_CAM+1, enc, PORT_ENC));
	OERRw(OMX_SetupTunnel(cam, PORT_CAM,   nul, PORT_NUL));
WAIT;

/* Dump current port states: */
	dumpport(clk, PORT_CLK);
	dumpport(cam, PORT_CAM+3);
	dumpport(cam, PORT_CAM+1);
	dumpport(enc, PORT_ENC);
	dumpport(enc, PORT_ENC+1);

/* Transition to IDLE: */
	OERRw(OMX_SendCommand(clk, OMX_CommandStateSet, OMX_StateIdle, NULL));
	OERRw(OMX_SendCommand(cam, OMX_CommandStateSet, OMX_StateIdle, NULL));
	OERRw(OMX_SendCommand(enc, OMX_CommandStateSet, OMX_StateIdle, NULL));
	OERRw(OMX_SendCommand(nul, OMX_CommandStateSet, OMX_StateIdle, NULL));
WAIT;

/* Enable all relevant ports: */
	OERRw(OMX_SendCommand(clk, OMX_CommandPortEnable, PORT_CLK,   NULL));
	OERRw(OMX_SendCommand(cam, OMX_CommandPortEnable, PORT_CAM,   NULL));
	OERRw(OMX_SendCommand(cam, OMX_CommandPortEnable, PORT_CAM+1, NULL));
//	OERRw(OMX_SendCommand(cam, OMX_CommandPortEnable, PORT_CAM+3, NULL));
	OERRw(OMX_SendCommand(enc, OMX_CommandPortEnable, PORT_ENC,   NULL));
	OERRw(OMX_SendCommand(enc, OMX_CommandPortEnable, PORT_ENC+1, NULL));
	OERRw(OMX_SendCommand(nul, OMX_CommandPortEnable, PORT_NUL,   NULL));
WAIT;

//	allocbufs(cam, PORT_CAM+3, 0);
//	allocbufs(clk, PORT_CLK, 0);
	ctx.encbufs = allocbufs(enc, PORT_ENC+1, 0);
	allocbufs(nul, PORT_NUL, 0);
	allocbufs(cam, PORT_CAM+1, 0);

	dumpport(nul, PORT_NUL);
	dumpport(cam, PORT_CAM+3);
	dumpport(cam, PORT_CAM+1);
	dumpport(enc, PORT_ENC);
	dumpport(enc, PORT_ENC+1);

/* Get going: */
WAIT;
	OERR(OMX_SendCommand(cam, OMX_CommandStateSet, OMX_StateExecuting, NULL));
	OERR(OMX_SendCommand(nul, OMX_CommandStateSet, OMX_StateExecuting, NULL));
	OERR(OMX_SendCommand(enc, OMX_CommandStateSet, OMX_StateExecuting, NULL));
//	OERR(OMX_SendCommand(clk, OMX_CommandStateSet, OMX_StateExecuting, NULL));

	tmpbufoff = 0;
	tmpbuf = NULL;

	dumpport(cam, PORT_CAM+1);

	if (url) {
		ctx.coc = openoutput(url, &ctx.cocvidindex);
	}

	OERR(OMX_FillThisBuffer(enc, ctx.encbufs));
	do {
		pthread_mutex_lock(&ctx.lock);
		spare = ctx.bufhead;
		ctx.bufhead = NULL;
		pthread_mutex_unlock(&ctx.lock);
		if (!spare) {
			usleep(10);
			continue;
		}
		while (spare) {
			struct frame *pkt;
			OMX_TICKS tick = spare->nTimeStamp;

			tmpbuf = av_realloc(tmpbuf,
					tmpbufoff + spare->nFilledLen);
			memcpy(&tmpbuf[tmpbufoff],
					&spare->pBuffer[spare->nOffset],
					spare->nFilledLen);
			tmpbufoff += spare->nFilledLen;

			if (spare->nFlags & OMX_BUFFERFLAG_CODECSIDEINFO) {
				findmotion(tmpbuf);
				tmpbuf = NULL;
				tmpbufoff = 0;
				spare->nFilledLen = 0;
				spare->nOffset = 0;
				OERRq(OMX_FillThisBuffer(enc, spare));
				spare = spare->pAppPrivate;
				continue;
			}

			if ((spare->nFlags & OMX_BUFFERFLAG_ENDOFNAL)
					== 0) {
				spare->nFilledLen = 0;
				spare->nOffset = 0;
				OERRq(OMX_FillThisBuffer(enc, spare));
				spare = spare->pAppPrivate;
				continue;
			}

			pkt = &ctx.frames[ctx.framenum % INMEMFRAMES];
			if (pkt->buf) {
				av_free(pkt->buf);
				pkt->buf = NULL;
			}

			if (spare->nFlags & OMX_BUFFERFLAG_SYNCFRAME) {
				ctx.previframe = ctx.lastiframe;
				ctx.lastiframe = ctx.framenum;
			}

			pkt->buf = tmpbuf;
			pkt->len = tmpbufoff;
			pkt->flags = spare->nFlags;
			pkt->tick = tick;
			tmpbuf = NULL;
			tmpbufoff = 0;

			if (pkt->buf[0] == 0 && pkt->buf[1] == 0 &&
					pkt->buf[2] == 0 && pkt->buf[3] == 1) {
				int nt = pkt->buf[4] & 0x1f;
				if (nt == 7) {
					if (ctx.sps)
						free(ctx.sps);
					ctx.sps = malloc(pkt->len);
					memcpy(ctx.sps, pkt->buf, pkt->len);
					ctx.spslen = pkt->len;
//					printf("New SPS, length %d\n",
//							ctx.spslen);
				} else if (nt == 8) {
					if (ctx.pps)
						free(ctx.pps);
					ctx.pps = malloc(pkt->len);
					memcpy(ctx.pps, pkt->buf, pkt->len);
					ctx.ppslen = pkt->len;
//					printf("New PPS, length %d\n",
//							ctx.ppslen);
				}
			}

			if (ctx.coc)
				writeframe(ctx.coc, pkt, ctx.cocvidindex);

			checkstate(pkt);

			spare->nFilledLen = 0;
			spare->nOffset = 0;
			OERRq(OMX_FillThisBuffer(enc, spare));
			spare = spare->pAppPrivate;

			ctx.framenum++;
		}
	} while (1);

	if (oc) {
		av_write_trailer(oc);

		for (i = 0; i < oc->nb_streams; i++)
			avcodec_close(oc->streams[i]->codec);

		avio_close(oc->pb);
	} else {
		close(fd);
	}

	return 0;
}
