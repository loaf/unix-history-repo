/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Hugh Smith at The University of Guelph.
 *
 * %sccs.include.redist.c%
 */

#ifndef lint
static char sccsid[] = "@(#)archive.c	5.2 (Berkeley) %G%";
#endif /* not lint */

#include <sys/param.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <ar.h>
#include <stdio.h>
#include "archive.h"

extern CHDR chdr;			/* converted header */
extern char *archive;			/* archive name */

typedef struct ar_hdr HDR;
static char hb[sizeof(HDR) + 1];	/* real header */

open_archive(mode)
	int mode;
{
	int created, fd, nr;
	char buf[SARMAG];
	
	created = 0;
	if (mode & O_CREAT) {
		mode |= O_EXCL;
		if ((fd = open(archive, mode, DEFFILEMODE)) >= 0) {
			/* POSIX.2 puts create message on stderr. */
			if (!(options & AR_C))
				(void)fprintf(stderr,
				    "ar: creating archive %s.\n", archive);
			created = 1;
			goto opened;
		}
		if (errno != EEXIST)
			error(archive);
		mode &= ~O_EXCL;
	}
	if ((fd = open(archive, mode, DEFFILEMODE)) < 0)
		error(archive);

	/* 
	 * Attempt to place a lock on the opened file - if we get an 
	 * error then someone is already working on this library (or
	 * it's going across NFS).
	 */
opened:	if (flock(fd, LOCK_EX|LOCK_NB) && errno != EOPNOTSUPP)
		error(archive);
	
	/*
	 * If not created, O_RDONLY|O_RDWR indicates that it has to be
	 * in archive format.
	 */
	if (!created &&
	    ((mode & O_ACCMODE) == O_RDONLY || (mode & O_ACCMODE) == O_RDWR)) {
		if ((nr = read(fd, buf, SARMAG) != SARMAG)) {
			if (nr >= 0)
				badfmt();
			error(archive);
		} else if (bcmp(buf, ARMAG, SARMAG))
			badfmt();
	} else if (write(fd, ARMAG, SARMAG) != SARMAG)
		error(archive);
	return(fd);
}

close_archive(fd)
	int fd;
{
	(void)flock(fd, LOCK_UN);
	(void)close(fd);
}

/* Convert ar header field to an integer. */
#define	AR_ATOI(from, to, len, base) { \
	bcopy(from, buf, len); \
	buf[len] = '\0'; \
	to = strtol(buf, (char **)NULL, base); \
}

/*
 * get_header --
 *	read the archive header for this member
 */
get_header(fd)
	int fd;
{
	struct ar_hdr *hdr;
	register int len, nr;
	register char *p, buf[20];

	nr = read(fd, hb, sizeof(HDR));
	if (nr != sizeof(HDR)) {
		if (!nr)
			return(0);
		if (nr < 0)
			error(archive);
		badfmt();
	}

	hdr = (struct ar_hdr *)hb;
	if (strncmp(hdr->ar_fmag, ARFMAG, sizeof(ARFMAG) - 1))
		badfmt();

	/* Convert the header into the internal format. */
#define	DECIMAL	10
#define	OCTAL	 8

	AR_ATOI(hdr->ar_date, chdr.date, sizeof(hdr->ar_date), DECIMAL);
	AR_ATOI(hdr->ar_uid, chdr.uid, sizeof(hdr->ar_uid), DECIMAL);
	AR_ATOI(hdr->ar_gid, chdr.gid, sizeof(hdr->ar_gid), DECIMAL);
	AR_ATOI(hdr->ar_mode, chdr.mode, sizeof(hdr->ar_mode), OCTAL);
	AR_ATOI(hdr->ar_size, chdr.size, sizeof(hdr->ar_size), DECIMAL);

	/* Leading spaces should never happen. */
	if (hdr->ar_name[0] == ' ')
		badfmt();

	/*
	 * Long name support.  Set the "real" size of the file, and the
	 * long name flag/size.
	 */
	if (!bcmp(hdr->ar_name, AR_EFMT1, sizeof(AR_EFMT1) - 1)) {
		chdr.lname = len = atoi(hdr->ar_name + sizeof(AR_EFMT1) - 1);
		if (len <= 0 || len > MAXNAMLEN)
			badfmt();
		nr = read(fd, chdr.name, len);
		if (nr != len) {
			if (nr < 0)
				error(archive);
			badfmt();
		}
		chdr.name[len] = 0;
		chdr.size -= len;
	} else {
		chdr.lname = 0;
		bcopy(hdr->ar_name, chdr.name, sizeof(hdr->ar_name));

		/* Strip trailing spaces, null terminate. */
		for (p = chdr.name + sizeof(hdr->ar_name) - 1; *p == ' '; --p);
		*++p = '\0';
	}
	return(1);
}

