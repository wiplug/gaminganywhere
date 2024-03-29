/*
 * Copyright (c) 2013 Chun-Ying Huang
 *
 * This file is part of Gaming Anywere (GA).
 *
 * GA is free software; you can redistribute it and/or modify it
 * under the terms of the 3-clause BSD License as published by the
 * Free Software Foundation: http://directory.fsf.org/wiki/License:BSD_3Clause
 *
 * GA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the 3-clause BSD License along with GA;
 * if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#ifndef WIN32
#include <unistd.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <arpa/inet.h>
#endif	/* ifndef WIN32 */

#include "vsource.h"
#include "asource.h"
#include "encoder-common.h"
//#include "encoder-video.h"
//#include "encoder-audio.h"
#include "server.h"
#include "rtspserver.h"

#include "ga-common.h"
#include "ga-avcodec.h"
#if 0
#ifndef WIN32
#include "ga-xwin.h"
#include "ga-alsa.h"
#endif
#endif

#define	RTSP_STREAM_FORMAT	"streamid=%d"
#define	RTSP_STREAM_FORMAT_MAXLEN	64

static struct RTSPConf *rtspconf = NULL;

void
rtsp_cleanup(RTSPContext *rtsp, int retcode) {
	rtsp->state = SERVER_STATE_TEARDOWN;
#ifdef WIN32
	Sleep(1000);
#else
	sleep(1);
#endif
	return;
}

static int
rtsp_write(RTSPContext *ctx, const void *buf, size_t count) {
	return write(ctx->fd, buf, count);
}

static int
rtsp_printf(RTSPContext *ctx, const char *fmt, ...) {
	va_list ap;
	char buf[8192];
	int buflen;
	//
	va_start(ap, fmt);
	buflen = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return rtsp_write(ctx, buf, buflen);
}

int
rtsp_write_bindata(RTSPContext *ctx, int streamid, uint8_t *buf, int buflen) {
	int i, pktlen;
	char header[4];
	//
	if(buflen < 4) {
		return buflen;
	}
	// XXX: buffer is the reuslt from avio_open_dyn_buf.
	// Multiple RTP packets can be placed in a single buffer.
	// Format == 4-bytes (big-endian) packet size + packet-data
	i = 0;
	while(i < buflen) {
		pktlen  = (buf[i+0] << 24);
		pktlen += (buf[i+1] << 16);
		pktlen += (buf[i+2] << 8);
		pktlen += (buf[i+3]);
		if(pktlen == 0) {
			i += 4;
			continue;
		}
		//
		header[0] = '$';
		header[1] = (streamid<<1) & 0x0ff;
		header[2] = pktlen>>8;
		header[3] = pktlen & 0x0ff;
		pthread_mutex_lock(&ctx->rtsp_writer_mutex);
		if(rtsp_write(ctx, header, 4) != 4) {
			pthread_mutex_unlock(&ctx->rtsp_writer_mutex);
			return i;
		}
		if(rtsp_write(ctx, &buf[i+4], pktlen) != pktlen) {
			return i;
		}
		pthread_mutex_unlock(&ctx->rtsp_writer_mutex);
		//
		i += (4+pktlen);
	}
	return i;
}

static int
rtsp_read_internal(RTSPContext *ctx) {
	int rlen;
	if((rlen = read(ctx->fd, 
		ctx->rbuffer + ctx->rbuftail,
		ctx->rbufsize - ctx->rbuftail)) <= 0) {
		return -1;
	}
	ctx->rbuftail += rlen;
	return ctx->rbuftail - ctx->rbufhead;
}

static int
rtsp_read_text(RTSPContext *ctx, char *buf, size_t count) {
	int i;
	size_t textlen;
again:
	for(i = ctx->rbufhead; i < ctx->rbuftail; i++) {
		if(ctx->rbuffer[i] == '\n') {
			textlen = i - ctx->rbufhead + 1;
			if(textlen > count-1) {
				ga_error("Insufficient string buffer length.\n");
				return -1;
			}
			bcopy(ctx->rbuffer + ctx->rbufhead, buf, textlen);
			buf[textlen] = '\0';
			ctx->rbufhead += textlen;
			if(ctx->rbufhead == ctx->rbuftail)
				ctx->rbufhead = ctx->rbuftail = 0;
			return textlen;
		}
	}
	// buffer full?
	if(ctx->rbuftail - ctx->rbufhead == ctx->rbufsize) {
		ga_error("Buffer full: Extremely long text data encountered?\n");
		return -1;
	}
	// did not found '\n', read more
	bcopy(ctx->rbuffer + ctx->rbufhead, ctx->rbuffer, ctx->rbuftail - ctx->rbufhead);
	ctx->rbuftail = ctx->rbuftail - ctx->rbufhead;
	ctx->rbufhead = 0;
	//
	if(rtsp_read_internal(ctx) < 0)
		return -1;
	goto again;
	// unreachable, but to meet compiler's requirement
	return -1;
}

