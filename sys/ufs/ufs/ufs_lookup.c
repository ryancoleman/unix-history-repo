/*
 * Copyright (c) 1989, 1993
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
 *	@(#)ufs_lookup.c	8.15 (Berkeley) 6/16/95
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/namei.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/dir.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>

#ifdef DIAGNOSTIC
int	dirchk = 1;
#else
int	dirchk = 0;
#endif

SYSCTL_INT(_debug, OID_AUTO, dircheck, CTLFLAG_RW, &dirchk, 0, "");

/* true if old FS format...*/
#define OFSFMT(vp)	((vp)->v_mount->mnt_maxsymlinklen <= 0)

/*
 * Convert a component of a pathname into a pointer to a locked inode.
 * This is a very central and rather complicated routine.
 * If the file system is not maintained in a strict tree hierarchy,
 * this can result in a deadlock situation (see comments in code below).
 *
 * The cnp->cn_nameiop argument is LOOKUP, CREATE, RENAME, or DELETE depending
 * on whether the name is to be looked up, created, renamed, or deleted.
 * When CREATE, RENAME, or DELETE is specified, information usable in
 * creating, renaming, or deleting a directory entry may be calculated.
 * If flag has LOCKPARENT or'ed into it and the target of the pathname
 * exists, lookup returns both the target and its parent directory locked.
 * When creating or renaming and LOCKPARENT is specified, the target may
 * not be ".".  When deleting and LOCKPARENT is specified, the target may
 * be "."., but the caller must check to ensure it does an vrele and vput
 * instead of two vputs.
 *
 * This routine is actually used as VOP_CACHEDLOOKUP method, and the
 * filesystem employs the generic vfs_cache_lookup() as VOP_LOOKUP
 * method.
 *
 * vfs_cache_lookup() performs the following for us:
 *	check that it is a directory
 *	check accessibility of directory
 *	check for modification attempts on read-only mounts
 *	if name found in cache
 *	    if at end of path and deleting or creating
 *		drop it
 *	     else
 *		return name.
 *	return VOP_CACHEDLOOKUP()
 *
 * Overall outline of ufs_lookup:
 *
 *	search for name in directory, to found or notfound
 * notfound:
 *	if creating, return locked directory, leaving info on available slots
 *	else return error
 * found:
 *	if at end of path and deleting, return information to allow delete
 *	if at end of path and rewriting (RENAME and LOCKPARENT), lock target
 *	  inode and return info to allow rewrite
 *	if not at end, add name to cache; if at end and neither creating
 *	  nor deleting, add name to cache
 */
