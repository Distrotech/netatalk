/*
 * $Id: macbin.h,v 1.2.16.1 2010-01-28 17:15:52 didg Exp $
 */

#ifndef _MACBIN_H
#define _MACBIN_H 1

/* Forward Declarations */
struct FHeader;

int bin_open(char *binfile, int flags, struct FHeader *fh, int options);
int bin_close(int keepflag);
ssize_t bin_read(int fork, char *buffer, size_t length);
ssize_t bin_write(int fork, char *buffer, size_t length);
int bin_header_read(struct FHeader *fh, int revision);
int bin_header_write(struct FHeader *fh);
int test_header(void);
u_short updcrc();

#endif /* _MACBIN_H */