static int
rtsp_read_binary(RTSPContext *ctx, char *buf, size_t count) {
	int reqlength;
	if(ctx->rbuftail - ctx->rbufhead < 4)
		goto readmore;
again:
	reqlength = (unsigned char) ctx->rbuffer[ctx->rbufhead+2];
	reqlength <<= 8;
	reqlength += (unsigned char) ctx->rbuffer[ctx->rbufhead+3];
	// data is ready
	if(4+reqlength <= ctx->rbuftail - ctx->rbufhead) {
		bcopy(ctx->rbuffer + ctx->rbufhead, buf, 4+reqlength);
		ctx->rbufhead += (4+reqlength);
		if(ctx->rbufhead == ctx->rbuftail)
			ctx->rbufhead = ctx->rbuftail = 0;
		return 4+reqlength;
	}
	// second trail?
	if(ctx->rbuftail - ctx->rbufhead == ctx->rbufsize) {
		ga_error("Buffer full: Extremely long binary data encountered?\n");
		return -1;
	}
readmore:
	bcopy(ctx->rbuffer + ctx->rbufhead, ctx->rbuffer, ctx->rbuftail - ctx->rbufhead);
	ctx->rbuftail = ctx->rbuftail - ctx->rbufhead;
	ctx->rbufhead = 0;
	//
	if(rtsp_read_internal(ctx) < 0)
		return -1;
	goto again;
	// unreachable, but to meet compiler's requirement
	return -1;
}

static int
rtsp_getnext(RTSPContext *ctx, char *buf, size_t count) {
	// initialize if necessary
	if(ctx->rbuffer == NULL) {
		ctx->rbufsize = 65536;
		if((ctx->rbuffer = (char*) malloc(ctx->rbufsize)) == NULL) {
			ctx->rbufsize = 0;
			return -1;
		}
		ctx->rbufhead = 0;
		ctx->rbuftail = 0;
	}
	// buffer is empty, force read
	if(ctx->rbuftail == ctx->rbufhead) {
		if(rtsp_read_internal(ctx) < 0)
			return -1;
	}
	// buffer is not empty
	if(ctx->rbuffer[ctx->rbufhead] != '$') {
		// text data
		return rtsp_read_text(ctx, buf, count);
	}
	// binary data
	return rtsp_read_binary(ctx, buf, count);
}

static int
per_client_init(RTSPContext *ctx) {
	int i;
	AVOutputFormat *fmt;
	//
	if((fmt = av_guess_format("rtp", NULL, NULL)) == NULL) {
		ga_error("RTP not supported.\n");
		return -1;
	}
	if((ctx->sdp_fmtctx = avformat_alloc_context()) == NULL) {
		ga_error("create avformat context failed.\n");
		return -1;
	}
	ctx->sdp_fmtctx->oformat = fmt;
	// video stream
	for(i = 0; i < video_source_channels(); i++) {
		if((ctx->sdp_vstream[i] = ga_avformat_new_stream(
			ctx->sdp_fmtctx,
			i, rtspconf->video_encoder_codec)) == NULL) {
			//
			ga_error("cannot create new video stream (%d:%d)\n",
				i, rtspconf->video_encoder_codec->id);
			return -1;
		}
		if((ctx->sdp_vencoder[i] = ga_avcodec_vencoder_init(
			ctx->sdp_vstream[i]->codec,
			rtspconf->video_encoder_codec,
			video_source_width(i), video_source_height(i),
			rtspconf->video_fps,
			rtspconf->vso)) == NULL) {
			//
			ga_error("cannot init video encoder\n");
			return -1;
		}
	}
	// audio stream
#ifdef ENABLE_AUDIO
	if((ctx->sdp_astream = ga_avformat_new_stream(
			ctx->sdp_fmtctx,
			video_source_channels(),
			rtspconf->audio_encoder_codec)) == NULL) {
		ga_error("cannot create new audio stream (%d)\n",
			rtspconf->audio_encoder_codec->id);
		return -1;
	}
	if((ctx->sdp_aencoder = ga_avcodec_aencoder_init(
			ctx->sdp_astream->codec,
			rtspconf->audio_encoder_codec,
			rtspconf->audio_bitrate,
			rtspconf->audio_samplerate,
			rtspconf->audio_channels,
			rtspconf->audio_codec_format,
			rtspconf->audio_codec_channel_layout)) == NULL) {
		ga_error("cannot init audio encoder\n");
		return -1;
	}
#endif
	return 0;
}

static void
close_av(AVFormatContext *fctx, AVStream *st, AVCodecContext *cctx, enum RTSPLowerTransport transport) {
	unsigned i;
	//
	if(cctx) {
		ga_avcodec_close(cctx);
	}
	if(st && st->codec != NULL) {
		if(st->codec != cctx) {
			ga_avcodec_close(st->codec);
		}
		st->codec = NULL;
	}
	if(fctx) {
		for(i = 0; i < fctx->nb_streams; i++) {
			if(cctx != fctx->streams[i]->codec) {
				if(fctx->streams[i]->codec)
					ga_avcodec_close(fctx->streams[i]->codec);
			} else {
				cctx = NULL;
			}
			av_freep(&fctx->streams[i]->codec);
			if(st == fctx->streams[i])
				st = NULL;
			av_freep(&fctx->streams[i]);
		}
		if(transport==RTSP_LOWER_TRANSPORT_UDP && fctx->pb)
			avio_close(fctx->pb);
		av_free(fctx);
	}
	if(cctx != NULL)
		av_free(cctx);
	if(st != NULL)
		av_free(st);
	return;
}