int
ufs_lookup(ap)
	struct vop_cachedlookup_args /* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
	} */ *ap;
{
	register struct vnode *vdp;	/* vnode for directory being searched */
	register struct inode *dp;	/* inode for directory being searched */
	struct buf *bp;			/* a buffer of directory entries */
	register struct direct *ep;	/* the current directory entry */
	int entryoffsetinblock;		/* offset of ep in bp's buffer */
	enum {NONE, COMPACT, FOUND} slotstatus;
	doff_t slotoffset;		/* offset of area with free space */
	int slotsize;			/* size of area at slotoffset */
	int slotfreespace;		/* amount of space free in slot */
	int slotneeded;			/* size of the entry we're seeking */
	int numdirpasses;		/* strategy for directory search */
	doff_t endsearch;		/* offset to end directory search */
	doff_t prevoff;			/* prev entry dp->i_offset */
	struct vnode *pdp;		/* saved dp during symlink work */
	struct vnode *tdp;		/* returned by VFS_VGET */
	doff_t enduseful;		/* pointer past last used dir slot */
	u_long bmask;			/* block offset mask */
	int lockparent;			/* 1 => lockparent flag is set */
	int wantparent;			/* 1 => wantparent or lockparent flag */
	int namlen, error;
	struct vnode **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	struct ucred *cred = cnp->cn_cred;
	int flags = cnp->cn_flags;
	int nameiop = cnp->cn_nameiop;
	struct proc *p = cnp->cn_proc;

	bp = NULL;
	slotoffset = -1;
/*
 *  XXX there was a soft-update diff about this I couldn't merge.
 * I think this was the equiv.
 */
	*vpp = NULL;

	vdp = ap->a_dvp;
	dp = VTOI(vdp);
	lockparent = flags & LOCKPARENT;
	wantparent = flags & (LOCKPARENT|WANTPARENT);

	/*
	 * We now have a segment name to search for, and a directory to search.
	 *
	 * Suppress search for slots unless creating
	 * file and at end of pathname, in which case
	 * we watch for a place to put the new file in
	 * case it doesn't already exist.
	 */
	slotstatus = FOUND;
	slotfreespace = slotsize = slotneeded = 0;
	if ((nameiop == CREATE || nameiop == RENAME) &&
	    (flags & ISLASTCN)) {
		slotstatus = NONE;
		slotneeded = DIRECTSIZ(cnp->cn_namelen);
	}

	/*
	 * If there is cached information on a previous search of
	 * this directory, pick up where we last left off.
	 * We cache only lookups as these are the most common
	 * and have the greatest payoff. Caching CREATE has little
	 * benefit as it usually must search the entire directory
	 * to determine that the entry does not exist. Caching the
	 * location of the last DELETE or RENAME has not reduced
	 * profiling time and hence has been removed in the interest
	 * of simplicity.
	 */
	bmask = VFSTOUFS(vdp->v_mount)->um_mountp->mnt_stat.f_iosize - 1;
	if (nameiop != LOOKUP || dp->i_diroff == 0 ||
	    dp->i_diroff >= dp->i_size) {
		entryoffsetinblock = 0;
		dp->i_offset = 0;
		numdirpasses = 1;
	} else {
		dp->i_offset = dp->i_diroff;
		if ((entryoffsetinblock = dp->i_offset & bmask) &&
		    (error = UFS_BLKATOFF(vdp, (off_t)dp->i_offset, NULL, &bp)))
			return (error);
		numdirpasses = 2;
		nchstats.ncs_2passes++;
	}
	prevoff = dp->i_offset;
	endsearch = roundup2(dp->i_size, DIRBLKSIZ);
	enduseful = 0;

searchloop:
	while (dp->i_offset < endsearch) {
		/*
		 * If necessary, get the next directory block.
		 */
		if ((dp->i_offset & bmask) == 0) {
			if (bp != NULL)
				brelse(bp);
			error =
			    UFS_BLKATOFF(vdp, (off_t)dp->i_offset, NULL, &bp);
			if (error)
				return (error);
			entryoffsetinblock = 0;
		}
		/*
		 * If still looking for a slot, and at a DIRBLKSIZE
		 * boundary, have to start looking for free space again.
		 */
		if (slotstatus == NONE &&
		    (entryoffsetinblock & (DIRBLKSIZ - 1)) == 0) {
			slotoffset = -1;
			slotfreespace = 0;
		}
		/*
		 * Get pointer to next entry.
		 * Full validation checks are slow, so we only check
		 * enough to insure forward progress through the
		 * directory. Complete checks can be run by patching
		 * "dirchk" to be true.
		 */
		ep = (struct direct *)((char *)bp->b_data + entryoffsetinblock);
		if (ep->d_reclen == 0 ||
		    (dirchk && ufs_dirbadentry(vdp, ep, entryoffsetinblock))) {
			int i;

			ufs_dirbad(dp, dp->i_offset, "mangled entry");
			i = DIRBLKSIZ - (entryoffsetinblock & (DIRBLKSIZ - 1));
			dp->i_offset += i;
			entryoffsetinblock += i;
			continue;
		}

		/*
		 * If an appropriate sized slot has not yet been found,
		 * check to see if one is available. Also accumulate space
		 * in the current block so that we can determine if
		 * compaction is viable.
		 */
		if (slotstatus != FOUND) {
			int size = ep->d_reclen;

			if (ep->d_ino != 0)
				size -= DIRSIZ(OFSFMT(vdp), ep);
			if (size > 0) {
				if (size >= slotneeded) {
					slotstatus = FOUND;
					slotoffset = dp->i_offset;
					slotsize = ep->d_reclen;
				} else if (slotstatus == NONE) {
					slotfreespace += size;
					if (slotoffset == -1)
						slotoffset = dp->i_offset;
					if (slotfreespace >= slotneeded) {
						slotstatus = COMPACT;
						slotsize = dp->i_offset +
						      ep->d_reclen - slotoffset;
					}
				}
			}
		}

		/*
		 * Check for a name match.
		 */
		if (ep->d_ino) {
#			if (BYTE_ORDER == LITTLE_ENDIAN)
				if (OFSFMT(vdp))
					namlen = ep->d_type;
				else
					namlen = ep->d_namlen;
#			else
				namlen = ep->d_namlen;
#			endif
			if (namlen == cnp->cn_namelen &&
				(cnp->cn_nameptr[0] == ep->d_name[0]) &&
			    !bcmp(cnp->cn_nameptr, ep->d_name,
				(unsigned)namlen)) {
				/*
				 * Save directory entry's inode number and
				 * reclen in ndp->ni_ufs area, and release
				 * directory buffer.
				 */
				if (vdp->v_mount->mnt_maxsymlinklen > 0 &&
				    ep->d_type == DT_WHT) {
					slotstatus = FOUND;
					slotoffset = dp->i_offset;
					slotsize = ep->d_reclen;
					dp->i_reclen = slotsize;
					enduseful = dp->i_size;
					ap->a_cnp->cn_flags |= ISWHITEOUT;
					numdirpasses--;
					goto notfound;
				}
				dp->i_ino = ep->d_ino;
				dp->i_reclen = ep->d_reclen;
				goto found;
			}
		}
		prevoff = dp->i_offset;
		dp->i_offset += ep->d_reclen;
		entryoffsetinblock += ep->d_reclen;
		if (ep->d_ino)
			enduseful = dp->i_offset;
	}
notfound:
	/*
	 * If we started in the middle of the directory and failed
	 * to find our target, we must check the beginning as well.
	 */
	if (numdirpasses == 2) {
		numdirpasses--;
		dp->i_offset = 0;
		endsearch = dp->i_diroff;
		goto searchloop;
	}
	if (bp != NULL)
		brelse(bp);
	/*
	 * If creating, and at end of pathname and current
	 * directory has not been removed, then can consider
	 * allowing file to be created.
	 */
	if ((nameiop == CREATE || nameiop == RENAME ||
	     (nameiop == DELETE &&
	      (ap->a_cnp->cn_flags & DOWHITEOUT) &&
	      (ap->a_cnp->cn_flags & ISWHITEOUT))) &&
	    (flags & ISLASTCN) && dp->i_effnlink != 0) {
		/*
		 * Access for write is interpreted as allowing
		 * creation of files in the directory.
		 */
		error = VOP_ACCESS(vdp, VWRITE, cred, cnp->cn_proc);
		if (error)
			return (error);
		/*
		 * Return an indication of where the new directory
		 * entry should be put.  If we didn't find a slot,
		 * then set dp->i_count to 0 indicating
		 * that the new slot belongs at the end of the
		 * directory. If we found a slot, then the new entry
		 * can be put in the range from dp->i_offset to
		 * dp->i_offset + dp->i_count.
		 */
		if (slotstatus == NONE) {
			dp->i_offset = roundup2(dp->i_size, DIRBLKSIZ);
			dp->i_count = 0;
			enduseful = dp->i_offset;
		} else if (nameiop == DELETE) {
			dp->i_offset = slotoffset;
			if ((dp->i_offset & (DIRBLKSIZ - 1)) == 0)
				dp->i_count = 0;
			else
				dp->i_count = dp->i_offset - prevoff;
		} else {
			dp->i_offset = slotoffset;
			dp->i_count = slotsize;
			if (enduseful < slotoffset + slotsize)
				enduseful = slotoffset + slotsize;
		}
		dp->i_endoff = roundup2(enduseful, DIRBLKSIZ);
		dp->i_flag |= IN_CHANGE | IN_UPDATE;
		/*
		 * We return with the directory locked, so that
		 * the parameters we set up above will still be
		 * valid if we actually decide to do a direnter().
		 * We return ni_vp == NULL to indicate that the entry
		 * does not currently exist; we leave a pointer to
		 * the (locked) directory inode in ndp->ni_dvp.
		 * The pathname buffer is saved so that the name
		 * can be obtained later.
		 *
		 * NB - if the directory is unlocked, then this
		 * information cannot be used.
		 */
		cnp->cn_flags |= SAVENAME;
		if (!lockparent)
			VOP_UNLOCK(vdp, 0, p);
		return (EJUSTRETURN);
	}
	/*
	 * Insert name into cache (as non-existent) if appropriate.
	 */
	if ((cnp->cn_flags & MAKEENTRY) && nameiop != CREATE)
		cache_enter(vdp, *vpp, cnp);
	return (ENOENT);

found:
	if (numdirpasses == 2)
		nchstats.ncs_pass2++;
	/*
	 * Check that directory length properly reflects presence
	 * of this entry.
	 */
	if (dp->i_offset + DIRSIZ(OFSFMT(vdp), ep) > dp->i_size) {
		ufs_dirbad(dp, dp->i_offset, "i_size too small");
		dp->i_size = dp->i_offset + DIRSIZ(OFSFMT(vdp), ep);
		dp->i_flag |= IN_CHANGE | IN_UPDATE;
	}
	brelse(bp);

	/*
	 * Found component in pathname.
	 * If the final component of path name, save information
	 * in the cache as to where the entry was found.
	 */
	if ((flags & ISLASTCN) && nameiop == LOOKUP)
		dp->i_diroff = dp->i_offset &~ (DIRBLKSIZ - 1);

	/*
	 * If deleting, and at end of pathname, return
	 * parameters which can be used to remove file.
	 * If the wantparent flag isn't set, we return only
	 * the directory (in ndp->ni_dvp), otherwise we go
	 * on and lock the inode, being careful with ".".
	 */
	if (nameiop == DELETE && (flags & ISLASTCN)) {
		/*
		 * Write access to directory required to delete files.
		 */
		error = VOP_ACCESS(vdp, VWRITE, cred, cnp->cn_proc);
		if (error)
			return (error);
		/*
		 * Return pointer to current entry in dp->i_offset,
		 * and distance past previous entry (if there
		 * is a previous entry in this block) in dp->i_count.
		 * Save directory inode pointer in ndp->ni_dvp for dirremove().
		 */
		if ((dp->i_offset & (DIRBLKSIZ - 1)) == 0)
			dp->i_count = 0;
		else
			dp->i_count = dp->i_offset - prevoff;
		if (dp->i_number == dp->i_ino) {
			VREF(vdp);
			*vpp = vdp;
			return (0);
		}
		if (flags & ISDOTDOT)
			VOP_UNLOCK(vdp, 0, p);	/* race to get the inode */
		error = VFS_VGET(vdp->v_mount, dp->i_ino, &tdp);
		if (flags & ISDOTDOT)
			vn_lock(vdp, LK_EXCLUSIVE | LK_RETRY, p);
		if (error)
			return (error);
		/*
		 * If directory is "sticky", then user must own
		 * the directory, or the file in it, else she
		 * may not delete it (unless she's root). This
		 * implements append-only directories.
		 */
		if ((dp->i_mode & ISVTX) &&
		    cred->cr_uid != 0 &&
		    cred->cr_uid != dp->i_uid &&
		    VTOI(tdp)->i_uid != cred->cr_uid) {
			vput(tdp);
			return (EPERM);
		}
		*vpp = tdp;
		if (!lockparent)
			VOP_UNLOCK(vdp, 0, p);
		return (0);
	}

	/*
	 * If rewriting (RENAME), return the inode and the
	 * information required to rewrite the present directory
	 * Must get inode of directory entry to verify it's a
	 * regular file, or empty directory.
	 */
	if (nameiop == RENAME && wantparent && (flags & ISLASTCN)) {
		if ((error = VOP_ACCESS(vdp, VWRITE, cred, cnp->cn_proc)) != 0)
			return (error);
		/*
		 * Careful about locking second inode.
		 * This can only occur if the target is ".".
		 */
		if (dp->i_number == dp->i_ino)
			return (EISDIR);
		if (flags & ISDOTDOT)
			VOP_UNLOCK(vdp, 0, p);	/* race to get the inode */
		error = VFS_VGET(vdp->v_mount, dp->i_ino, &tdp);
		if (flags & ISDOTDOT)
			vn_lock(vdp, LK_EXCLUSIVE | LK_RETRY, p);
		if (error)
			return (error);
		*vpp = tdp;
		cnp->cn_flags |= SAVENAME;
		if (!lockparent)
			VOP_UNLOCK(vdp, 0, p);
		return (0);
	}

	/*
	 * Step through the translation in the name.  We do not `vput' the
	 * directory because we may need it again if a symbolic link
	 * is relative to the current directory.  Instead we save it
	 * unlocked as "pdp".  We must get the target inode before unlocking
	 * the directory to insure that the inode will not be removed
	 * before we get it.  We prevent deadlock by always fetching
	 * inodes from the root, moving down the directory tree. Thus
	 * when following backward pointers ".." we must unlock the
	 * parent directory before getting the requested directory.
	 * There is a potential race condition here if both the current
	 * and parent directories are removed before the VFS_VGET for the
	 * inode associated with ".." returns.  We hope that this occurs
	 * infrequently since we cannot avoid this race condition without
	 * implementing a sophisticated deadlock detection algorithm.
	 * Note also that this simple deadlock detection scheme will not
	 * work if the file system has any hard links other than ".."
	 * that point backwards in the directory structure.
	 */
	pdp = vdp;
	if (flags & ISDOTDOT) {
		VOP_UNLOCK(pdp, 0, p);	/* race to get the inode */
		if ((error = VFS_VGET(vdp->v_mount, dp->i_ino, &tdp)) != 0) {
			vn_lock(pdp, LK_EXCLUSIVE | LK_RETRY, p);
			return (error);
		}
		if (lockparent && (flags & ISLASTCN) &&
		    (error = vn_lock(pdp, LK_EXCLUSIVE, p))) {
			vput(tdp);
			return (error);
		}
		*vpp = tdp;
	} else if (dp->i_number == dp->i_ino) {
		VREF(vdp);	/* we want ourself, ie "." */
		*vpp = vdp;
	} else {
		error = VFS_VGET(vdp->v_mount, dp->i_ino, &tdp);
		if (error)
			return (error);
		if (!lockparent || !(flags & ISLASTCN))
			VOP_UNLOCK(pdp, 0, p);
		*vpp = tdp;
	}

	/*
	 * Insert name into cache if appropriate.
	 */
	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(vdp, *vpp, cnp);
	return (0);
}

