/*  -*- c-file-style: "linux" -*-
 *
 * Copyright (C) 1996-2000 by Andrew Tridgell
 * Copyright (C) Paul Mackerras 1996
 * Copyright (C) 2001, 2002 by Martin Pool <mbp@samba.org>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/**
 * @file
 *
 * Utilities used in rsync
 **/

#include "rsync.h"

extern int verbose;
extern struct exclude_list_struct server_exclude_list;

int sanitize_paths = 0;



/**
 * Set a fd into nonblocking mode
 **/
void set_nonblocking(int fd)
{
	int val;

	if ((val = fcntl(fd, F_GETFL, 0)) == -1)
		return;
	if (!(val & NONBLOCK_FLAG)) {
		val |= NONBLOCK_FLAG;
		fcntl(fd, F_SETFL, val);
	}
}

/**
 * Set a fd into blocking mode
 **/
void set_blocking(int fd)
{
	int val;

	if ((val = fcntl(fd, F_GETFL, 0)) == -1)
		return;
	if (val & NONBLOCK_FLAG) {
		val &= ~NONBLOCK_FLAG;
		fcntl(fd, F_SETFL, val);
	}
}


/**
 * Create a file descriptor pair - like pipe() but use socketpair if
 * possible (because of blocking issues on pipes).
 *
 * Always set non-blocking.
 */
int fd_pair(int fd[2])
{
	int ret;

#if HAVE_SOCKETPAIR
	ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fd);
#else
	ret = pipe(fd);
#endif

	if (ret == 0) {
		set_nonblocking(fd[0]);
		set_nonblocking(fd[1]);
	}

	return ret;
}


void print_child_argv(char **cmd)
{
	rprintf(FINFO, "opening connection using ");
	for (; *cmd; cmd++) {
		/* Look for characters that ought to be quoted.  This
		* is not a great quoting algorithm, but it's
		* sufficient for a log message. */
		if (strspn(*cmd, "abcdefghijklmnopqrstuvwxyz"
			   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			   "0123456789"
			   ",.-_=+@/") != strlen(*cmd)) {
			rprintf(FINFO, "\"%s\" ", *cmd);
		} else {
			rprintf(FINFO, "%s ", *cmd);
		}
	}
	rprintf(FINFO, "\n");
}


void out_of_memory(char *str)
{
	rprintf(FERROR, "ERROR: out of memory in %s\n", str);
	exit_cleanup(RERR_MALLOC);
}

void overflow(char *str)
{
	rprintf(FERROR, "ERROR: buffer overflow in %s\n", str);
	exit_cleanup(RERR_MALLOC);
}



int set_modtime(char *fname, time_t modtime)
{
	extern int dry_run;
	if (dry_run)
		return 0;

	if (verbose > 2) {
		rprintf(FINFO, "set modtime of %s to (%ld) %s",
			fname, (long) modtime,
			asctime(localtime(&modtime)));
	}

	{
#ifdef HAVE_UTIMBUF
		struct utimbuf tbuf;
		tbuf.actime = time(NULL);
		tbuf.modtime = modtime;
		return utime(fname,&tbuf);
#elif defined(HAVE_UTIME)
		time_t t[2];
		t[0] = time(NULL);
		t[1] = modtime;
		return utime(fname,t);
#else
		struct timeval t[2];
		t[0].tv_sec = time(NULL);
		t[0].tv_usec = 0;
		t[1].tv_sec = modtime;
		t[1].tv_usec = 0;
		return utimes(fname,t);
#endif
	}
}


/**
   Create any necessary directories in fname. Unfortunately we don't know
   what perms to give the directory when this is called so we need to rely
   on the umask
**/
int create_directory_path(char *fname, int base_umask)
{
	char *p;

	while (*fname == '/')
		fname++;
	while (strncmp(fname, "./", 2) == 0)
		fname += 2;

	p = fname;
	while ((p = strchr(p,'/')) != NULL) {
		*p = 0;
		do_mkdir(fname, 0777 & ~base_umask);
		*p = '/';
		p++;
	}
	return 0;
}