static void
per_client_deinit(RTSPContext *ctx) {
	int i;
	for(i = 0; i < video_source_channels()+1; i++) {
		close_av(ctx->fmtctx[i], ctx->stream[i], ctx->encoder[i], ctx->lower_transport[i]);
	}
	//close_av(ctx->fmtctx[0], ctx->stream[0], ctx->encoder[0], ctx->lower_transport[0]);
	//close_av(ctx->fmtctx[1], ctx->stream[1], ctx->encoder[1], ctx->lower_transport[1]);
	//
	close_av(ctx->sdp_fmtctx, NULL, NULL, RTSP_LOWER_TRANSPORT_UDP);
	//
	if(ctx->rbuffer) {
		free(ctx->rbuffer);
	}
	ctx->rbufsize = 0;
	ctx->rbufhead = ctx->rbuftail = 0;
	//
	return;
}

static void
rtsp_reply_header(RTSPContext *c, enum RTSPStatusCode error_number) {
	const char *str;
	time_t ti;
	struct tm rtm;
	char buf2[32];

	switch(error_number) {
	case RTSP_STATUS_OK:
		str = "OK";
		break;
	case RTSP_STATUS_METHOD:
		str = "Method Not Allowed";
		break;
	case RTSP_STATUS_BANDWIDTH:
		str = "Not Enough Bandwidth";
		break;
	case RTSP_STATUS_SESSION:
		str = "Session Not Found";
		break;
	case RTSP_STATUS_STATE:
		str = "Method Not Valid in This State";
		break;
	case RTSP_STATUS_AGGREGATE:
		str = "Aggregate operation not allowed";
		break;
	case RTSP_STATUS_ONLY_AGGREGATE:
		str = "Only aggregate operation allowed";
		break;
	case RTSP_STATUS_TRANSPORT:
		str = "Unsupported transport";
		break;
	case RTSP_STATUS_INTERNAL:
		str = "Internal Server Error";
		break;
	case RTSP_STATUS_SERVICE:
		str = "Service Unavailable";
		break;
	case RTSP_STATUS_VERSION:
		str = "RTSP Version not supported";
		break;
	default:
		str = "Unknown Error";
		break;
	}

	rtsp_printf(c, "RTSP/1.0 %d %s\r\n", error_number, str);
	rtsp_printf(c, "CSeq: %d\r\n", c->seq);
	/* output GMT time */
	ti = time(NULL);
	gmtime_r(&ti, &rtm);
	strftime(buf2, sizeof(buf2), "%a, %d %b %Y %H:%M:%S", &rtm);
	rtsp_printf(c, "Date: %s GMT\r\n", buf2);
	//
	return;
}

static void
rtsp_reply_error(RTSPContext *c, enum RTSPStatusCode error_number) {
	rtsp_reply_header(c, error_number);
	rtsp_printf(c, "\r\n");
}

static int
prepare_sdp_description(RTSPContext *ctx, char *buf, int bufsize) {
	buf[0] = '\0';
	av_dict_set(&ctx->sdp_fmtctx->metadata, "title", rtspconf->title, 0);
	snprintf(ctx->sdp_fmtctx->filename, sizeof(ctx->sdp_fmtctx->filename), "rtp://0.0.0.0");
	av_sdp_create(&ctx->sdp_fmtctx, 1, buf, bufsize);
	return strlen(buf);
}

static void
rtsp_cmd_describe(RTSPContext *ctx, const char *url) {
	struct sockaddr_in myaddr;
#ifdef WIN32
	int addrlen;
#else
	socklen_t addrlen;
#endif
	char path[4096];
	char content[4096];
	int content_length;
	//
	av_url_split(NULL, 0, NULL, 0, NULL, 0, NULL, path, sizeof(path), url);
	if(strcmp(path, rtspconf->object) != 0) {
		rtsp_reply_error(ctx, RTSP_STATUS_SERVICE);
		return;
	}
	//
	addrlen = sizeof(myaddr);
	getsockname(ctx->fd, (struct sockaddr*) &myaddr, &addrlen);
	content_length = prepare_sdp_description(ctx, content, sizeof(content));
	if(content_length < 0) {
		rtsp_reply_error(ctx, RTSP_STATUS_INTERNAL);
		return;
	}
	// state does not change
	rtsp_reply_header(ctx, RTSP_STATUS_OK);
	rtsp_printf(ctx, "Content-Base: %s/\r\n", url);
	rtsp_printf(ctx, "Content-Type: application/sdp\r\n");
	rtsp_printf(ctx, "Content-Length: %d\r\n", content_length);
	rtsp_printf(ctx, "\r\n");
	rtsp_write(ctx, content, content_length);
	return;
}

