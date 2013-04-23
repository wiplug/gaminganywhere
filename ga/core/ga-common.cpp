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
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#ifndef WIN32
#ifndef ANDROID
#include <execinfo.h>
#endif
#include <unistd.h>
#include <sys/time.h>
#include <sys/syscall.h>
#endif
#ifdef ANDROID
#include <android/log.h>
#endif

#include "ga-common.h"
#include "ga-conf.h"
#include "ga-avcodec.h"
#include "rtspconf.h"

#ifndef NIPQUAD
#define NIPQUAD(x)	((unsigned char*)&(x))[0],	\
			((unsigned char*)&(x))[1],	\
			((unsigned char*)&(x))[2],	\
			((unsigned char*)&(x))[3]
#endif

static char *ga_logfile = NULL;

long long
tvdiff_us(struct timeval *tv1, struct timeval *tv2) {
	struct timeval delta;
	delta.tv_sec = tv1->tv_sec - tv2->tv_sec;
	delta.tv_usec = tv1->tv_usec - tv2->tv_usec;
	if(delta.tv_usec < 0) {
		delta.tv_sec--;
		delta.tv_usec += 1000000;
	}
	return 1000000LL*delta.tv_sec + delta.tv_usec;
}

long long
ga_usleep(long long interval, struct timeval *ptv) {
	long long delta;
	struct timeval tv;
	if(ptv != NULL) {
		gettimeofday(&tv, NULL);
		delta = tvdiff_us(&tv, ptv);
		if(delta >= interval) {
			usleep(1);
			return -1;
		}
		interval -= delta;
	}
	usleep(interval);
	return 0LL;
}

int
ga_error(const char *fmt, ...) {
	char msg[4096];
	struct timeval tv;
	va_list ap;
	gettimeofday(&tv, NULL);
	va_start(ap, fmt);
#ifdef ANDROID
	__android_log_vprint(ANDROID_LOG_INFO, "ga_log", fmt, ap);
#else
	vsnprintf(msg, sizeof(msg), fmt, ap);
#endif
	va_end(ap);
	fprintf(stderr, "# [%d] %ld.%06ld %s", getpid(), tv.tv_sec, tv.tv_usec, msg);
#if 1	/* log to file? */
	if(ga_logfile != NULL) {
		FILE *fp;
		char buf[4096];
		snprintf(buf, sizeof(buf),
			"[%d] %ld.%06ld %s", getpid(), tv.tv_sec, tv.tv_usec, msg);
		if((fp = fopen(ga_logfile, "at")) != NULL) {
			fprintf(fp, "%s", buf);
			fclose(fp);
		}
	}
#endif
	return -1;
}

int
ga_malloc(int size, void **ptr, int *alignment) {
	if((*ptr = malloc(size+16)) == NULL)
		return -1;
#ifdef __x86_64__
	*alignment = 16 - (((long long) *ptr)&0x0f);
#else
	*alignment = 16 - (((unsigned) *ptr)&0x0f);
#endif
	return 0;
}

long
ga_gettid() {
#ifdef WIN32
	return GetCurrentThreadId();
#elif defined __APPLE__
	return pthread_mach_thread_np(pthread_self());
#elif defined ANDROID
	return gettid();
#else
	return (pid_t) syscall(SYS_gettid);
#endif
}

static int
winsock_init() {
#ifdef WIN32
	WSADATA wd;
	if(WSAStartup(MAKEWORD(2,2), &wd) != 0)
		return -1;
#endif
	return 0;
}

int
ga_init(const char *config, const char *url) {
	srand(time(0));
	winsock_init();
	av_register_all();
	avcodec_register_all();
	avformat_network_init();
	if(config != NULL) {
		if(ga_conf_load(config) < 0) {
			ga_error("GA: cannot load configuration file '%s'\n", config);
			return -1;
		}
	}
	if(url != NULL) {
		if(ga_url_parse(url) < 0) {
			ga_error("GA: invalid URL '%s'\n", url);
			return -1;
		}
	}
	return 0;
}

void
ga_deinit() {
	return;
}

void
ga_openlog() {
	char fn[1024];
	FILE *fp;
	//
	if(ga_conf_readv("logfile", fn, sizeof(fn)) == NULL)
		return;
	if((fp = fopen(fn, "at")) != NULL) {
		fclose(fp);
		ga_logfile = strdup(fn);
	}
	//
	return;
}

void
ga_closelog() {
	if(ga_logfile != NULL) {
		free(ga_logfile);
		ga_logfile = NULL;
	}
	return;
}

long
ga_atoi(const char *str) {
	// XXX: not sure why sometimes windows strtol failed on
	// handling read-only constant strings ...
	char buf[64];
	long val;
	strncpy(buf, str, sizeof(buf));
	val = strtol(buf, NULL, 0);
	return val;
}