void
ufs_dirbad(ip, offset, how)
	struct inode *ip;
	doff_t offset;
	char *how;
{
	struct mount *mp;

	mp = ITOV(ip)->v_mount;
	(void)printf("%s: bad dir ino %lu at offset %ld: %s\n",
	    mp->mnt_stat.f_mntonname, (u_long)ip->i_number, (long)offset, how);
	if ((mp->mnt_stat.f_flags & MNT_RDONLY) == 0)
		panic("ufs_dirbad: bad dir");
}

/*
 * Do consistency checking on a directory entry:
 *	record length must be multiple of 4
 *	entry must fit in rest of its DIRBLKSIZ block
 *	record must be large enough to contain entry
 *	name is not longer than MAXNAMLEN
 *	name must be as long as advertised, and null terminated
 */
int
ufs_dirbadentry(dp, ep, entryoffsetinblock)
	struct vnode *dp;
	register struct direct *ep;
	int entryoffsetinblock;
{
	register int i;
	int namlen;

#	if (BYTE_ORDER == LITTLE_ENDIAN)
		if (OFSFMT(dp))
			namlen = ep->d_type;
		else
			namlen = ep->d_namlen;
#	else
		namlen = ep->d_namlen;
#	endif
	if ((ep->d_reclen & 0x3) != 0 ||
	    ep->d_reclen > DIRBLKSIZ - (entryoffsetinblock & (DIRBLKSIZ - 1)) ||
	    ep->d_reclen < DIRSIZ(OFSFMT(dp), ep) || namlen > MAXNAMLEN) {
		/*return (1); */
		printf("First bad\n");
		goto bad;
	}
	if (ep->d_ino == 0)
		return (0);
	for (i = 0; i < namlen; i++)
		if (ep->d_name[i] == '\0') {
			/*return (1); */
			printf("Second bad\n");
			goto bad;
	}
	if (ep->d_name[i])
		goto bad;
	return (0);
bad:
	return (1);
}