static void
rtsp_cmd_options(RTSPContext *c, const char *url) {
	// state does not change
	rtsp_printf(c, "RTSP/1.0 %d %s\r\n", RTSP_STATUS_OK, "OK");
	rtsp_printf(c, "CSeq: %d\r\n", c->seq);
	//rtsp_printf(c, "Public: %s\r\n", "OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE");
	rtsp_printf(c, "Public: %s\r\n", "OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY");
	rtsp_printf(c, "\r\n");
	return;
}

static RTSPTransportField *
find_transport(RTSPMessageHeader *h, enum RTSPLowerTransport lower_transport) {
	RTSPTransportField *th;
	int i;
	for(i = 0; i < h->nb_transports; i++) {
		th = &h->transports[i];
		if (th->lower_transport == lower_transport)
			return th;
	}
	return NULL;
}

static int
rtp_new_av_stream(RTSPContext *ctx, struct sockaddr_in *sin, int streamid, enum CodecID codecid) {
	AVOutputFormat *fmt = NULL;
	AVFormatContext *fmtctx = NULL;
	AVStream *stream = NULL;
	AVCodecContext *encoder = NULL;
	uint8_t *dummybuf = NULL;
	//
	if(streamid > IMAGE_SOURCE_CHANNEL_MAX) {
		ga_error("invalid stream index (%d > %d)\n",
			streamid, IMAGE_SOURCE_CHANNEL_MAX);
		return -1;
	}
	if(codecid != rtspconf->video_encoder_codec->id
	&& codecid != rtspconf->audio_encoder_codec->id) {
		ga_error("invalid codec (%d)\n", codecid);
		return -1;
	}
	if(ctx->fmtctx[streamid] != NULL) {
		ga_error("duplicated setup to an existing stream (%d)\n",
			streamid);
		return -1;
	}
	if((fmt = av_guess_format("rtp", NULL, NULL)) == NULL) {
		ga_error("RTP not supported.\n");
		return -1;
	}
	if((fmtctx = avformat_alloc_context()) == NULL) {
		ga_error("create avformat context failed.\n");
		return -1;
	}
	fmtctx->oformat = fmt;
	if(ctx->lower_transport[streamid] == RTSP_LOWER_TRANSPORT_UDP) {
		snprintf(fmtctx->filename, sizeof(fmtctx->filename),
			"rtp://%s:%d", inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));
		if(avio_open(&fmtctx->pb, fmtctx->filename, AVIO_FLAG_WRITE) < 0) {
			ga_error("cannot open URL: %s\n", fmtctx->filename);
			return -1;
		}
		ga_error("RTP/UDP: URL opened [%d]: %s, max_packet_size=%d\n",
			streamid, fmtctx->filename, fmtctx->pb->max_packet_size);
	} else if(ctx->lower_transport[streamid] == RTSP_LOWER_TRANSPORT_TCP) {
		// XXX: should we use avio_open_dyn_buf(&fmtctx->pb)?
		if(ffio_open_dyn_packet_buf(&fmtctx->pb, RTSP_TCP_MAX_PACKET_SIZE) < 0) {
			ga_error("cannot open dynamic packet buffer\n");
			return -1;
		}
		ga_error("RTP/TCP: Dynamic buffer opened, max_packet_size=%d.\n",
			fmtctx->pb->max_packet_size);
	}
	fmtctx->pb->seekable = 0;
	//
	if((stream = ga_avformat_new_stream(fmtctx, 0,
			codecid == rtspconf->video_encoder_codec->id ?
				rtspconf->video_encoder_codec : rtspconf->audio_encoder_codec)) == NULL) {
		ga_error("Cannot create new stream (%d)\n", codecid);
		return -1;
	}
//#ifndef SHARE_ENCODER
	if(codecid == rtspconf->video_encoder_codec->id) {
		encoder = ga_avcodec_vencoder_init(
				stream->codec,
				rtspconf->video_encoder_codec,
				video_source_width(streamid),
				video_source_height(streamid),
				rtspconf->video_fps,
				rtspconf->vso);
	} else if(codecid == rtspconf->audio_encoder_codec->id) {
		encoder = ga_avcodec_aencoder_init(
				stream->codec,
				rtspconf->audio_encoder_codec,
				rtspconf->audio_bitrate,
				rtspconf->audio_samplerate,
				rtspconf->audio_channels,
				rtspconf->audio_codec_format,
				rtspconf->audio_codec_channel_layout);
	}
	if(encoder == NULL) {
		ga_error("Cannot init encoder\n");
		return -1;
	}