/**
 * Write @p len bytes at @p ptr to descriptor @p desc, retrying if
 * interrupted.
 *
 * @retval len upon success
 *
 * @retval <0 write's (negative) error code
 *
 * Derived from GNU C's cccp.c.
 */
static int full_write(int desc, char *ptr, size_t len)
{
	int total_written;

	total_written = 0;
	while (len > 0) {
		int written = write(desc, ptr, len);
		if (written < 0)  {
			if (errno == EINTR)
				continue;
			return written;
		}
		total_written += written;
		ptr += written;
		len -= written;
	}
	return total_written;
}


/**
 * Read @p len bytes at @p ptr from descriptor @p desc, retrying if
 * interrupted.
 *
 * @retval >0 the actual number of bytes read
 *
 * @retval 0 for EOF
 *
 * @retval <0 for an error.
 *
 * Derived from GNU C's cccp.c. */
static int safe_read(int desc, char *ptr, size_t len)
{
	int n_chars;

	if (len == 0)
		return len;

	do {
		n_chars = read(desc, ptr, len);
	} while (n_chars < 0 && errno == EINTR);

	return n_chars;
}


/** Copy a file.
 *
 * This is used in conjunction with the --temp-dir option */
int copy_file(char *source, char *dest, mode_t mode)
{
	int ifd;
	int ofd;
	char buf[1024 * 8];
	int len;   /* Number of bytes read into `buf'. */

	ifd = do_open(source, O_RDONLY, 0);
	if (ifd == -1) {
		rprintf(FERROR,"open %s: %s\n",
			full_fname(source), strerror(errno));
		return -1;
	}

	if (robust_unlink(dest) && errno != ENOENT) {
		rprintf(FERROR,"unlink %s: %s\n",
			full_fname(dest), strerror(errno));
		return -1;
	}

	ofd = do_open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, mode);
	if (ofd == -1) {
		rprintf(FERROR,"open %s: %s\n",
			full_fname(dest), strerror(errno));
		close(ifd);
		return -1;
	}

	while ((len = safe_read(ifd, buf, sizeof buf)) > 0) {
		if (full_write(ofd, buf, len) < 0) {
			rprintf(FERROR,"write %s: %s\n",
				full_fname(dest), strerror(errno));
			close(ifd);
			close(ofd);
			return -1;
		}
	}

	if (len < 0) {
		rprintf(FERROR, "read %s: %s\n",
			full_fname(source), strerror(errno));
		close(ifd);
		close(ofd);
		return -1;
	}

	if (close(ifd) < 0) {
		rprintf(FINFO, "close failed on %s: %s\n",
			full_fname(source), strerror(errno));
	}

	if (close(ofd) < 0) {
		rprintf(FERROR, "close failed on %s: %s\n",
			full_fname(dest), strerror(errno));
		return -1;
	}

	return 0;
}

/* MAX_RENAMES should be 10**MAX_RENAMES_DIGITS */
#define MAX_RENAMES_DIGITS 3
#define MAX_RENAMES 1000

/**
 * Robust unlink: some OS'es (HPUX) refuse to unlink busy files, so
 * rename to <path>/.rsyncNNN instead.
 *
 * Note that successive rsync runs will shuffle the filenames around a
 * bit as long as the file is still busy; this is because this function
 * does not know if the unlink call is due to a new file coming in, or
 * --delete trying to remove old .rsyncNNN files, hence it renames it
 * each time.
 **/
int robust_unlink(char *fname)
{
#ifndef ETXTBSY
	return do_unlink(fname);
#else
	static int counter = 1;
	int rc, pos, start;
	char path[MAXPATHLEN];

	rc = do_unlink(fname);
	if (rc == 0 || errno != ETXTBSY)
		return rc;

	if ((pos = strlcpy(path, fname, MAXPATHLEN)) >= MAXPATHLEN)
		pos = MAXPATHLEN - 1;

	while (pos > 0 && path[pos-1] != '/')
		pos--;
	pos += strlcpy(path+pos, ".rsync", MAXPATHLEN-pos);

	if (pos > (MAXPATHLEN-MAX_RENAMES_DIGITS-1)) {
		errno = ETXTBSY;
		return -1;
	}

	/* start where the last one left off to reduce chance of clashes */
	start = counter;
	do {
		sprintf(&path[pos], "%03d", counter);
		if (++counter >= MAX_RENAMES)
			counter = 1;
	} while ((rc = access(path, 0)) == 0 && counter != start);

	if (verbose > 0) {
		rprintf(FINFO,"renaming %s to %s because of text busy\n",
			fname, path);
	}

	/* maybe we should return rename()'s exit status? Nah. */
	if (do_rename(fname, path) != 0) {
		errno = ETXTBSY;
		return -1;
	}
	return 0;
#endif
}

