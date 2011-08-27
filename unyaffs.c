/*
 * unyaffs: extract files from yaffs2 file system image to current directory
 *
 * Created by Kai Wei <kai.wei.cn@gmail.com>
 * Modified by Bernhard Ehlers <be@bernhard-ehlers.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * History:
 * V0.1  2008-12-29
 *   Initial version uploaded to http://code.google.com/p/unyaffs/
 * V0.8  2011-08-25
 *   Fork created at https://github.com/ehlers/unyaffs
 *   Support of chunksizes from 2k to 16k
 *   Restore special files (device nodes)
 *   Restore file date and time
 *   Restore file ownership, when run as root
 *   File listing
 *   Much more error checking
 */

#define VERSION		"0.8"

/* check if lutimes is available */
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || (defined(__APPLE__) && defined(__MACH__))
#define HAS_LUTIMES 1
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#ifdef HAS_LUTIMES
#include <sys/time.h>
#else
#include <utime.h>
#endif

#include "unyaffs.h"

#define MAX_CHUNK_SIZE		16384
#define MAX_SPARE_SIZE		  512
#define HASH_SIZE		 7001
#define MAX_WARN		   20
#define YAFFS_OBJECTID_ROOT	    1

#define STD_PERMS		(S_IRWXU|S_IRWXG|S_IRWXO)
#define EXTRA_PERMS		(S_ISUID|S_ISGID|S_ISVTX)

static struct t_layout {
	int chunk_size;
	int spare_size;
} possible_layouts[] =
	{ { 2048, 64 }, { 4096, 128 }, { 8192, 256 }, { 16384, 512 } };

int max_layout = sizeof(possible_layouts) / sizeof(struct t_layout);

unsigned char data[MAX_CHUNK_SIZE + MAX_SPARE_SIZE];
unsigned char buffer[2*(MAX_CHUNK_SIZE + MAX_SPARE_SIZE)];
unsigned char *chunk_data = data;
unsigned char *spare_data = NULL;
int chunk_size = 2048;
int spare_size = 64;
int buf_len = 0;
int buf_idx = 0;
int chunk_no   = 0;
int warn_count = 0;
int img_file;
int opt_list;
int opt_verbose;

typedef struct _object {
	unsigned id;
	struct _object *next;
	yaffs_ObjectType type;
	unsigned prev_dir_id;
	__u32    atime;
	__u32    mtime;
	char     path_name[1];		/* variable length, must be last */
} object;

object *obj_list[HASH_SIZE];

unsigned last_dir_id;

int set_utime(const char *filename, __u32 yst_atime, __u32 yst_mtime) {
#ifdef HAS_LUTIMES
	struct timeval ftime[2];

	ftime[0].tv_sec  = yst_atime;
	ftime[0].tv_usec = 0;
	ftime[1].tv_sec  = yst_mtime;
	ftime[1].tv_usec = 0;

	return lutimes(filename, ftime);
#else
	struct utimbuf ftime;

	ftime.actime  = yst_atime;
	ftime.modtime = yst_mtime;

	return utime(filename, &ftime);
#endif
}

/* error reporting function, similar to GNU error() */
static void prt_err(int status, int errnum, const char *format, ...) {
	va_list varg;

	va_start(varg, format);
	fflush(stdout);
	vfprintf(stderr, format, varg);
	if (errnum != 0)
		fprintf(stderr, ": %s", strerror(errnum));
	fprintf(stderr, "\n");
	va_end(varg);

	if (status != 0)
		exit(status);
}

/* read function, which handles partial and interrupted reads */
ssize_t xread(int fd, void *buf, size_t len) {
	char *ptr = buf;
	ssize_t offset, ret;

	offset = 0;
	while (offset < len) {
		ret = read(fd, ptr+offset, len-offset);
		if (ret < 0) {
			if (errno != EAGAIN && errno != EINTR)
				return -1;
		} else if (ret == 0)
			break;
		else
			offset += ret;
	}
	return offset;
}

/* write function, which handles partial and interrupted writes */
ssize_t xwrite(int fd, void *buf, size_t len) {
	char *ptr = buf;
	ssize_t offset, ret;

	offset = 0;
	while (offset < len) {
		ret = write(fd, ptr+offset, len-offset);
		if (ret < 0) {
			if (errno != EAGAIN && errno != EINTR)
				return -1;
		} else if (ret == 0)
			break;
		else
			offset += ret;
	}
	return offset;
}

/*
 * mkdirpath - creates directories including intermediate dirs
 */