//#endif	/* SHARE_ENCODER */
	//
	ctx->encoder[streamid] = encoder;
	ctx->stream[streamid] = stream;
	ctx->fmtctx[streamid] = fmtctx;
	// write header
	if(avformat_write_header(ctx->fmtctx[streamid], NULL) < 0) {
		ga_error("Cannot write stream id %d.\n", streamid);
		return -1;
	}
	if(ctx->lower_transport[streamid] == RTSP_LOWER_TRANSPORT_TCP) {
		int rlen;
		rlen = avio_close_dyn_buf(ctx->fmtctx[streamid]->pb, &dummybuf);
		av_free(dummybuf);
	}
	//
	return 0;
}

static void
rtsp_cmd_setup(RTSPContext *ctx, const char *url, RTSPMessageHeader *h) {
	int i;
	RTSPTransportField *th;
	struct sockaddr_in destaddr, myaddr;
#ifdef WIN32
	int destaddrlen, myaddrlen;
#else
	socklen_t destaddrlen, myaddrlen;
#endif
	char path[4096];
	char channelname[IMAGE_SOURCE_CHANNEL_MAX+1][RTSP_STREAM_FORMAT_MAXLEN];
	int baselen = strlen(rtspconf->object);
	int streamid;
	int rtp_port, rtcp_port;
	enum RTSPStatusCode errcode;
	//
	av_url_split(NULL, 0, NULL, 0, NULL, 0, NULL, path, sizeof(path), url);
	for(i = 0; i < IMAGE_SOURCE_CHANNEL_MAX+1; i++) {
		snprintf(channelname[i], RTSP_STREAM_FORMAT_MAXLEN, RTSP_STREAM_FORMAT, i);
	}
	//
	if(strncmp(path, rtspconf->object, baselen) != 0) {
		ga_error("invalid object (path=%s)\n", path);
		rtsp_reply_error(ctx, RTSP_STATUS_AGGREGATE);
		return;
	}
	for(i = 0; i < IMAGE_SOURCE_CHANNEL_MAX+1; i++) {
		if(strcmp(path+baselen+1, channelname[i]) == 0) {
			streamid = i;
			break;
		}
	}
	if(i == IMAGE_SOURCE_CHANNEL_MAX+1) {
		// not found
		ga_error("invalid service (path=%s)\n", path);
		rtsp_reply_error(ctx, RTSP_STATUS_SERVICE);
		return;
	}
	//
	if(ctx->state != SERVER_STATE_IDLE
	&& ctx->state != SERVER_STATE_READY) {
		rtsp_reply_error(ctx, RTSP_STATUS_STATE);
		return;
	}
	// create session id?
	if(ctx->session_id == NULL) {
		if(h->session_id[0] == '\0') {
			snprintf(h->session_id, sizeof(h->session_id), "%04x%04x",
				rand()%0x0ffff, rand()%0x0ffff);
			ctx->session_id = strdup(h->session_id);
			ga_error("New session created (id = %s)\n", ctx->session_id);
		}
	}
	// session id must match -- we have only one session
	if(ctx->session_id == NULL
	|| strcmp(ctx->session_id, h->session_id) != 0) {
		ga_error("Bad session id %s != %s\n", h->session_id, ctx->session_id);
		errcode = RTSP_STATUS_SESSION;
		goto error_setup;
	}
	// find supported transport
	if((th = find_transport(h, RTSP_LOWER_TRANSPORT_UDP)) == NULL) {
		th = find_transport(h, RTSP_LOWER_TRANSPORT_TCP);
	}
	if(th == NULL) {
		ga_error("Cannot find transport\n");
		errcode = RTSP_STATUS_TRANSPORT;
		goto error_setup;
	}
	//
	destaddrlen = sizeof(destaddr);
	bzero(&destaddr, destaddrlen);
	if(getpeername(ctx->fd, (struct sockaddr*) &destaddr, &destaddrlen) < 0) {
		ga_error("Cannot get peername\n");
		errcode = RTSP_STATUS_INTERNAL;
		goto error_setup;
	}
	destaddr.sin_port = htons(th->client_port_min);
	//
	myaddrlen = sizeof(myaddr);
	bzero(&myaddr, myaddrlen);
	if(getsockname(ctx->fd, (struct sockaddr*) &myaddr, &myaddrlen) < 0) {
		ga_error("Cannot get sockname\n");
		errcode = RTSP_STATUS_INTERNAL;
		goto error_setup;
	}
	//
	ctx->lower_transport[streamid] = th->lower_transport;
	if(rtp_new_av_stream(ctx, &destaddr, streamid,
			streamid == video_source_channels()/*rtspconf->audio_id*/ ?
				rtspconf->audio_encoder_codec->id : rtspconf->video_encoder_codec->id) < 0) {
		ga_error("Create AV stream %d failed.\n", streamid);
		errcode = RTSP_STATUS_TRANSPORT;
		goto error_setup;
	}
	//
	ctx->state = SERVER_STATE_READY;
	rtsp_reply_header(ctx, RTSP_STATUS_OK);
	rtsp_printf(ctx, "Session: %s\r\n", ctx->session_id);
	switch(th->lower_transport) {
	case RTSP_LOWER_TRANSPORT_UDP:
		rtp_port = ff_rtp_get_local_rtp_port((URLContext*) ctx->fmtctx[streamid]->pb->opaque);
		rtcp_port = ff_rtp_get_local_rtcp_port((URLContext*) ctx->fmtctx[streamid]->pb->opaque);
		ga_error("RTP/UDP: client=%d-%d; server=%d-%d\n",
		       th->client_port_min, th->client_port_max,
		       rtp_port, rtcp_port);
		rtsp_printf(ctx, "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d;server_port=%d-%d\r\n",
		       th->client_port_min, th->client_port_max,
		       rtp_port, rtcp_port);
		break;
	case RTSP_LOWER_TRANSPORT_TCP:
		ga_error("RTP/TCP: interleaved=%d-%d\n",
			streamid*2, streamid*2+1);
		rtsp_printf(ctx, "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n",
			streamid*2, streamid*2+1, streamid*2);
		break;
	default:
		// should not happen
		break;
	}
	rtsp_printf(ctx, "\r\n");
	return;
error_setup:
	if(ctx->session_id != NULL) {
		free(ctx->session_id);
		ctx->session_id = NULL;
	}
	if(ctx->encoder[streamid] != NULL) {
		ctx->encoder[streamid] = NULL;
	}
	if(ctx->stream[streamid] != NULL) {
		ctx->stream[streamid] = NULL;
	}
	if(ctx->fmtctx[streamid] != NULL) {
		avformat_free_context(ctx->fmtctx[streamid]);
		ctx->fmtctx[streamid] = NULL;
	}
	rtsp_reply_error(ctx, errcode);
	return;
}
static void
rtsp_cmd_play(RTSPContext *ctx, const char *url, RTSPMessageHeader *h) {
	char path[4096];
	//
	av_url_split(NULL, 0, NULL, 0, NULL, 0, NULL, path, sizeof(path), url);
	if(strncmp(path, rtspconf->object, strlen(rtspconf->object)) != 0) {
		rtsp_reply_error(ctx, RTSP_STATUS_SESSION);
		return;
	}
	if(strcmp(ctx->session_id, h->session_id) != 0) {
		rtsp_reply_error(ctx, RTSP_STATUS_SESSION);
		return;
	}
	//
	if(ctx->state != SERVER_STATE_READY
	&& ctx->state != SERVER_STATE_PAUSE) {
		rtsp_reply_error(ctx, RTSP_STATUS_STATE);
		return;
	}
	// create threads
#ifndef SHARE_ENCODER
	if(pthread_create(&ctx->vthread, NULL, vencoder_thread, ctx) != 0) {
		ga_error("cannot create video thread\n");
		rtsp_reply_error(ctx, RTSP_STATUS_INTERNAL);
		return;
	}
#ifdef ENABLE_AUDIO
	if(pthread_create(&ctx->athread, NULL, aencoder_thread, ctx) != 0) {
		ga_error("cannot create audio thread\n");
		rtsp_reply_error(ctx, RTSP_STATUS_INTERNAL);
		return;
	}
#endif	/* ENABLE_AUDIO */
#else
	if(encoder_register_client(ctx) < 0) {
		ga_error("cannot register encoder client.\n");
		rtsp_reply_error(ctx, RTSP_STATUS_INTERNAL);
		return;
	}
#endif	/* SHARE_ENCODER */
	//
	ctx->state = SERVER_STATE_PLAYING;
	rtsp_reply_header(ctx, RTSP_STATUS_OK);
	rtsp_printf(ctx, "Session: %s\r\n", ctx->session_id);
	rtsp_printf(ctx, "\r\n");
	return;
}

