/*
 * $Id: nad.h,v 1.2.8.1 2003-08-09 14:35:25 srittau Exp $
 */

#ifndef _NAD_H
#define _NAD_H 1

/* Forward Declarations */
struct FHeader;

int nad_open(char *path, int openflags, struct FHeader *fh, int options);
int nad_header_read(struct FHeader *fh);
int nad_header_write(struct FHeader *fh);
int nad_read(int fork, char *forkbuf, int bufc);
int nad_write(int fork, char *forkbuf, int bufc);
int nad_close(int status);

void select_charset(int options);

#endif /* _NAD_H */
