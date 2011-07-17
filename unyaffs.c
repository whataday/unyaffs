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
#ifdef HAS_LUTIMES
#include <sys/time.h>
#else
#include <utime.h>
#endif

#include "unyaffs.h"

#define CHUNK_SIZE 2048
#define SPARE_SIZE 64
#define MAX_OBJECTS 10000
#define MAX_DIRS    1000
#define MAX_WARN    20
#define YAFFS_OBJECTID_ROOT     1


unsigned char data[CHUNK_SIZE + SPARE_SIZE];
unsigned char *chunk_data = data;
unsigned char *spare_data = data + CHUNK_SIZE;
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

int set_utime(const char *filename, __u32 yst_atime, __u32 yst_mtime)
{
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

void set_dirs_utime(void)
{
	int i;
	for (i = dir_count-1; i >= 0; i--)
		set_utime(dir_list[i].path_name,
		          dir_list[i].yst_atime, dir_list[i].yst_mtime);
}

int read_chunk(void);

int process_chunk(void)
{
	int out_file, remain, s;
	char *full_path_name;

	yaffs_PackedTags2 *pt = (yaffs_PackedTags2 *)spare_data;
	if (pt->t.byteCount == 0xffff)  {	//a new object 

		yaffs_ObjectHeader oh = *(yaffs_ObjectHeader *)chunk_data;
		if (pt->t.objectId >= MAX_OBJECTS) {
			fprintf(stderr, "ObjectId %u (%s) out of range.\n",
			        pt->t.objectId, oh.name);
			exit(1);
		}
		if (oh.parentObjectId >= MAX_OBJECTS || obj_list[oh.parentObjectId] == NULL) {
			fprintf(stderr, "Invalid parentObjectId %u in object %u (%s)\n",
			        oh.parentObjectId, pt->t.objectId, oh.name);
			exit(1);
		}

		full_path_name = (char *)malloc(strlen(oh.name) + strlen(obj_list[oh.parentObjectId]) + 2);
		if (full_path_name == NULL) {
			fprintf(stderr, "malloc full path name.\n");
			exit(1);
		}
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
					if (read_chunk())
						return -1;
					s = (remain < pt->t.byteCount) ? remain : pt->t.byteCount;	
					if (write(out_file, chunk_data, s) == -1)
						return -1;
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
				if (oh.equivalentObjectId >= MAX_OBJECTS || obj_list[oh.equivalentObjectId] == NULL) {
					fprintf(stderr, "Invalid equivalentObjectId %u in object %u (%s)\n",
			        		oh.equivalentObjectId, pt->t.objectId, oh.name);
					exit(1);
				}
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
				if (dir_count >= MAX_DIRS) {
					fprintf(stderr, "Too many directories (more than %d).\n",
			        		MAX_DIRS);
					exit(1);
				}
				dir_list[dir_count].path_name = full_path_name;
				dir_list[dir_count].yst_atime = oh.yst_atime;
				dir_list[dir_count].yst_mtime = oh.yst_mtime;
				dir_count++;
				break;
			default:
				break;
		}
	} else {
		fprintf(stderr, "Warning: Invalid header at chunk #%d, skipping...\n",
		        chunk_no);
		if (++warn_count >= MAX_WARN) {
			fprintf(stderr, "Giving up\n");
			exit(1);
		}
	}
	return 0;
}


int read_chunk(void)
{
	ssize_t s;
	int ret = -1;

	chunk_no++;
	memset(chunk_data, 0xff, sizeof(chunk_data));
	s = read(img_file, data, CHUNK_SIZE + SPARE_SIZE);
	if (s == -1) {
		perror("read image file\n");
	} else if (s == 0) {
		printf("end of image\n");
	} else if ((s == (CHUNK_SIZE + SPARE_SIZE))) {
		ret = 0;
	} else {
		fprintf(stderr, "broken image file\n");
	}
	return ret;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		printf("Usage: unyaffs image_file_name\n");
		exit(1);
	}
	img_file = open(argv[1], O_RDONLY);
	if (img_file == -1) {
		printf("open image file failed\n");
		exit(1);
	}

	umask(0);

	obj_list[YAFFS_OBJECTID_ROOT] = ".";
	while(1) {
		if (read_chunk() == -1)
			break;
		process_chunk();
	}
	set_dirs_utime();
	close(img_file);
	return 0;
}