static void
rtsp_cmd_pause(RTSPContext *ctx, const char *url, RTSPMessageHeader *h) {
	char path[4096];
	//
	av_url_split(NULL, 0, NULL, 0, NULL, 0, NULL, path, sizeof(path), url);
	if(strncmp(path, rtspconf->object, strlen(rtspconf->object)) != 0) {
		rtsp_reply_error(ctx, RTSP_STATUS_SESSION);
		return;
	}
	if(strcmp(ctx->session_id, h->session_id) != 0) {
		rtsp_reply_error(ctx, RTSP_STATUS_SESSION);
		return;
	}
	//
	if(ctx->state != SERVER_STATE_PLAYING) {
		rtsp_reply_error(ctx, RTSP_STATUS_STATE);
		return;
	}
	//
	ctx->state = SERVER_STATE_PAUSE;
	rtsp_reply_header(ctx, RTSP_STATUS_OK);
	rtsp_printf(ctx, "Session: %s\r\n", ctx->session_id);
	rtsp_printf(ctx, "\r\n");
	return;
}

static void
rtsp_cmd_teardown(RTSPContext *ctx, const char *url, RTSPMessageHeader *h) {
	char path[4096];
	//
	av_url_split(NULL, 0, NULL, 0, NULL, 0, NULL, path, sizeof(path), url);
	if(strncmp(path, rtspconf->object, strlen(rtspconf->object)) != 0) {
		rtsp_reply_error(ctx, RTSP_STATUS_SESSION);
		return;
	}
	if(strcmp(ctx->session_id, h->session_id) != 0) {
		rtsp_reply_error(ctx, RTSP_STATUS_SESSION);
		return;
	}
	//
	ctx->state = SERVER_STATE_TEARDOWN;
	rtsp_reply_header(ctx, RTSP_STATUS_OK);
	rtsp_printf(ctx, "Session: %s\r\n", ctx->session_id);
	rtsp_printf(ctx, "\r\n");
	return;
}