/* Returns 0 on success, -1 on most errors, and -2 if we got an error
 * trying to copy the file across file systems. */
int robust_rename(char *from, char *to, int mode)
{
	int tries = 4;

	while (tries--) {
		if (do_rename(from, to) == 0)
			return 0;

		switch (errno) {
#ifdef ETXTBSY
		case ETXTBSY:
			if (robust_unlink(to) != 0)
				return -1;
			break;
#endif
		case EXDEV:
			if (copy_file(from, to, mode) != 0)
				return -2;
			do_unlink(from);
			return 0;
		default:
			return -1;
		}
	}
	return -1;
}


static pid_t all_pids[10];
static int num_pids;

/** Fork and record the pid of the child. **/
pid_t do_fork(void)
{
	pid_t newpid = fork();

	if (newpid != 0  &&  newpid != -1) {
		all_pids[num_pids++] = newpid;
	}
	return newpid;
}

/**
 * Kill all children.
 *
 * @todo It would be kind of nice to make sure that they are actually
 * all our children before we kill them, because their pids may have
 * been recycled by some other process.  Perhaps when we wait for a
 * child, we should remove it from this array.  Alternatively we could
 * perhaps use process groups, but I think that would not work on
 * ancient Unix versions that don't support them.
 **/
void kill_all(int sig)
{
	int i;

	for (i = 0; i < num_pids; i++) {
		/* Let's just be a little careful where we
		 * point that gun, hey?  See kill(2) for the
		 * magic caused by negative values. */
		pid_t p = all_pids[i];

		if (p == getpid())
			continue;
		if (p <= 0)
			continue;

		kill(p, sig);
	}
}


/** Turn a user name into a uid */
int name_to_uid(char *name, uid_t *uid)
{
	struct passwd *pass;
	if (!name || !*name) return 0;
	pass = getpwnam(name);
	if (pass) {
		*uid = pass->pw_uid;
		return 1;
	}
	return 0;
}

/** Turn a group name into a gid */
int name_to_gid(char *name, gid_t *gid)
{
	struct group *grp;
	if (!name || !*name) return 0;
	grp = getgrnam(name);
	if (grp) {
		*gid = grp->gr_gid;
		return 1;
	}
	return 0;
}


/** Lock a byte range in a open file */
int lock_range(int fd, int offset, int len)
{
	struct flock lock;

	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = offset;
	lock.l_len = len;
	lock.l_pid = 0;

	return fcntl(fd,F_SETLK,&lock) == 0;
}

static int exclude_server_path(char *arg)
{
	char *s;

	if (server_exclude_list.head) {
		for (s = arg; (s = strchr(s, '/')) != NULL; ) {
			*s = '\0';
			if (check_exclude(&server_exclude_list, arg, 1) < 0) {
				/* We must leave arg truncated! */
				return 1;
			}
			*s++ = '/';
		}
	}
	return 0;
}

static void glob_expand_one(char *s, char **argv, int *argc, int maxargs)
{
#if !(defined(HAVE_GLOB) && defined(HAVE_GLOB_H))
	if (!*s) s = ".";
	s = argv[*argc] = strdup(s);
	exclude_server_path(s);
	(*argc)++;
#else
	extern int sanitize_paths;
	glob_t globbuf;
	int i;

	if (!*s) s = ".";

	s = argv[*argc] = strdup(s);
	if (sanitize_paths) {
		sanitize_path(s, NULL);
	}

	memset(&globbuf, 0, sizeof globbuf);
	if (!exclude_server_path(s))
		glob(s, 0, NULL, &globbuf);
	if (globbuf.gl_pathc == 0) {
		(*argc)++;
		globfree(&globbuf);
		return;
	}
	for (i = 0; i < maxargs - *argc && i < (int)globbuf.gl_pathc; i++) {
		if (i == 0)
			free(s);
		argv[*argc + i] = strdup(globbuf.gl_pathv[i]);
		if (!argv[*argc + i])
			out_of_memory("glob_expand");
	}
	globfree(&globbuf);
	*argc += i;
#endif
}