static int mkdirpath(const char *name) {
	struct stat st;
	char *cp;
	char *buf;
	int ret;

	ret = 0;
	if ((buf = malloc(strlen(name)+2)) == NULL)
		{ errno = ENOMEM; return -1; }
	strcpy(buf, name); strcat(buf, "/");
	for (cp = buf; *cp == '/'; cp++);
	while ((cp = strchr(cp, '/')) != NULL) {
		*cp = '\0';
		if (mkdir(buf, STD_PERMS) < 0 && errno != EEXIST)
			{ ret = -1; break; }
		*cp++ = '/';
	}
	free(buf);

	if (ret >= 0 &&
	    (ret = stat(name, &st)) >= 0 && !S_ISDIR(st.st_mode)) {
		ret = -1; errno = ENOTDIR;
	}
	return ret;
}

static void init_obj_list(void) {
	object *obj;
	unsigned idx;

	for (idx = 0; idx < HASH_SIZE; idx++)
		obj_list[idx] = NULL;
	last_dir_id = 0;

	obj = malloc(offsetof(object, path_name) + 2);
	if (obj == NULL)
		prt_err(1, 0, "Malloc struct object failed.");

	obj->id = YAFFS_OBJECTID_ROOT;
	obj->type = YAFFS_OBJECT_TYPE_DIRECTORY;
	obj->prev_dir_id = 0;
	obj->atime = obj->mtime = 0;
	strcpy(obj->path_name, ".");
	idx = obj->id % HASH_SIZE;
	obj->next = obj_list[idx];
	obj_list[idx] = obj;
}

static object *get_object(unsigned id) {
	object *obj;

	obj = obj_list[id % HASH_SIZE];
	while (obj != NULL && obj->id != id)
		obj = obj->next;
	return obj;
}

static object *add_object(yaffs_ObjectHeader *oh, yaffs_PackedTags2 *pt) {
	object *obj, *parent;
	unsigned idx;

	obj = get_object(pt->t.objectId);
	if (pt->t.objectId == YAFFS_OBJECTID_ROOT) {
		if (obj == NULL)
			prt_err(1, 0, "Missing root object");
		if (oh->type != YAFFS_OBJECT_TYPE_DIRECTORY)
			prt_err(1, 0, "Root object must be directory");
		if (last_dir_id == 0)
			last_dir_id = YAFFS_OBJECTID_ROOT;
	} else {
		if (oh->type != YAFFS_OBJECT_TYPE_FILE &&
		    oh->type != YAFFS_OBJECT_TYPE_DIRECTORY &&
		    oh->type != YAFFS_OBJECT_TYPE_SYMLINK &&
		    oh->type != YAFFS_OBJECT_TYPE_HARDLINK &&
		    oh->type != YAFFS_OBJECT_TYPE_SPECIAL &&
		    oh->type != YAFFS_OBJECT_TYPE_UNKNOWN)
			prt_err(1, 0, "Illegal type %d in object %u (%s)",
			        oh->type, pt->t.objectId, oh->name);
		if (oh->name[0] == '\0' || strchr(oh->name, '/') != NULL ||
		    strcmp(oh->name, ".") == 0 || strcmp(oh->name, "..") == 0)
			prt_err(1, 0, "Illegal file name %s in object %u",
			        oh->name, pt->t.objectId);
		if (obj != NULL)
			prt_err(1, 0, "Duplicate objectId %u", pt->t.objectId);
		parent = get_object(oh->parentObjectId);
		if (parent == NULL)
			prt_err(1, 0, "Invalid parentObjectId %u in object %u (%s)",
	        		oh->parentObjectId, pt->t.objectId, oh->name);
		if (parent->type != YAFFS_OBJECT_TYPE_DIRECTORY)
			prt_err(1, ENOTDIR, "File %s can't be created in %s",
	        		oh->name, parent->path_name);
		obj = malloc(offsetof(object, path_name) +
		             strlen(parent->path_name) + strlen(oh->name) + 2);
		if (obj == NULL)
			prt_err(1, 0, "Malloc struct object failed.");

		obj->id = pt->t.objectId;
		obj->type = oh->type;
		if (obj->type == YAFFS_OBJECT_TYPE_DIRECTORY) {
			obj->prev_dir_id = last_dir_id;
			last_dir_id = obj->id;
		} else
			obj->prev_dir_id = 0;
		if (strcmp(parent->path_name, ".") == 0) {
			strcpy(obj->path_name, oh->name);
		} else {
			strcpy(obj->path_name, parent->path_name);
			strcat(obj->path_name, "/");
			strcat(obj->path_name, oh->name);
		}
		idx = obj->id % HASH_SIZE;
		obj->next = obj_list[idx];
		obj_list[idx] = obj;
	}

