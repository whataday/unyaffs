/*
 * definition copied from yaffs project
 */

#ifndef __UNYAFFS_H__
#define __UNYAFFS_H__


#define YAFFS_MAX_NAME_LENGTH	255
#define YAFFS_MAX_ALIAS_LENGTH	159

/* Definition of types */
typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned __u32;

typedef struct {
	unsigned sequenceNumber;
	unsigned objectId;
	unsigned chunkId;
	unsigned byteCount;
} yaffs_PackedTags2TagsPart;

typedef struct {
	unsigned char colParity;
	unsigned lineParity;
	unsigned lineParityPrime;
} yaffs_ECCOther;

typedef struct {
	yaffs_PackedTags2TagsPart t;
	yaffs_ECCOther ecc;
} yaffs_PackedTags2;

typedef enum {
	YAFFS_OBJECT_TYPE_UNKNOWN,
	YAFFS_OBJECT_TYPE_FILE,
	YAFFS_OBJECT_TYPE_SYMLINK,
	YAFFS_OBJECT_TYPE_DIRECTORY,
	YAFFS_OBJECT_TYPE_HARDLINK,
	YAFFS_OBJECT_TYPE_SPECIAL
} yaffs_ObjectType;


/* -------------------------- Object structure -------------------------------*/
/* This is the object structure as stored on NAND */

typedef struct {
	yaffs_ObjectType type;

	/* Apply to everything */
	int parentObjectId;
	__u16 sum__NoLongerUsed;	/* checksum of name. No longer used */
	char name[YAFFS_MAX_NAME_LENGTH + 1];

	/* The following apply to directories, files, symlinks - not hard links */
	__u32 yst_mode;			/* protection */

#ifdef CONFIG_YAFFS_WINCE
	__u32 notForWinCE[5];
#else
	__u32 yst_uid;
	__u32 yst_gid;
	__u32 yst_atime;
	__u32 yst_mtime;
	__u32 yst_ctime;
#endif

	/* File size applies to files only */
	int fileSize;

	/* Equivalent object id applies to hard links only. */
	int equivalentObjectId;

	/* Alias is for symlinks only. */
	char alias[YAFFS_MAX_ALIAS_LENGTH + 1];

	__u32 yst_rdev;		/* device stuff for block and char devices (major/min) */

#ifdef CONFIG_YAFFS_WINCE
	__u32 win_ctime[2];
	__u32 win_atime[2];
	__u32 win_mtime[2];
#else
	__u32 roomToGrow[6];

#endif
	__u32 inbandShadowsObject;
	__u32 inbandIsShrink;

	__u32 reservedSpace[2];
	int shadowsObject;	/* This object header shadows the specified object if > 0 */

	/* isShrink applies to object headers written when we shrink the file (ie resize) */
	__u32 isShrink;

} yaffs_ObjectHeader;

#endif