static int already_written;

/*
 * put_object --
 *	Write an archive member to a file.
 */
put_object(cfp, sb)
	CF *cfp;
	struct stat *sb;
{
	register int lname;
	register char *name;
	struct ar_hdr *hdr;
	off_t size;
	char *rname();

	/*
	 * If passed an sb structure, reading a file from disk.  Get stat(2)
	 * information, build a name and construct a header.  (Files are named
	 * by their last component in the archive.)  If not, then just write
	 * the last header read.
	 */
	if (sb) {
		name = rname(cfp->rname);
		(void)fstat(cfp->rfd, sb);

		if ((lname = strlen(name)) > sizeof(hdr->ar_name) ||
		    index(name, ' ')) {
			(void)sprintf(hb, HDR1, AR_EFMT1, lname, sb->st_mtime,
			    sb->st_uid, sb->st_gid, sb->st_mode,
			    sb->st_size + lname, ARFMAG);
		} else {
			lname = 0;
			(void)sprintf(hb, HDR2, name, sb->st_mtime, sb->st_uid,
			    sb->st_gid, sb->st_mode, sb->st_size, ARFMAG);
		}
		size = sb->st_size;
	} else {
		lname = chdr.lname;
		name = chdr.name;
		size = chdr.size;
	}

	if (write(cfp->wfd, hb, sizeof(HDR)) != sizeof(HDR))
		error(cfp->wname);
	if (lname) {
		if (write(cfp->wfd, name, lname) != lname)
			error(cfp->wname);
		already_written = lname;
	}
	copyfile(cfp, size);
	already_written = 0;
}

/*
 * copyfile --
 *	Copy size bytes from one file to another - taking care to handle the
 *	extra byte (for odd size files) when reading archives and writing an
 *	extra byte if necessary when adding files to archive.  The length of
 *	the object is the long name plus the object itself; the variable
 *	already_written gets set if a long name was written.
 *
 *	The padding is really unnecessary, and is almost certainly a remnant
 *	of early archive formats where the header included binary data which
 *	a PDP-11 required to start on an even byte boundary.  (Or, perhaps,
 *	because 16-bit word addressed copies were faster?)  Anyhow, it should
 *	have been ripped out long ago.
 */
copyfile(cfp, size)
	CF *cfp;
	off_t size;
{
	static char pad = '\n';
	register off_t sz;
	register int from, nr, nw, off, to;
	char buf[8*1024];
	
	if (!(sz = size))
		return;

	from = cfp->rfd;
	to = cfp->wfd;
	sz = size;
	while (sz && (nr = read(from, buf, MIN(sz, sizeof(buf)))) > 0) {
		sz -= nr;
		for (off = 0; off < nr; nr -= off, off += nw)
			if ((nw = write(to, buf + off, nr)) < 0)
				error(cfp->wname);
	}
	if (sz) {
		if (nr == 0)
			badfmt();
		error(cfp->rname);
	}

	if (cfp->flags & RPAD && size & 1 && (nr = read(from, buf, 1)) != 1) {
		if (nr == 0)
			badfmt();
		error(cfp->rname);
	}
	if (cfp->flags & WPAD && (size + already_written) & 1 &&
	    write(to, &pad, 1) != 1)
		error(cfp->wname);
}

/*
 * skipobj -
 *	Skip over an object -- taking care to skip the pad bytes.
 */
skipobj(fd)
	int fd;
{
	off_t len;

	len = chdr.size + (chdr.size + chdr.lname & 1);
	if (lseek(fd, len, SEEK_CUR) == (off_t)-1)
		error(archive);
}