/*
 * Construct a new directory entry after a call to namei, using the
 * parameters that it left in the componentname argument cnp. The
 * argument ip is the inode to which the new directory entry will refer.
 */
void
ufs_makedirentry(ip, cnp, newdirp)
	struct inode *ip;
	struct componentname *cnp;
	struct direct *newdirp;
{

#ifdef DIAGNOSTIC
	if ((cnp->cn_flags & SAVENAME) == 0)
		panic("ufs_makedirentry: missing name");
#endif
	newdirp->d_ino = ip->i_number;
	newdirp->d_namlen = cnp->cn_namelen;
	bcopy(cnp->cn_nameptr, newdirp->d_name, (unsigned)cnp->cn_namelen + 1);
	if (ITOV(ip)->v_mount->mnt_maxsymlinklen > 0)
		newdirp->d_type = IFTODT(ip->i_mode);
	else {
		newdirp->d_type = 0;
#		if (BYTE_ORDER == LITTLE_ENDIAN)
			{ u_char tmp = newdirp->d_namlen;
			newdirp->d_namlen = newdirp->d_type;
			newdirp->d_type = tmp; }
#		endif
	}
}

/*
 * Write a directory entry after a call to namei, using the parameters
 * that it left in nameidata. The argument dirp is the new directory
 * entry contents. Dvp is a pointer to the directory to be written,
 * which was left locked by namei. Remaining parameters (dp->i_offset, 
 * dp->i_count) indicate how the space for the new entry is to be obtained.
 * Non-null bp indicates that a directory is being created (for the
 * soft dependency code).
 */