/* This routine is only used in daemon mode. */
void glob_expand(char *base1, char **argv, int *argc, int maxargs)
{
	char *s = argv[*argc];
	char *p, *q;
	char *base = base1;
	int base_len = strlen(base);

	if (!s || !*s) return;

	if (strncmp(s, base, base_len) == 0)
		s += base_len;

	s = strdup(s);
	if (!s) out_of_memory("glob_expand");

	if (asprintf(&base," %s/", base1) <= 0) out_of_memory("glob_expand");
	base_len++;

	q = s;
	while ((p = strstr(q,base)) != NULL && *argc < maxargs) {
		/* split it at this point */
		*p = 0;
		glob_expand_one(q, argv, argc, maxargs);
		q = p + base_len;
	}

	if (*q && *argc < maxargs)
		glob_expand_one(q, argv, argc, maxargs);

	free(s);
	free(base);
}

/**
 * Convert a string to lower case
 **/
void strlower(char *s)
{
	while (*s) {
		if (isupper(* (unsigned char *) s))
			*s = tolower(* (unsigned char *) s);
		s++;
	}
}

/* Join strings p1 & p2 into "dest" with a guaranteed '/' between them.  (If
 * p1 ends with a '/', no extra '/' is inserted.)  Returns the length of both
 * strings + 1 (if '/' was inserted), regardless of whether the null-terminated
 * string fits into destsize. */
size_t pathjoin(char *dest, size_t destsize, const char *p1, const char *p2)
{
	size_t len = strlcpy(dest, p1, destsize);
	if (len < destsize - 1) {
		if (!len || dest[len-1] != '/')
			dest[len++] = '/';
		if (len < destsize - 1)
			len += strlcpy(dest + len, p2, destsize - len);
		else {
			dest[len] = '\0';
			len += strlen(p2);
		}
	}
	else
		len += strlen(p2) + 1; /* Assume we'd insert a '/'. */
	return len;
}

/* Join any number of strings together, putting them in "dest".  The return
 * value is the length of all the strings, regardless of whether the null-
 * terminated whole fits in destsize.  Your list of string pointers must end
 * with a NULL to indicate the end of the list. */
size_t stringjoin(char *dest, size_t destsize, ...)
{
	va_list ap;
	size_t len, ret = 0;
	const char *src;

	va_start(ap, destsize);
	while (1) {
		if (!(src = va_arg(ap, const char *)))
			break;
		len = strlen(src);
		ret += len;
		if (destsize > 1) {
			if (len >= destsize)
				len = destsize - 1;
			memcpy(dest, src, len);
			destsize -= len;
			dest += len;
		}
	}
	*dest = '\0';
	va_end(ap);

	return ret;
}

void clean_fname(char *name)
{
	char *p;
	int l;
	int modified = 1;

	if (!name) return;

	while (modified) {
		modified = 0;

		if ((p = strstr(name,"/./")) != NULL) {
			modified = 1;
			while (*p) {
				p[0] = p[2];
				p++;
			}
		}

		if ((p = strstr(name,"//")) != NULL) {
			modified = 1;
			while (*p) {
				p[0] = p[1];
				p++;
			}
		}

		if (strncmp(p = name, "./", 2) == 0) {
			modified = 1;
			do {
				p[0] = p[2];
			} while (*p++);
		}

		l = strlen(p = name);
		if (l > 1 && p[l-1] == '/') {
			modified = 1;
			p[l-1] = 0;
		}
	}
}

