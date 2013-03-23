/*
   Copyright (C) Andrew Tridgell 1996
   Copyright (C) Paul Mackerras 1996

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "rsync.h"

#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif

extern int verbose;
extern int csum_length;
extern struct stats stats;
extern int io_error;
extern int dry_run;
extern int am_server;
extern int am_daemon;
extern int protocol_version;
extern int make_backups;
#ifdef EA_SUPPORT
extern int extended_attributes;
#endif
extern struct stats stats;


/**
 * @file
 *
 * The sender gets checksums from the generator, calculates deltas,
 * and transmits them to the receiver.  The sender process runs on the
 * machine holding the source files.
 **/
void read_sum_head(int f, struct sum_struct *sum)
{
	sum->count = read_int(f);
	sum->blength = read_int(f);
	if (protocol_version < 27) {
		sum->s2length = csum_length;
	} else {
		sum->s2length = read_int(f);
		if (sum->s2length > MD4_SUM_LENGTH) {
			rprintf(FERROR, "Invalid checksum length %ld\n",
			    (long)sum->s2length);
			exit_cleanup(RERR_PROTOCOL);
		}
	}
	sum->remainder = read_int(f);
}

/**
 * Receive the checksums for a buffer
 **/
static struct sum_struct *receive_sums(int f)
{
	struct sum_struct *s;
	int i;
	OFF_T offset = 0;

	if (!(s = new(struct sum_struct)))
		out_of_memory("receive_sums");

	read_sum_head(f, s);

	s->sums = NULL;

	if (verbose > 3) {
		rprintf(FINFO, "count=%ld n=%u rem=%u\n",
			(long)s->count, s->blength, s->remainder);
	}

	if (s->count == 0)
		return(s);

	if (!(s->sums = new_array(struct sum_buf, s->count)))
		out_of_memory("receive_sums");

	for (i = 0; i < (int)s->count; i++) {
		s->sums[i].sum1 = read_int(f);
		read_buf(f, s->sums[i].sum2, s->s2length);

		s->sums[i].offset = offset;
		s->sums[i].flags = 0;

		if (i == (int)s->count-1 && s->remainder != 0)
			s->sums[i].len = s->remainder;
		else
			s->sums[i].len = s->blength;
		offset += s->sums[i].len;

		if (verbose > 3) {
			rprintf(FINFO,
				"chunk[%d] len=%d offset=%.0f sum1=%08x\n",
				i, s->sums[i].len, (double)s->sums[i].offset,
				s->sums[i].sum1);
		}
	}

	s->flength = offset;

	return s;
}