struct gaRect *
ga_fillrect(struct gaRect *rect, int left, int top, int right, int bottom) {
	if(rect == NULL)
		return NULL;
#define SWAP(a,b)	do { int tmp = a; a = b; b = tmp; } while(0);
	if(left > right)
		SWAP(left, right);
	if(top > bottom)
		SWAP(top, bottom);
#undef	SWAP
	rect->left = left;
	rect->top = top;
	rect->right = right;
	rect->bottom = bottom;
	//
	rect->width = rect->right - rect->left + 1;
	rect->height = rect->bottom - rect->top + 1;
	rect->linesize = rect->width * RGBA_SIZE;
	rect->size = rect->width * rect->height * RGBA_SIZE;
	//
	if(rect->width <= 0 || rect->height <= 0) {
		ga_error("# invalid rect size (%dx%d)\n", rect->width, rect->height);
		return NULL;
	}
	//
	return rect;
}

#ifdef WIN32
int
ga_crop_window(struct gaRect *rect, struct gaRect **prect) {
	char wndname[1024], wndclass[1024];
	char *pname;
	char *pclass;
	int find_wnd_arg = 0;
	HWND hWnd;
	RECT client;
	POINT lt, rb;
	//
	if(rect == NULL || prect == NULL)
		return -1;
	//
	pname = ga_conf_readv("find-window-name", wndname, sizeof(wndname));
	pclass = ga_conf_readv("find-window-class", wndclass, sizeof(wndclass));
	//
	if(pname != NULL && *pname != '\0')
		find_wnd_arg++;
	if(pclass != NULL && *pclass != '\0')
		find_wnd_arg++;
	if(find_wnd_arg <= 0) {
		*prect = NULL;
		return 0;
	}
	//
	if((hWnd = FindWindow(pclass, pname)) == NULL) {
		ga_error("FindWindow failed for '%s/%s'\n",
			pclass ? pclass : "",
			pname ? pname : "");
		return -1;
	}
	//
	GetWindowText(hWnd, wndname, sizeof(wndname));
	//
	ga_error("Found window (0x%08x) :%s%s%s%s\n", hWnd,
		pclass ? " class=" : "",
		pclass ? pclass : "",
		pname ? " name=" : "",
		pname ? pname : "");
	//
	if(SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE|SWP_SHOWWINDOW) == 0) {
		ga_error("SetWindowPos failed.\n");
		return -1;
	}
	if(GetClientRect(hWnd, &client) == 0) {
		ga_error("GetClientRect failed.\n");
		return -1;
	}
	if(SetForegroundWindow(hWnd) == 0) {
		ga_error("SetForegroundWindow failed.\n");
	}
	//
	lt.x = client.left;
	lt.y = client.top;
	rb.x = client.right-1;
	rb.y = client.bottom-1;
	//
	if(ClientToScreen(hWnd, &lt) == 0
	|| ClientToScreen(hWnd, &rb) == 0) {
		ga_error("Map from client coordinate to screen coordinate failed.\n");
		return -1;
	}
	//
	rect->left = lt.x;
	rect->top = lt.y;
	rect->right = rb.x;
	rect->bottom = rb.y;
	// size check: multiples of 2?
	if((rect->right - rect->left + 1) % 2 != 0)
		rect->left--;
	if((rect->bottom - rect->top + 1) % 2 != 0)
		rect->top--;
	//
	if(rect->left < 0 || rect->top < 0) {
		ga_error("Invalid window: (%d,%d)-(%d,%d) w=%d h=%d.\n",
			rect->left, rect->top, rect->right, rect->bottom,
			rect->right - rect->left + 1,
			rect->bottom - rect->top + 1);
		return -1;
	}
	//
	*prect = rect;
	return 1;
}
#else
int
ga_crop_window(struct gaRect *rect, struct gaRect **prect) {
	// XXX: implement find window for other platforms
	*prect = NULL;
#if 0
	if(rect == NULL || prect == NULL)
		return -1;
	rect->left = 100;
	rect->top = 100;
	rect->right = 739;
	rect->bottom = 579;
	*prect = rect;
#endif
	return 0;
}
#endif

void
ga_backtrace() {
#if defined WIN32 || defined ANDROID
	return;
#else
	int j, nptrs;
#define SIZE 100
	void *buffer[SIZE];
	char **strings;

	nptrs = backtrace(buffer, SIZE);
	printf("-- backtrace() returned %d addresses -----------\n", nptrs);

	strings = backtrace_symbols(buffer, nptrs);
	if (strings == NULL) {
		perror("backtrace_symbols");
		exit(-1);
	}

	for (j = 0; j < nptrs; j++)
		printf("%s\n", strings[j]);

	free(strings);
	printf("------------------------------------------------\n");
#endif	/* WIN32 */
}

void
ga_dummyfunc() {
	// place some required functions - for link purpose
	swr_alloc_set_opts(NULL, 0, (AVSampleFormat) 0, 0, 0, (AVSampleFormat) 0, 0, 0, NULL);
	return;
}