/**
 * Make path appear as if a chroot had occurred:
 *
 * @li 1. remove leading "/" (or replace with "." if at end)
 *
 * @li 2. remove leading ".." components (except those allowed by @p reldir)
 *
 * @li 3. delete any other "<dir>/.." (recursively)
 *
 * Can only shrink paths, so sanitizes in place.
 *
 * While we're at it, remove double slashes and "." components like
 *   clean_fname() does, but DON'T remove a trailing slash because that
 *   is sometimes significant on command line arguments.
 *
 * If @p reldir is non-null, it is a sanitized directory that the path will be
 *    relative to, so allow as many ".." at the beginning of the path as
 *    there are components in reldir.  This is used for symbolic link targets.
 *    If reldir is non-null and the path began with "/", to be completely like
 *    a chroot we should add in depth levels of ".." at the beginning of the
 *    path, but that would blow the assumption that the path doesn't grow and
 *    it is not likely to end up being a valid symlink anyway, so just do
 *    the normal removal of the leading "/" instead.
 *
 * Contributed by Dave Dykstra <dwd@bell-labs.com>
 */
void sanitize_path(char *p, char *reldir)
{
	char *start, *sanp;
	int depth = 0;
	int allowdotdot = 0;

	if (reldir) {
		depth++;
		while (*reldir) {
			if (*reldir++ == '/') {
				depth++;
			}
		}
	}
	start = p;
	sanp = p;
	while (*p == '/') {
		/* remove leading slashes */
		p++;
	}
	while (*p != '\0') {
		/* this loop iterates once per filename component in p.
		 * both p (and sanp if the original had a slash) should
		 * always be left pointing after a slash
		 */
		if (*p == '.' && (p[1] == '/' || p[1] == '\0')) {
			/* skip "." component */
			while (*++p == '/') {
				/* skip following slashes */
				;
			}
			continue;
		}
		allowdotdot = 0;
		if (*p == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
			/* ".." component followed by slash or end */
			if (depth > 0 && sanp == start) {
				/* allow depth levels of .. at the beginning */
				--depth;
				allowdotdot = 1;
			} else {
				p += 2;
				if (*p == '/')
					p++;
				if (sanp != start) {
					/* back up sanp one level */
					--sanp; /* now pointing at slash */
					while (sanp > start && sanp[-1] != '/') {
						/* skip back up to slash */
						sanp--;
					}
				}
				continue;
			}
		}
		while (1) {
			/* copy one component through next slash */
			*sanp++ = *p++;
			if (*p == '\0' || p[-1] == '/') {
				while (*p == '/') {
					/* skip multiple slashes */
					p++;
				}
				break;
			}
		}
		if (allowdotdot) {
			/* move the virtual beginning to leave the .. alone */
			start = sanp;
		}
	}
	if (sanp == start && !allowdotdot) {
		/* ended up with nothing, so put in "." component */
		/*
		 * note that the !allowdotdot doesn't prevent this from
		 *  happening in all allowed ".." situations, but I didn't
		 *  think it was worth putting in an extra variable to ensure
		 *  it since an extra "." won't hurt in those situations.
		 */
		*sanp++ = '.';
	}
	*sanp = '\0';
}

/* Works much like sanitize_path(), with these differences:  (1) a new buffer
 * is allocated for the sanitized path rather than modifying it in-place; (2)
 * a leading slash gets transformed into the rootdir value (which can be empty
 * or NULL if you just want the slash to get dropped); (3) no "reldir" can be
 * specified. */
char *alloc_sanitize_path(const char *path, const char *rootdir)
{
	char *buf;
	int rlen, plen = strlen(path);

	if (*path == '/' && rootdir) {
		rlen = strlen(rootdir);
		if (rlen == 1)
			path++;
	} else
		rlen = 0;
	if (!(buf = new_array(char, rlen + plen + 1)))
		out_of_memory("alloc_sanitize_path");
	if (rlen)
		memcpy(buf, rootdir, rlen);
	memcpy(buf + rlen, path, plen + 1);

	if (rlen > 1)
		rlen++;
	sanitize_path(buf + rlen, NULL);
	if (rlen && buf[rlen] == '.' && buf[rlen+1] == '\0') {
		if (rlen > 1)
			rlen--;
		buf[rlen] = '\0';
	}

	return buf;
}

char curr_dir[MAXPATHLEN];
unsigned int curr_dir_len;