int
ufs_direnter(dvp, tvp, dirp, cnp, newdirbp)
	struct vnode *dvp;
	struct vnode *tvp;
	struct direct *dirp;
	struct componentname *cnp;
	struct buf *newdirbp;
{
	struct ucred *cr;
	struct proc *p;
	int newentrysize;
	struct inode *dp;
	struct buf *bp;
	u_int dsize;
	struct direct *ep, *nep;
	int error, ret, blkoff, loc, spacefree, flags;
	char *dirbuf;

	p = curproc;	/* XXX */
	cr = p->p_ucred;

	dp = VTOI(dvp);
	newentrysize = DIRSIZ(OFSFMT(dvp), dirp);

	if (dp->i_count == 0) {
		/*
		 * If dp->i_count is 0, then namei could find no
		 * space in the directory. Here, dp->i_offset will
		 * be on a directory block boundary and we will write the
		 * new entry into a fresh block.
		 */
		if (dp->i_offset & (DIRBLKSIZ - 1))
			panic("ufs_direnter: newblk");
		flags = B_CLRBUF;
		if (!DOINGSOFTDEP(dvp) && !DOINGASYNC(dvp))
			flags |= B_SYNC;
		if ((error = VOP_BALLOC(dvp, (off_t)dp->i_offset, DIRBLKSIZ,
		    cr, flags, &bp)) != 0) {
			if (DOINGSOFTDEP(dvp) && newdirbp != NULL)
				bdwrite(newdirbp);
			return (error);
		}
		dp->i_size = dp->i_offset + DIRBLKSIZ;
		dp->i_flag |= IN_CHANGE | IN_UPDATE;
		vnode_pager_setsize(dvp, (u_long)dp->i_size);
		dirp->d_reclen = DIRBLKSIZ;
		blkoff = dp->i_offset &
		    (VFSTOUFS(dvp->v_mount)->um_mountp->mnt_stat.f_iosize - 1);
		bcopy((caddr_t)dirp, (caddr_t)bp->b_data + blkoff,newentrysize);
		if (DOINGSOFTDEP(dvp)) {
			/*
			 * Ensure that the entire newly allocated block is a
			 * valid directory so that future growth within the
			 * block does not have to ensure that the block is
			 * written before the inode.
			 */
			blkoff += DIRBLKSIZ;
			while (blkoff < bp->b_bcount) {
				((struct direct *)
				   (bp->b_data + blkoff))->d_reclen = DIRBLKSIZ;
				blkoff += DIRBLKSIZ;
			}
			softdep_setup_directory_add(bp, dp, dp->i_offset,
			    dirp->d_ino, newdirbp);
			bdwrite(bp);
			return (UFS_UPDATE(dvp, 0));
		}
		if (DOINGASYNC(dvp)) {
			bdwrite(bp);
			return (UFS_UPDATE(dvp, 0));
		}
		error = BUF_WRITE(bp);
		ret = UFS_UPDATE(dvp, 1);
		if (error == 0)
			return (ret);
		return (error);
	}

