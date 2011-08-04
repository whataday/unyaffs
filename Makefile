CFLAGS = -O2 -Wall

unyaffs: unyaffs.c unyaffs.h
	$(CC) $(CFLAGS) $(LDFLAGS) unyaffs.c -o unyaffs
