/*
 * unyaffs: extract files from yaffs2 file system image to current directory
 *
 * Created by Kai Wei <kai.wei.cn@gmail.com>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

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
#include <stdarg.h>
#include <errno.h>
#ifdef HAS_LUTIMES
#include <sys/time.h>
#else
#include <utime.h>
#endif

#include "unyaffs.h"

#define MAX_CHUNK_SIZE		16384
#define MAX_SPARE_SIZE		  512
#define MAX_OBJECTS		10000
#define MAX_DIRS		 1000
#define MAX_WARN		   20
#define YAFFS_OBJECTID_ROOT	    1

static struct t_layout {
	int chunk_size;
	int spare_size;
} possible_layouts[] =
	{ { 2048, 64 }, { 4096, 128 }, { 8192, 256 }, { 16384, 512 } };

int max_layout = sizeof(possible_layouts) / sizeof(struct t_layout);

unsigned char data[MAX_CHUNK_SIZE + MAX_SPARE_SIZE];
unsigned char *chunk_data = data;
unsigned char *spare_data = NULL;
int chunk_size = 2048;
int spare_size = 64;
int chunk_no   = 0;
int warn_count = 0;
int img_file;

char *obj_list[MAX_OBJECTS];

struct {
	char *path_name;
	__u32 yst_atime;
	__u32 yst_mtime;
} dir_list[MAX_DIRS];
int dir_count = 0;

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

void set_dirs_utime(void) {
	int i;
	for (i = dir_count-1; i >= 0; i--)
		set_utime(dir_list[i].path_name,
		          dir_list[i].yst_atime, dir_list[i].yst_mtime);
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

int read_chunk(void);

void process_chunk(void) {
	int out_file, remain, s;
	char *full_path_name;

	yaffs_PackedTags2 *pt = (yaffs_PackedTags2 *)spare_data;
	if (pt->t.byteCount == 0xffff) {	/* a new object */
		yaffs_ObjectHeader oh = *(yaffs_ObjectHeader *)chunk_data;

		if (pt->t.objectId >= MAX_OBJECTS)
			prt_err(1, 0, "ObjectId %u (%s) out of range.",
			        pt->t.objectId, oh.name);
		if (oh.parentObjectId >= MAX_OBJECTS || obj_list[oh.parentObjectId] == NULL)
			prt_err(1, 0, "Invalid parentObjectId %u in object %u (%s)",
			        oh.parentObjectId, pt->t.objectId, oh.name);

		full_path_name = (char *)malloc(strlen(oh.name) + strlen(obj_list[oh.parentObjectId]) + 2);
		if (full_path_name == NULL)
			prt_err(1, 0, "Malloc full path name failed.");
		strcpy(full_path_name, obj_list[oh.parentObjectId]);
		if (oh.name[0] != '\0') {
			strcat(full_path_name, "/");
			strcat(full_path_name, oh.name);
 		}
		obj_list[pt->t.objectId] = full_path_name;

		switch(oh.type) {
			case YAFFS_OBJECT_TYPE_FILE:
				remain = oh.fileSize;
				out_file = creat(full_path_name, oh.yst_mode);
				while(remain > 0) {
					if (!read_chunk())
						prt_err(1, 0, "Broken image file");
					s = (remain < pt->t.byteCount) ? remain : pt->t.byteCount;
					if (write(out_file, chunk_data, s) < 0)
						return;
					remain -= s;
				}
				close(out_file);
				lchown(full_path_name, oh.yst_uid, oh.yst_gid);
				break;
			case YAFFS_OBJECT_TYPE_SYMLINK:
				symlink(oh.alias, full_path_name);
				lchown(full_path_name, oh.yst_uid, oh.yst_gid);
				break;
			case YAFFS_OBJECT_TYPE_DIRECTORY:
				mkdir(full_path_name, oh.yst_mode);
				lchown(full_path_name, oh.yst_uid, oh.yst_gid);
				break;
			case YAFFS_OBJECT_TYPE_HARDLINK:
				if (oh.equivalentObjectId >= MAX_OBJECTS || obj_list[oh.equivalentObjectId] == NULL)
					prt_err(1, 0, "Invalid equivalentObjectId %u in object %u (%s)",
					        oh.equivalentObjectId, pt->t.objectId, oh.name);
				link(obj_list[oh.equivalentObjectId], full_path_name);
				break;
			case YAFFS_OBJECT_TYPE_SPECIAL:
				mknod(full_path_name, oh.yst_mode, oh.yst_rdev);
				lchown(full_path_name, oh.yst_uid, oh.yst_gid);
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
				set_utime(full_path_name,
				          oh.yst_atime, oh.yst_mtime);
				break;
			case YAFFS_OBJECT_TYPE_DIRECTORY:
				if (dir_count >= MAX_DIRS)
					prt_err(1, 0, "Too many directories (more than %d).", MAX_DIRS);
				dir_list[dir_count].path_name = full_path_name;
				dir_list[dir_count].yst_atime = oh.yst_atime;
				dir_list[dir_count].yst_mtime = oh.yst_mtime;
				dir_count++;
				break;
			default:
				break;
		}
	} else {
		prt_err(0, 0, "Warning: Invalid header at chunk #%d, skipping...",
		        chunk_no);
		if (++warn_count >= MAX_WARN)
			prt_err(1, 0, "Giving up");
	}
}