	obj->atime = oh->yst_atime;
	obj->mtime = oh->yst_mtime;

	return obj;
}

void set_dirs_utime(void) {
	unsigned id;
	object *obj;

	id = last_dir_id;
	while (id != 0 && (obj = get_object(id)) != NULL) {
		set_utime(obj->path_name, obj->atime, obj->mtime);
		id = obj->prev_dir_id;
	}
}

static void prt_node(char *name, yaffs_ObjectHeader *oh) {
	object *eq_obj;
	struct tm tm;
	time_t mtime;
	mode_t mode;
	char type;
	char fsize[16];
	char perm[10];

	/* get file type, size, mtine and mode */
	eq_obj = NULL;
	strcpy(fsize, "0");
	mtime = oh->yst_mtime;
	mode  = oh->yst_mode;
	switch(oh->type) {
		case YAFFS_OBJECT_TYPE_FILE:		type = '-';
			snprintf(fsize, sizeof(fsize), "%d", oh->fileSize);
			break;
		case YAFFS_OBJECT_TYPE_DIRECTORY:	type = 'd'; break;
		case YAFFS_OBJECT_TYPE_SYMLINK:		type = 'l'; break;
		case YAFFS_OBJECT_TYPE_HARDLINK:	type = 'h';
			eq_obj = get_object(oh->equivalentObjectId);
			mtime = eq_obj != NULL ? eq_obj->mtime : 0;
			mode = STD_PERMS;
			break;
		case YAFFS_OBJECT_TYPE_SPECIAL:
			switch (oh->yst_mode & S_IFMT) {
				case S_IFBLK:		type = 'b';
					snprintf(fsize, sizeof(fsize), "%d,%4d",
					         major(oh->yst_rdev),
					         minor(oh->yst_rdev));
					break;
				case S_IFCHR:		type = 'c';
					snprintf(fsize, sizeof(fsize), "%d,%4d",
					         major(oh->yst_rdev),
					         minor(oh->yst_rdev));
					break;
				case S_IFIFO:		type = 'p'; break;
				case S_IFSOCK:		type = 's'; break;
				default:		type = '?'; break;
			}
			break;
		default:				type = '?'; break;
	}

	/* get file permissions */
	perm[0] = mode & S_IRUSR ? 'r' : '-';
	perm[1] = mode & S_IWUSR ? 'w' : '-';
	perm[2] = mode & S_IXUSR ? 'x' : '-';
	perm[3] = mode & S_IRGRP ? 'r' : '-';
	perm[4] = mode & S_IWGRP ? 'w' : '-';
	perm[5] = mode & S_IXGRP ? 'x' : '-';
	perm[6] = mode & S_IROTH ? 'r' : '-';
	perm[7] = mode & S_IWOTH ? 'w' : '-';
	perm[8] = mode & S_IXOTH ? 'x' : '-';
	if (mode & S_ISUID) perm[2] = perm[2] == '-' ? 'S' : 's';
	if (mode & S_ISGID) perm[5] = perm[5] == '-' ? 'S' : 's';
	if (mode & S_ISVTX) perm[8] = perm[8] == '-' ? 'T' : 't';
	perm[9] = '\0';

	/* print file infos */
	localtime_r(&mtime, &tm);
	printf("%c%s %8s %4d-%02d-%02d %02d:%02d %s",
	       type, perm, fsize,
	       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	       tm.tm_hour, tm.tm_min, name);

	/* link destination */
	if (oh->type == YAFFS_OBJECT_TYPE_HARDLINK) {
		if (eq_obj == NULL)
			printf(" -> !!! Invalid !!!");
		else
			printf(" -> /%s", eq_obj->path_name);
	} else if (oh->type == YAFFS_OBJECT_TYPE_SYMLINK) {
		printf(" -> %s", oh->alias);
	}
	printf("\n");
}

int read_chunk(void);