	/*
	 * If dp->i_count is non-zero, then namei found space for the new
	 * entry in the range dp->i_offset to dp->i_offset + dp->i_count
	 * in the directory. To use this space, we may have to compact
	 * the entries located there, by copying them together towards the
	 * beginning of the block, leaving the free space in one usable
	 * chunk at the end.
	 */

	/*
	 * Increase size of directory if entry eats into new space.
	 * This should never push the size past a new multiple of
	 * DIRBLKSIZE.
	 *
	 * N.B. - THIS IS AN ARTIFACT OF 4.2 AND SHOULD NEVER HAPPEN.
	 */
	if (dp->i_offset + dp->i_count > dp->i_size)
		dp->i_size = dp->i_offset + dp->i_count;
	/*
	 * Get the block containing the space for the new directory entry.
	 */
	error = UFS_BLKATOFF(dvp, (off_t)dp->i_offset, &dirbuf, &bp);
	if (error) {
		if (DOINGSOFTDEP(dvp) && newdirbp != NULL)
			bdwrite(newdirbp);
		return (error);
	}
	/*
	 * Find space for the new entry. In the simple case, the entry at
	 * offset base will have the space. If it does not, then namei
	 * arranged that compacting the region dp->i_offset to
	 * dp->i_offset + dp->i_count would yield the space.
	 */
	ep = (struct direct *)dirbuf;
	dsize = DIRSIZ(OFSFMT(dvp), ep);
	spacefree = ep->d_reclen - dsize;
	for (loc = ep->d_reclen; loc < dp->i_count; ) {
		nep = (struct direct *)(dirbuf + loc);
		if (ep->d_ino) {
			/* trim the existing slot */
			ep->d_reclen = dsize;
			ep = (struct direct *)((char *)ep + dsize);
		} else {
			/* overwrite; nothing there; header is ours */
			spacefree += dsize;
		}
		dsize = DIRSIZ(OFSFMT(dvp), nep);
		spacefree += nep->d_reclen - dsize;
		loc += nep->d_reclen;
		if (DOINGSOFTDEP(dvp))
			softdep_change_directoryentry_offset(dp, dirbuf,
			    (caddr_t)nep, (caddr_t)ep, dsize); 
		else
			bcopy((caddr_t)nep, (caddr_t)ep, dsize);
	}
	/*
	 * Update the pointer fields in the previous entry (if any),
	 * copy in the new entry, and write out the block.
	 */
	if (ep->d_ino == 0 ||
	    (ep->d_ino == WINO &&
	     bcmp(ep->d_name, dirp->d_name, dirp->d_namlen) == 0)) {
		if (spacefree + dsize < newentrysize)
			panic("ufs_direnter: compact1");
		dirp->d_reclen = spacefree + dsize;
	} else {
		if (spacefree < newentrysize)
			panic("ufs_direnter: compact2");
		dirp->d_reclen = spacefree;
		ep->d_reclen = dsize;
		ep = (struct direct *)((char *)ep + dsize);
	}
	bcopy((caddr_t)dirp, (caddr_t)ep, (u_int)newentrysize);