struct RTCPHeader {
	unsigned char vps;	// version, padding, RC/SC
#define	RTCP_Version(hdr)	(((hdr)->vps) >> 6)
#define	RTCP_Padding(hdr)	((((hdr)->vps) >> 5) & 0x01)
#define	RTCP_RC(hdr)		(((hdr)->vps) & 0x1f)
#define	RTCP_SC(hdr)		RTCP_RC(hdr)
	unsigned char pt;
	unsigned short length;
}
#ifdef WIN32
;
#else
__attribute__ ((__packed__));
#endif

static int
handle_rtcp(RTSPContext *ctx, const char *buf, size_t buflen) {
#if 0
	int reqlength;
	struct RTCPHeader *rtcp;
	char msg[64] = "", *ptr = msg;
	//
	reqlength = (unsigned char) buf[2];
	reqlength <<= 8;
	reqlength += (unsigned char) buf[3];
	rtcp = (struct RTCPHeader*) (buf+4);
	//
	ga_error("TCP feedback for stream %d received (%d bytes): ver=%d; sc=%d; pt=%d; length=%d\n",
		buf[1], reqlength,
		RTCP_Version(rtcp), RTCP_SC(rtcp), rtcp->pt, ntohs(rtcp->length));
	for(int i = 0; i < 16; i++, ptr += 3) {
		snprintf(ptr, sizeof(msg)-(ptr-msg),
			"%2.2x ", (unsigned char) buf[i]);
	}
	ga_error("HEX: %s\n", msg);
#endif
	return 0;
}

static void
skip_spaces(const char **pp) {
	const char *p;
	p = *pp;
	while (*p == ' ' || *p == '\t')
		p++;
	*pp = p;
}

static void
get_word(char *buf, int buf_size, const char **pp) {
	const char *p;
	char *q;

	p = *pp;
	skip_spaces(&p);
	q = buf;
	while (!isspace(*p) && *p != '\0') {
		if ((q - buf) < buf_size - 1)
			*q++ = *p;
		p++;
	}
	if (buf_size > 0)
		*q = '\0';
	*pp = p;
}