void process_chunk(void) {
	yaffs_ObjectHeader oh;
	yaffs_PackedTags2 *pt;
	object *obj, *eq_obj;
	int out_file, remain, s;

	oh = *(yaffs_ObjectHeader *)chunk_data;
	pt = (yaffs_PackedTags2 *)spare_data;

	if (pt->t.byteCount == 0xffffffff)	/* empty object */
		return;
	else if (pt->t.byteCount != 0xffff) {	/* not a new object */
		prt_err(0, 0, "Warning: Invalid header at chunk #%d, skipping...",
		        chunk_no);
		if (++warn_count >= MAX_WARN)
			prt_err(1, 0, "Giving up");
		return;
	}

	obj = add_object(&oh, pt);

	/* listing */
	if (opt_verbose)
		prt_node(obj->path_name, &oh);
	else if (opt_list)
		printf("%s\n", obj->path_name);
	if (opt_list) {
		if (oh.type == YAFFS_OBJECT_TYPE_FILE) {
			remain = oh.fileSize;	/* skip over data chunks */
			while(remain > 0) {
				if (!read_chunk())
					prt_err(1, 0, "Broken image file");
				remain -= pt->t.byteCount;
			}
		}
		return;
	}

	switch(oh.type) {
		case YAFFS_OBJECT_TYPE_FILE:
			remain = oh.fileSize;
			out_file = creat(obj->path_name, oh.yst_mode & STD_PERMS);
			if (out_file < 0)
				prt_err(1, errno, "Can't create file %s", obj->path_name);
			while(remain > 0) {
				if (!read_chunk())
					prt_err(1, 0, "Broken image file");
				s = (remain < pt->t.byteCount) ? remain : pt->t.byteCount;
				if (xwrite(out_file, chunk_data, s) < 0)
					prt_err(1, errno, "Can't write to %s", obj->path_name);
				remain -= s;
			}
			close(out_file);
			lchown(obj->path_name, oh.yst_uid, oh.yst_gid);
			if ((oh.yst_mode & EXTRA_PERMS) != 0 &&
			    chmod(obj->path_name, oh.yst_mode) < 0)
				prt_err(0, errno, "Warning: Can't chmod %s", obj->path_name);
			break;
		case YAFFS_OBJECT_TYPE_SYMLINK:
			if (symlink(oh.alias, obj->path_name) < 0)
				prt_err(1, errno, "Can't create symlink %s", obj->path_name);
			lchown(obj->path_name, oh.yst_uid, oh.yst_gid);
			break;
		case YAFFS_OBJECT_TYPE_DIRECTORY:
			if (pt->t.objectId != YAFFS_OBJECTID_ROOT &&
			    mkdir(obj->path_name, oh.yst_mode & STD_PERMS) < 0)
					prt_err(1, errno, "Can't create directory %s", obj->path_name);
			lchown(obj->path_name, oh.yst_uid, oh.yst_gid);
			if ((pt->t.objectId == YAFFS_OBJECTID_ROOT ||
			     (oh.yst_mode & EXTRA_PERMS) != 0) &&
			    chmod(obj->path_name, oh.yst_mode) < 0)
				prt_err(0, errno, "Warning: Can't chmod %s", obj->path_name);
			break;
		case YAFFS_OBJECT_TYPE_HARDLINK:
			eq_obj = get_object(oh.equivalentObjectId);
			if (eq_obj == NULL)
				prt_err(1, 0, "Invalid equivalentObjectId %u in object %u (%s)",
				        oh.equivalentObjectId, pt->t.objectId, oh.name);
			if (link(eq_obj->path_name, obj->path_name) < 0)
				prt_err(1, errno, "Can't create hardlink %s", obj->path_name);
			break;
		case YAFFS_OBJECT_TYPE_SPECIAL:
			if (mknod(obj->path_name, oh.yst_mode, oh.yst_rdev) < 0) {
				if (errno == EPERM || errno == EINVAL)
					prt_err(0, errno, "Warning: Can't create device %s", obj->path_name);
				else
					prt_err(1, errno, "Can't create device %s", obj->path_name);
			}
			lchown(obj->path_name, oh.yst_uid, oh.yst_gid);
			break;
		case YAFFS_OBJECT_TYPE_UNKNOWN:
			break;
	}

	/* set file date and time */
	switch(oh.type) {
		case YAFFS_OBJECT_TYPE_FILE:
		case YAFFS_OBJECT_TYPE_SPECIAL:
#ifdef HAS_LUTIMES
		case YAFFS_OBJECT_TYPE_SYMLINK:
#endif
			set_utime(obj->path_name,
			          oh.yst_atime, oh.yst_mtime);
			break;
		case YAFFS_OBJECT_TYPE_DIRECTORY:
		default:
			break;
	}
}


int read_chunk(void) {
	ssize_t s, len, offset;

	chunk_no++;
	len = chunk_size + spare_size;
	offset = 0;
	memset(chunk_data, 0xff, sizeof(chunk_data));

	if (buf_len > buf_idx) {		/* copy from buffer */
		s = buf_len - buf_idx;
		if (s > len) s = len;
		memcpy(data, buffer+buf_idx, s);
		buf_idx += s; offset += s;
	}

	if (offset < len) {			/* read from file */
		s = xread(img_file, data+offset, len-offset);
		if (s < 0)
			prt_err(1, errno, "Read image file");
		offset += s;
	}

	if (offset != 0 && offset != len)	/* partial chunk */
		prt_err(1, 0, "Broken image file");

	return offset != 0;
}