	if (DOINGSOFTDEP(dvp)) {
		softdep_setup_directory_add(bp, dp,
		    dp->i_offset + (caddr_t)ep - dirbuf, dirp->d_ino, newdirbp);
		bdwrite(bp);
	} else {
		if (DOINGASYNC(dvp)) {
			bdwrite(bp);
			error = 0;
		} else {
			error = bowrite(bp);
		}
	}
	dp->i_flag |= IN_CHANGE | IN_UPDATE;
	/*
	 * If all went well, and the directory can be shortened, proceed
	 * with the truncation. Note that we have to unlock the inode for
	 * the entry that we just entered, as the truncation may need to
	 * lock other inodes which can lead to deadlock if we also hold a
	 * lock on the newly entered node.
	 */
	if (error == 0 && dp->i_endoff && dp->i_endoff < dp->i_size) {
		if (tvp != NULL)
			VOP_UNLOCK(tvp, 0, p);
		(void) UFS_TRUNCATE(dvp, (off_t)dp->i_endoff, IO_SYNC, cr, p);
		if (tvp != NULL)
			vn_lock(tvp, LK_EXCLUSIVE | LK_RETRY, p);
	}
	return (error);
}

/*
 * Remove a directory entry after a call to namei, using
 * the parameters which it left in nameidata. The entry
 * dp->i_offset contains the offset into the directory of the
 * entry to be eliminated.  The dp->i_count field contains the
 * size of the previous record in the directory.  If this
 * is 0, the first entry is being deleted, so we need only
 * zero the inode number to mark the entry as free.  If the
 * entry is not the first in the directory, we must reclaim
 * the space of the now empty record by adding the record size
 * to the size of the previous entry.
 */
int
ufs_dirremove(dvp, ip, flags, isrmdir)
	struct vnode *dvp;
	struct inode *ip;
	int flags;
	int isrmdir;
{
	struct inode *dp;
	struct direct *ep;
	struct buf *bp;
	int error;

	dp = VTOI(dvp);

	if (flags & DOWHITEOUT) {
		/*
		 * Whiteout entry: set d_ino to WINO.
		 */
		if ((error =
		    UFS_BLKATOFF(dvp, (off_t)dp->i_offset, (char **)&ep, &bp)) != 0)
			return (error);
		ep->d_ino = WINO;
		ep->d_type = DT_WHT;
		goto out;
	}

	if ((error = UFS_BLKATOFF(dvp,
	    (off_t)(dp->i_offset - dp->i_count), (char **)&ep, &bp)) != 0)
		return (error);
	if (dp->i_count == 0) {
		/*
		 * First entry in block: set d_ino to zero.
		 */
		ep->d_ino = 0;
	} else {
		/*
		 * Collapse new free space into previous entry.
		 */
		ep->d_reclen += dp->i_reclen;
	}
out:
	if (DOINGSOFTDEP(dvp)) {
		if (ip) {
			ip->i_effnlink--;
			softdep_change_linkcnt(ip);
			softdep_setup_remove(bp, dp, ip, isrmdir);
		}
		bdwrite(bp);
	} else {
		if (ip) {
			ip->i_effnlink--;
			ip->i_nlink--;
			ip->i_flag |= IN_CHANGE;
		}
		if (flags & DOWHITEOUT)
			error = BUF_WRITE(bp);
		else if (DOINGASYNC(dvp) && dp->i_count != 0) {
			bdwrite(bp);
			error = 0;
		} else
			error = bowrite(bp);
	}
	dp->i_flag |= IN_CHANGE | IN_UPDATE;
	return (error);
}

/*
 * Rewrite an existing directory entry to point at the inode
 * supplied.  The parameters describing the directory entry are
 * set up by a call to namei.
 */