void*
rtspserver(void *arg) {
#ifdef WIN32
	SOCKET s = *((SOCKET*) arg);
	int sinlen = sizeof(struct sockaddr_in);
#else
	int s = *((int*) arg);
	socklen_t sinlen = sizeof(struct sockaddr_in);
#endif
	const char *p;
	char buf[8192];
	char cmd[32], url[1024], protocol[32];
	int rlen;
	struct sockaddr_in sin;
	RTSPContext ctx;
	RTSPMessageHeader header1, *header = &header1;
	int thread_ret;
	// image info
	int iwidth = video_source_width(0);
	int iheight = video_source_height(0);
	//
	rtspconf = rtspconf_global();
	sinlen = sizeof(sin);
	getpeername(s, (struct sockaddr*) &sin, &sinlen);
	//
	bzero(&ctx, sizeof(ctx));
	if(per_client_init(&ctx) < 0) {
		ga_error("server initialization failed.\n");
		return NULL;
	}
	ctx.state = SERVER_STATE_IDLE;
	// XXX: hasVideo is used to sync audio/video
	// This value is increased by 1 for each captured frame until it is gerater than zero
	// when this value is greater than zero, audio encoding then starts ...
	//ctx.hasVideo = -(rtspconf->video_fps>>1);	// for slow encoders?
	ctx.hasVideo = 0;	// with 'zerolatency'
	pthread_mutex_init(&ctx.rtsp_writer_mutex, NULL);
#if 0
	ctx.audioparam.channels = rtspconf->audio_channels;
	ctx.audioparam.samplerate = rtspconf->audio_samplerate;
	if(rtspconf->audio_device_format == AV_SAMPLE_FMT_S16) {
#ifdef WIN32
#else
		ctx.audioparam.format = SND_PCM_FORMAT_S16_LE;
#endif
		ctx.audioparam.bits_per_sample = 16;
	}
	//
	ga_error("INFO: image: %dx%d; audio: %d ch 16-bit pcm @ %dHz\n",
			iwidth, iheight,
			ctx.audioparam.channels,
			ctx.audioparam.samplerate);
#endif
	//
#if 0
#ifdef WIN32
	if(ga_wasapi_init(&ctx.audioparam) < 0) {
		ga_error("cannot init wasapi.\n");
		return NULL;
	}
#else
	if((ctx.audioparam.handle = ga_alsa_init(&ctx.audioparam.sndlog)) == NULL) {
		ga_error("cannot init alsa.\n");
		return NULL;
	}
	if(ga_alsa_set_param(&ctx.audioparam) < 0) {
		ga_error("cannot set alsa parameter\n");
		return NULL;
	}
#endif
#endif
	//
	ga_error("[tid %ld] client connected from %s:%d\n",
		ga_gettid(),
		inet_ntoa(sin.sin_addr), htons(sin.sin_port));
	//
	ctx.fd = s;
	//
	do {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(ctx.fd, &rfds);
		if(select(ctx.fd+1, &rfds, NULL, NULL, NULL) <=0) {
			ga_error("select() failed: %s\n", strerror(errno));
			goto quit;
		}
		// read commands
		if((rlen = rtsp_getnext(&ctx, buf, sizeof(buf))) < 0) {
			goto quit;
		}
		// Interleaved binary data?
		if(buf[0] == '$') {
			handle_rtcp(&ctx, buf, rlen);
			continue;
		}
		// REQUEST line
		ga_error("%s", buf);
		p = buf;
		get_word(cmd, sizeof(cmd), &p);
		get_word(url, sizeof(url), &p);
		get_word(protocol, sizeof(protocol), &p);
		// check protocol
		if(strcmp(protocol, "RTSP/1.0") != 0) {
			rtsp_reply_error(&ctx, RTSP_STATUS_VERSION);
			goto quit;
		}
		// read headers
		bzero(header, sizeof(*header));
		do {
			int myseq = -1;
			char mysession[sizeof(header->session_id)] = "";
			if((rlen = rtsp_getnext(&ctx, buf, sizeof(buf))) < 0)
				goto quit;
			if(buf[0]=='\n' || (buf[0]=='\r' && buf[1]=='\n'))
				break;
#if 0
			ga_error("HEADER: %s", buf);
#endif
			// Special handling to CSeq & Session header
			// ff_rtsp_parse_line cannot handle CSeq & Session properly on Windows
			// any more?
			if(strncasecmp("CSeq: ", buf, 6) == 0) {
				myseq = strtol(buf+6, NULL, 10);
			}
			if(strncasecmp("Session: ", buf, 9) == 0) {
				strcpy(mysession, buf+9);
			}
			//
			ff_rtsp_parse_line(header, buf, NULL, NULL);
			//
			if(myseq > 0 && header->seq <= 0) {
				ga_error("WARNING: CSeq fixes applied (%d->%d).\n",
					header->seq, myseq);
				header->seq = myseq;
			}
			if(mysession[0] != '\0' && header->session_id[0]=='\0') {
				unsigned i;
				for(i = 0; i < sizeof(header->session_id)-1; i++) {
					if(mysession[i] == '\0'
					|| isspace(mysession[i])
					|| mysession[i] == ';')
						break;
					header->session_id[i] = mysession[i];
				}
				header->session_id[i+1] = '\0';
				ga_error("WARNING: Session fixes applied (%s)\n",
					header->session_id);
			}
		} while(1);
		// special handle to session_id
		if(header->session_id != NULL) {
			char *p = header->session_id;
			while(*p != '\0') {
				if(*p == '\r' || *p == '\n') {
					*p = '\0';
					break;
				}
				p++;
			}
		}
		// handle commands
		ctx.seq = header->seq;
		if (!strcmp(cmd, "DESCRIBE"))
			rtsp_cmd_describe(&ctx, url);
		else if (!strcmp(cmd, "OPTIONS"))
			rtsp_cmd_options(&ctx, url);
		else if (!strcmp(cmd, "SETUP"))
			rtsp_cmd_setup(&ctx, url, header);
		else if (!strcmp(cmd, "PLAY"))
			rtsp_cmd_play(&ctx, url, header);
		else if (!strcmp(cmd, "PAUSE"))
			rtsp_cmd_pause(&ctx, url, header);
		else if (!strcmp(cmd, "TEARDOWN"))
			rtsp_cmd_teardown(&ctx, url, header);
		else
			rtsp_reply_error(&ctx, RTSP_STATUS_METHOD);
		if(ctx.state == SERVER_STATE_TEARDOWN) {
			break;
		}
	} while(1);
quit:
	ctx.state = SERVER_STATE_TEARDOWN;
	//
	close(ctx.fd);
#ifdef	SHARE_ENCODER
	encoder_unregister_client(&ctx);
#else
	ga_error("connection closed, checking for worker threads...\n");
#if 0
	//
	if(ctx.vthreadId != 0) {
		video_source_notify_one(ctx.vthreadId);
	}
#endif
	pthread_join(ctx.vthread, (void**) &thread_ret);
#ifdef	ENABLE_AUDIO
	pthread_join(ctx.athread, (void**) &thread_ret);
#endif	/* ENABLE_AUDIO */
#endif	/* SHARE_ENCODER */
	//
	per_client_deinit(&ctx);
	//ga_error("RTSP client thread terminated (%d/%d clients left).\n",
	//	video_source_client_count(), audio_source_client_count());
	ga_error("RTSP client thread terminated.\n");
	//
	return NULL;
}

