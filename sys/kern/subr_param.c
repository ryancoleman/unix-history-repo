/*
 * Copyright (c) 1980, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)param.c	8.3 (Berkeley) 8/20/94
 * $FreeBSD$
 */

#include "opt_param.h"
#include "opt_maxusers.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>

/*
 * System parameter formulae.
 */

#ifndef HZ
#define	HZ 100
#endif
#define	NPROC (20 + 16 * maxusers)
#ifndef NBUF
#define NBUF 0
#endif
#ifndef MAXFILES
#define	MAXFILES (maxproc * 2)
#endif

int	hz;
int	tick;
int	tickadj;			 /* can adjust 30ms in 60s */
int	maxusers;			/* base tunable */
int	maxproc;			/* maximum # of processes */
int	maxprocperuid;			/* max # of procs per user */
int	maxfiles;			/* sys. wide open files limit */
int	maxfilesperproc;		/* per-proc open files limit */
int	ncallout;			/* maximum # of timer events */
int	nbuf;
int	nswbuf;
int	maxswzone;			/* max swmeta KVA storage */
int	maxbcache;			/* max buffer cache KVA storage */

/*
 * These have to be allocated somewhere; allocating
 * them here forces loader errors if this file is omitted
 * (if they've been externed everywhere else; hah!).
 */
struct	buf *swbuf;

/*
 * Total number of shared mutexes to protect all lockmgr locks.
 */
#ifndef	LOCKMUTEX
#define LOCKMUTEX	10
#endif
int	lock_nmtx = LOCKMUTEX;

/*
 * Boot time overrides
 */
void
init_param(void)
{

	/* Base parameters */
	maxusers = MAXUSERS;
	TUNABLE_INT_FETCH("kern.maxusers", &maxusers);
	hz = HZ;
	TUNABLE_INT_FETCH("kern.hz", &hz);
	tick = 1000000 / hz;
	tickadj = howmany(30000, 60 * hz);	/* can adjust 30ms in 60s */

	/* The following can be overridden after boot via sysctl */
	maxproc = NPROC;
	TUNABLE_INT_FETCH("kern.maxproc", &maxproc);
	maxfiles = MAXFILES;
	TUNABLE_INT_FETCH("kern.maxfiles", &maxfiles);
	maxprocperuid = maxproc - 1;
	maxfilesperproc = maxfiles;

	/* Cannot be changed after boot */
	nbuf = NBUF;
	TUNABLE_INT_FETCH("kern.nbuf", &nbuf);
#ifdef VM_SWZONE_SIZE_MAX
	maxswzone = VM_SWZONE_SIZE_MAX;
#endif
	TUNABLE_INT_FETCH("kern.maxswzone", &maxswzone);
#ifdef VM_BCACHE_SIZE_MAX
	maxbcache = VM_BCACHE_SIZE_MAX;
#endif
	TUNABLE_INT_FETCH("kern.maxbcache", &maxbcache);
	ncallout = 16 + maxproc + maxfiles;
	TUNABLE_INT_FETCH("kern.ncallout", &ncallout);
}