int
ufs_dirrewrite(dp, oip, newinum, newtype, isrmdir)
	struct inode *dp, *oip;
	ino_t newinum;
	int newtype;
	int isrmdir;
{
	struct buf *bp;
	struct direct *ep;
	struct vnode *vdp = ITOV(dp);
	int error;

	error = UFS_BLKATOFF(vdp, (off_t)dp->i_offset, (char **)&ep, &bp);
	if (error)
		return (error);
	ep->d_ino = newinum;
	if (!OFSFMT(vdp))
		ep->d_type = newtype;
	oip->i_effnlink--;
	if (DOINGSOFTDEP(vdp)) {
		softdep_change_linkcnt(oip);
		softdep_setup_directory_change(bp, dp, oip, newinum, isrmdir);
		bdwrite(bp);
	} else {
		oip->i_nlink--;
		oip->i_flag |= IN_CHANGE;
		if (DOINGASYNC(vdp)) {
			bdwrite(bp);
			error = 0;
		} else {
			error = bowrite(bp);
		}
	}
	dp->i_flag |= IN_CHANGE | IN_UPDATE;
	return (error);
}

/*
 * Check if a directory is empty or not.
 * Inode supplied must be locked.
 *
 * Using a struct dirtemplate here is not precisely
 * what we want, but better than using a struct direct.
 *
 * NB: does not handle corrupted directories.
 */
int
ufs_dirempty(ip, parentino, cred)
	register struct inode *ip;
	ino_t parentino;
	struct ucred *cred;
{
	register off_t off;
	struct dirtemplate dbuf;
	register struct direct *dp = (struct direct *)&dbuf;
	int error, count, namlen;
#define	MINDIRSIZ (sizeof (struct dirtemplate) / 2)

	for (off = 0; off < ip->i_size; off += dp->d_reclen) {
		error = vn_rdwr(UIO_READ, ITOV(ip), (caddr_t)dp, MINDIRSIZ, off,
		   UIO_SYSSPACE, IO_NODELOCKED, cred, &count, (struct proc *)0);
		/*
		 * Since we read MINDIRSIZ, residual must
		 * be 0 unless we're at end of file.
		 */
		if (error || count != 0)
			return (0);
		/* avoid infinite loops */
		if (dp->d_reclen == 0)
			return (0);
		/* skip empty entries */
		if (dp->d_ino == 0 || dp->d_ino == WINO)
			continue;
		/* accept only "." and ".." */
#		if (BYTE_ORDER == LITTLE_ENDIAN)
			if (OFSFMT(ITOV(ip)))
				namlen = dp->d_type;
			else
				namlen = dp->d_namlen;
#		else
			namlen = dp->d_namlen;
#		endif
		if (namlen > 2)
			return (0);
		if (dp->d_name[0] != '.')
			return (0);
		/*
		 * At this point namlen must be 1 or 2.
		 * 1 implies ".", 2 implies ".." if second
		 * char is also "."
		 */
		if (namlen == 1 && dp->d_ino == ip->i_number)
			continue;
		if (dp->d_name[1] == '.' && dp->d_ino == parentino)
			continue;
		return (0);
	}
	return (1);
}

/*
 * Check if source directory is in the path of the target directory.
 * Target is supplied locked, source is unlocked.
 * The target is always vput before returning.
 */
int
ufs_checkpath(source, target, cred)
	struct inode *source, *target;
	struct ucred *cred;
{
	struct vnode *vp;
	int error, rootino, namlen;
	struct dirtemplate dirbuf;

	vp = ITOV(target);
	if (target->i_number == source->i_number) {
		error = EEXIST;
		goto out;
	}
	rootino = ROOTINO;
	error = 0;
	if (target->i_number == rootino)
		goto out;

	for (;;) {
		if (vp->v_type != VDIR) {
			error = ENOTDIR;
			break;
		}
		error = vn_rdwr(UIO_READ, vp, (caddr_t)&dirbuf,
			sizeof (struct dirtemplate), (off_t)0, UIO_SYSSPACE,
			IO_NODELOCKED, cred, (int *)0, (struct proc *)0);
		if (error != 0)
			break;
#		if (BYTE_ORDER == LITTLE_ENDIAN)
			if (OFSFMT(vp))
				namlen = dirbuf.dotdot_type;
			else
				namlen = dirbuf.dotdot_namlen;
#		else
			namlen = dirbuf.dotdot_namlen;
#		endif
		if (namlen != 2 ||
		    dirbuf.dotdot_name[0] != '.' ||
		    dirbuf.dotdot_name[1] != '.') {
			error = ENOTDIR;
			break;
		}
		if (dirbuf.dotdot_ino == source->i_number) {
			error = EINVAL;
			break;
		}
		if (dirbuf.dotdot_ino == rootino)
			break;
		vput(vp);
		error = VFS_VGET(vp->v_mount, dirbuf.dotdot_ino, &vp);
		if (error) {
			vp = NULL;
			break;
		}
	}

out:
	if (error == ENOTDIR)
		printf("checkpath: .. not a directory\n");
	if (vp != NULL)
		vput(vp);
	return (error);
}