int read_chunk(void) {
	ssize_t s;

	chunk_no++;
	memset(chunk_data, 0xff, sizeof(chunk_data));
	s = read(img_file, data, chunk_size + spare_size);
	if (s < 0) {
		prt_err(1, errno, "Read image file");
	} else if (s != 0 && s != (chunk_size + spare_size)) {
		prt_err(1, 0, "Broken image file");
	}
	return s != 0;
}

void detect_chunk_size(void) {
	unsigned char buf[MAX_CHUNK_SIZE + MAX_SPARE_SIZE +
	                  sizeof(yaffs_ObjectHeader)];
	yaffs_ObjectHeader *oh, *next_oh;
	yaffs_PackedTags2  *pt;
	ssize_t  len;
	int      i;

	memset(buf, 0xff, sizeof(buf));
	len = read(img_file, buf, sizeof(buf));
	if (len < 0)
		prt_err(1, errno, "Read image file");
	else if (len != sizeof(buf))
		prt_err(1, 0, "Broken image file");
	lseek(img_file, 0, SEEK_SET);

	oh = (yaffs_ObjectHeader *)buf;
	if (oh->type           != YAFFS_OBJECT_TYPE_DIRECTORY ||
	    oh->parentObjectId != YAFFS_OBJECTID_ROOT)
		prt_err(1, 0, "Not a yaffs2 image");

	for (i = 0; i < max_layout; i++) {
 		pt      = (yaffs_PackedTags2 *)
			(buf + possible_layouts[i].chunk_size);
		next_oh = (yaffs_ObjectHeader *)
			(buf + possible_layouts[i].chunk_size +
			       possible_layouts[i].spare_size);

		if (pt->t.byteCount == 0xffff &&	/* a new object */
		    pt->t.objectId  == YAFFS_OBJECTID_ROOT &&
		    next_oh->parentObjectId == YAFFS_OBJECTID_ROOT)
			break;
	}

	if (i >= max_layout)
		prt_err(1, 0, "Can't determine chunk size");

	chunk_size = possible_layouts[i].chunk_size;
	spare_size = possible_layouts[i].spare_size;
	printf("Header check OK, chunk size = %d, spare size = %d.\n",
	       chunk_size, spare_size);
}

void usage(void) {
	fprintf(stderr, "\
Usage: unyaffs [-l <layout>] image_file_name\n\
               layout=0: detect chunk and spare size (default)\n\
               layout=1:  2K chunk,  64 byte spare size\n\
               layout=2:  4K chunk, 128 byte spare size\n\
               layout=3:  8K chunk, 256 byte spare size\n\
               layout=4: 16K chunk, 512 byte spare size\n\
");
	exit(1);
}

int main(int argc, char **argv) {
	int ch;
	int layout = 0;

	/* handle command line options */
	while ((ch = getopt(argc, argv, "l:h?")) > 0) {
		switch (ch) {
			case 'l':
				if (optarg[0] < '0' ||
				    optarg[0] > '0' + max_layout ||
				    optarg[1] != '\0') usage();
				layout = optarg[0] - '0';
				break;
			case 'h':
			case '?':
			default:
				usage();
				break;
    		}
	}

	/* extract rest of command line parameters */
	if ((argc - optind) !=  1) usage();

	img_file = open(argv[optind], O_RDONLY);
	if (img_file < 0)
		prt_err(1, errno, "Open image file failed");

	if (layout == 0) {
		detect_chunk_size();
	} else {
		chunk_size = possible_layouts[layout-1].chunk_size;
		spare_size = possible_layouts[layout-1].spare_size;
	}
	spare_data = data + chunk_size;

	umask(0);

	obj_list[YAFFS_OBJECTID_ROOT] = ".";
	while (read_chunk()) {
		process_chunk();
	}
	set_dirs_utime();
	close(img_file);
	return 0;
}