/**
 * Like chdir(), but it keeps track of the current directory (in the
 * global "curr_dir"), and ensures that the path size doesn't overflow.
 * Also cleans the path using the clean_fname() function.
 **/
int push_dir(char *dir)
{
	static int initialised;
	unsigned int len;

	if (!initialised) {
		initialised = 1;
		getcwd(curr_dir, sizeof curr_dir - 1);
		curr_dir_len = strlen(curr_dir);
	}

	if (!dir)	/* this call was probably just to initialize */
		return 0;

	len = strlen(dir);
	if (len == 1 && *dir == '.')
		return 1;

	if ((*dir == '/' ? len : curr_dir_len + 1 + len) >= sizeof curr_dir)
		return 0;

	if (chdir(dir))
		return 0;

	if (*dir == '/') {
		memcpy(curr_dir, dir, len + 1);
		curr_dir_len = len;
	} else {
		curr_dir[curr_dir_len++] = '/';
		memcpy(curr_dir + curr_dir_len, dir, len + 1);
		curr_dir_len += len;
	}

	clean_fname(curr_dir);

	return 1;
}

/**
 * Reverse a push_dir() call.  You must pass in an absolute path
 * that was copied from a prior value of "curr_dir".
 **/
int pop_dir(char *dir)
{
	if (chdir(dir))
		return 0;

	curr_dir_len = strlcpy(curr_dir, dir, sizeof curr_dir);
	if (curr_dir_len >= sizeof curr_dir)
		curr_dir_len = sizeof curr_dir - 1;

	return 1;
}

/**
 * Return a quoted string with the full pathname of the indicated filename.
 * The string " (in MODNAME)" may also be appended.  The returned pointer
 * remains valid until the next time full_fname() is called.
 **/
char *full_fname(char *fn)
{
	extern int module_id;
	static char *result = NULL;
	char *m1, *m2, *m3;
	char *p1, *p2;

	if (result)
		free(result);

	if (*fn == '/')
		p1 = p2 = "";
	else {
		p1 = curr_dir;
		p2 = "/";
	}
	if (module_id >= 0) {
		m1 = " (in ";
		m2 = lp_name(module_id);
		m3 = ")";
		if (*p1) {
			if (!lp_use_chroot(module_id)) {
				char *p = lp_path(module_id);
				if (*p != '/' || p[1])
					p1 += strlen(p);
			}
			if (!*p1)
				p2++;
			else
				p1++;
		}
		else
			fn++;
	} else
		m1 = m2 = m3 = "";

	asprintf(&result, "\"%s%s%s\"%s%s%s", p1, p2, fn, m1, m2, m3);

	return result;
}

/** We need to supply our own strcmp function for file list comparisons
   to ensure that signed/unsigned usage is consistent between machines. */
int u_strcmp(const char *cs1, const char *cs2)
{
	const uchar *s1 = (const uchar *)cs1;
	const uchar *s2 = (const uchar *)cs2;

	while (*s1 && *s2 && (*s1 == *s2)) {
		s1++; s2++;
	}

	return (int)*s1 - (int)*s2;
}



/**
 * Determine if a symlink points outside the current directory tree.
 * This is considered "unsafe" because e.g. when mirroring somebody
 * else's machine it might allow them to establish a symlink to
 * /etc/passwd, and then read it through a web server.
 *
 * Null symlinks and absolute symlinks are always unsafe.
 *
 * Basically here we are concerned with symlinks whose target contains
 * "..", because this might cause us to walk back up out of the
 * transferred directory.  We are not allowed to go back up and
 * reenter.
 *
 * @param dest Target of the symlink in question.
 *
 * @param src Top source directory currently applicable.  Basically this
 * is the first parameter to rsync in a simple invocation, but it's
 * modified by flist.c in slightly complex ways.
 *
 * @retval True if unsafe
 * @retval False is unsafe
 *
 * @sa t_unsafe.c
 **/