void send_files(struct file_list *flist, int f_out, int f_in)
{
	int fd = -1;
	struct sum_struct *s;
	struct map_struct *mbuf = NULL;
	STRUCT_STAT st;
	char *fname2, fname[MAXPATHLEN];
	int i;
	struct file_struct *file;
	int phase = 0;
	struct stats initial_stats;
	int save_make_backups = make_backups;
	int j;
#if HAVE_COPYFILE
	char fname_tmp[MAXPATHLEN];
#endif

	if (verbose > 2)
		rprintf(FINFO, "send_files starting\n");

	while (1) {
		unsigned int offset;

		i = read_int(f_in);
		if (i == -1) {
			if (phase == 0) {
				phase++;
				csum_length = SUM_LENGTH;
				write_int(f_out, -1);
				if (verbose > 2)
					rprintf(FINFO, "send_files phase=%d\n", phase);
				/* For inplace: redo phase turns off the backup
				 * flag so that we do a regular inplace send. */
				make_backups = 0;
				continue;
			}
			break;
		}

		if (i < 0 || i >= flist->count) {
			rprintf(FERROR, "Invalid file index %d (count=%d)\n",
				i, flist->count);
			exit_cleanup(RERR_PROTOCOL);
		}

		file = flist->files[i];

		stats.current_file_index = i;
		stats.num_transferred_files++;
		stats.total_transferred_size += file->length;

		if (file->basedir) {
			/* N.B. We're sure that this fits, so offset is OK. */
			offset = strlcpy(fname, file->basedir, sizeof fname);
			if (!offset || fname[offset-1] != '/')
				fname[offset++] = '/';
		} else
			offset = 0;
		fname2 = f_name_to(file, fname + offset);

		if (verbose > 2)
			rprintf(FINFO, "send_files(%d, %s)\n", i, fname);

		if (dry_run) {
			if (!am_server && verbose) /* log the transfer */
				rprintf(FINFO, "%s\n", safe_fname(fname2));
			write_int(f_out, i);
			continue;
		}

		initial_stats = stats;

		if (!(s = receive_sums(f_in))) {
			io_error |= IOERR_GENERAL;
			rprintf(FERROR, "receive_sums failed\n");
			return;
		}

#ifdef HAVE_COPYFILE
		if (extended_attributes
		    && !strncmp(file->basename, "._", 2)) {
			    char fname_src[MAXPATHLEN];
			    extern char *tmpdir;

			    if (tmpdir == NULL)
				tmpdir = "/tmp";

			    strlcpy(fname_src, fname, MAXPATHLEN);

			    if (file->dirname)
			       sprintf(fname_src + offset, "%s/%s",
					   file->dirname, file->basename + 2);
			   else
			       strlcpy(fname_src + offset, file->basename + 2, MAXPATHLEN);

			   if(!get_tmpname(fname_tmp, file->basename))
			       continue;

			    if(mktemp(fname_tmp)
				&& !copyfile(fname_src, fname_tmp, NULL,
				       COPYFILE_PACK | COPYFILE_METADATA)) {
				    fd = do_open(fname_tmp, O_RDONLY, 0);
			    }
			    else
			    {
				    rprintf(FERROR, "send_files failed to open %s: %s\n",
				       full_fname(fname_tmp), strerror(errno));
				    continue;
			    }
		   } else
#endif
		fd = do_open(fname, O_RDONLY, 0);
		if (fd == -1) {
			if (errno == ENOENT) {
				enum logcode c = am_daemon
				    && protocol_version < 28 ? FERROR
							     : FINFO;
				io_error |= IOERR_VANISHED;
				rprintf(c, "file has vanished: %s\n",
					full_fname(fname));
			} else {
				io_error |= IOERR_GENERAL;
				rsyserr(FERROR, errno,
					"send_files failed to open %s",
					full_fname(fname));
			}
			free_sums(s);
			continue;
		}

		/* map the local file */
		if (do_fstat(fd, &st) != 0) {
			io_error |= IOERR_GENERAL;
			rsyserr(FERROR, errno, "fstat failed");
			free_sums(s);
			close(fd);
			return;
		}

		if (st.st_size) {
			OFF_T map_size = MAX(s->blength * 3, MAX_MAP_SIZE);
			mbuf = map_file(fd, st.st_size, map_size, s->blength);
		} else
			mbuf = NULL;

		if (verbose > 2) {
			rprintf(FINFO, "send_files mapped %s of size %.0f\n",
				safe_fname(fname), (double)st.st_size);
		}

		write_int(f_out, i);
		write_sum_head(f_out, s);

		if (verbose > 2) {
			rprintf(FINFO, "calling match_sums %s\n",
				safe_fname(fname));
		}

		if (!am_server && verbose) /* log the transfer */
			rprintf(FINFO, "%s\n", safe_fname(fname2));

		set_compression(fname);

		match_sums(f_out, s, mbuf, st.st_size);
		log_send(file, &initial_stats);

		if (mbuf) {
			j = unmap_file(mbuf);
			if (j) {
				io_error |= IOERR_GENERAL;
				rsyserr(FERROR, j,
					"read errors mapping %s",
					full_fname(fname));
			}
		}
		close(fd);

		free_sums(s);

		if (verbose > 2) {
			rprintf(FINFO, "sender finished %s\n",
				safe_fname(fname));
		}
#if HAVE_COPYFILE
		if (extended_attributes
		    && !strncmp(file->basename, "._", 2)) {
				    unlink(fname_tmp);
		}
#endif
	}
	make_backups = save_make_backups;

	if (verbose > 2)
		rprintf(FINFO, "send files finished\n");

	match_report();

	write_int(f_out, -1);
}