void detect_chunk_size(void) {
	yaffs_ObjectHeader *oh;
	yaffs_PackedTags2  *pt, *pt2;
	int      i;

	memset(buffer, 0xff, sizeof(buffer));
	buf_len = xread(img_file, buffer, sizeof(buffer));
	if (buf_len < 0)
		prt_err(1, errno, "Read image file");

	oh = (yaffs_ObjectHeader *)buffer;
	if (oh->parentObjectId != YAFFS_OBJECTID_ROOT ||
	    (oh->type          != YAFFS_OBJECT_TYPE_FILE &&
	     oh->type          != YAFFS_OBJECT_TYPE_DIRECTORY &&
	     oh->type          != YAFFS_OBJECT_TYPE_SYMLINK &&
	     oh->type          != YAFFS_OBJECT_TYPE_HARDLINK &&
	     oh->type          != YAFFS_OBJECT_TYPE_SPECIAL))
		prt_err(1, 0, "Not a yaffs2 image");

	for (i = 0; i < max_layout; i++) {
 		pt  = (yaffs_PackedTags2 *)
		      (buffer + possible_layouts[i].chunk_size);
		pt2 = (yaffs_PackedTags2 *)
		      (buffer + 2 * possible_layouts[i].chunk_size +
		       possible_layouts[i].spare_size);

		if (pt->t.byteCount == 0xffff && pt->t.chunkId == 0 &&
		    ((pt2->t.byteCount == 0xffff && pt2->t.chunkId == 0) ||
		     (pt2->t.objectId == pt->t.objectId && pt2->t.chunkId == 1)))
			break;
	}

	if (i >= max_layout)
		prt_err(1, 0, "Can't determine chunk size");

	chunk_size = possible_layouts[i].chunk_size;
	spare_size = possible_layouts[i].spare_size;
	if (opt_verbose)
		fprintf(stderr,
		        "Header check OK, chunk size = %d, spare size = %d.\n",
		        chunk_size, spare_size);
}

void usage(void) {
	fprintf(stderr, "\
unyaffs - extract files from a YAFFS2 file system image.\n\
\n\
Usage: unyaffs [-l <layout>] [-t] [-v] [-V] <image_file_name> [<base dir>]\n\
    -l <layout>      set flash memory layout\n\
        layout=0: detect chunk and spare size (default)\n\
        layout=1:  2K chunk,  64 byte spare size\n\
        layout=2:  4K chunk, 128 byte spare size\n\
        layout=3:  8K chunk, 256 byte spare size\n\
        layout=4: 16K chunk, 512 byte spare size\n\
    -t               list image contents\n\
    -v               verbose output\n\
    -V               print version\n\
");
	exit(1);
}

int main(int argc, char **argv) {
	int ch;
	int layout = 0;

	/* handle command line options */
	opt_list = 0;
	opt_verbose = 0;
	while ((ch = getopt(argc, argv, "l:tvVh?")) > 0) {
		switch (ch) {
			case 'l':
				if (optarg[0] < '0' ||
				    optarg[0] > '0' + max_layout ||
				    optarg[1] != '\0') usage();
				layout = optarg[0] - '0';
				break;
			case 't':
				opt_list = 1;
				break;
			case 'v':
				opt_verbose = 1;
				break;
			case 'V':
				printf("V%s\n", VERSION);
				exit(0);
				break;
			case 'h':
			case '?':
			default:
				usage();
				break;
    		}
	}

	/* extract rest of command line parameters */
	if ((argc - optind) < 1 || (argc - optind) > 2)
		usage();

	if (strcmp(argv[optind], "-") == 0) {	/* image file from stdin ? */
		img_file = 0;
	} else {
		img_file = open(argv[optind], O_RDONLY);
		if (img_file < 0)
			prt_err(1, errno, "Open image file failed");
	}

	if (layout == 0) {
		detect_chunk_size();
	} else {
		chunk_size = possible_layouts[layout-1].chunk_size;
		spare_size = possible_layouts[layout-1].spare_size;
	}
	spare_data = data + chunk_size;

	if ((argc - optind) == 2 && !opt_list) {
		if (mkdirpath(argv[optind+1]) < 0)
			prt_err(1, errno, "Can't mkdir %s", argv[optind+1]);
		if (chdir(argv[optind+1]) < 0)
			prt_err(1, errno, "Can't chdir to %s", argv[optind+1]);
	}

	umask(0);

	init_obj_list();
	while (read_chunk()) {
		process_chunk();
	}
	set_dirs_utime();
	close(img_file);
	return 0;
}