int unsafe_symlink(const char *dest, const char *src)
{
	const char *name, *slash;
	int depth = 0;

	/* all absolute and null symlinks are unsafe */
	if (!dest || !*dest || *dest == '/') return 1;

	/* find out what our safety margin is */
	for (name = src; (slash = strchr(name, '/')) != 0; name = slash+1) {
		if (strncmp(name, "../", 3) == 0) {
			depth = 0;
		} else if (strncmp(name, "./", 2) == 0) {
			/* nothing */
		} else {
			depth++;
		}
	}
	if (strcmp(name, "..") == 0)
		depth = 0;

	for (name = dest; (slash = strchr(name, '/')) != 0; name = slash+1) {
		if (strncmp(name, "../", 3) == 0) {
			/* if at any point we go outside the current directory
			   then stop - it is unsafe */
			if (--depth < 0)
				return 1;
		} else if (strncmp(name, "./", 2) == 0) {
			/* nothing */
		} else {
			depth++;
		}
	}
	if (strcmp(name, "..") == 0)
		depth--;

	return (depth < 0);
}


/**
 * Return the date and time as a string
 **/
char *timestring(time_t t)
{
	static char TimeBuf[200];
	struct tm *tm = localtime(&t);

#ifdef HAVE_STRFTIME
	strftime(TimeBuf, sizeof TimeBuf - 1, "%Y/%m/%d %H:%M:%S", tm);
#else
	strlcpy(TimeBuf, asctime(tm), sizeof TimeBuf);
#endif

	if (TimeBuf[strlen(TimeBuf)-1] == '\n') {
		TimeBuf[strlen(TimeBuf)-1] = 0;
	}

	return(TimeBuf);
}


/**
 * Sleep for a specified number of milliseconds.
 *
 * Always returns TRUE.  (In the future it might return FALSE if
 * interrupted.)
 **/
int msleep(int t)
{
	int tdiff = 0;
	struct timeval tval, t1, t2;

	gettimeofday(&t1, NULL);
	gettimeofday(&t2, NULL);

	while (tdiff < t) {
		tval.tv_sec = (t-tdiff)/1000;
		tval.tv_usec = 1000*((t-tdiff)%1000);

		errno = 0;
		select(0,NULL,NULL, NULL, &tval);

		gettimeofday(&t2, NULL);
		tdiff = (t2.tv_sec - t1.tv_sec)*1000 +
			(t2.tv_usec - t1.tv_usec)/1000;
	}

	return True;
}


/**
 * Determine if two file modification times are equivalent (either
 * exact or in the modification timestamp window established by
 * --modify-window).
 *
 * @retval 0 if the times should be treated as the same
 *
 * @retval +1 if the first is later
 *
 * @retval -1 if the 2nd is later
 **/
int cmp_modtime(time_t file1, time_t file2)
{
	extern int modify_window;

	if (file2 > file1) {
		if (file2 - file1 <= modify_window) return 0;
		return -1;
	}
	if (file1 - file2 <= modify_window) return 0;
	return 1;
}


#ifdef __INSURE__XX
#include <dlfcn.h>

/**
   This routine is a trick to immediately catch errors when debugging
   with insure. A xterm with a gdb is popped up when insure catches
   a error. It is Linux specific.
**/
int _Insure_trap_error(int a1, int a2, int a3, int a4, int a5, int a6)
{
	static int (*fn)();
	int ret;
	char *cmd;

	asprintf(&cmd, "/usr/X11R6/bin/xterm -display :0 -T Panic -n Panic -e /bin/sh -c 'cat /tmp/ierrs.*.%d ; gdb /proc/%d/exe %d'",
		getpid(), getpid(), getpid());

	if (!fn) {
		static void *h;
		h = dlopen("/usr/local/parasoft/insure++lite/lib.linux2/libinsure.so", RTLD_LAZY);
		fn = dlsym(h, "_Insure_trap_error");
	}

	ret = fn(a1, a2, a3, a4, a5, a6);

	system(cmd);

	free(cmd);

	return ret;
}
#endif


#define MALLOC_MAX 0x40000000

void *_new_array(unsigned int size, unsigned long num)
{
	if (num >= MALLOC_MAX/size)
		return NULL;
	return malloc(size * num);
}

void *_realloc_array(void *ptr, unsigned int size, unsigned long num)
{
	if (num >= MALLOC_MAX/size)
		return NULL;
	/* No realloc should need this, but just in case... */
	if (!ptr)
		return malloc(size * num);
	return realloc(ptr, size * num);
}
