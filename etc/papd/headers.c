/*
 * $Id: headers.c,v 1.7.2.1 2002-03-12 15:44:38 srittau Exp $
 *
 * Copyright (c) 1990,1994 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" 
#endif /* HAVE_CONFIG_H */

#include <syslog.h>
#include <sys/param.h>
#include <stdio.h>

#include <netatalk/at.h>

#include "file.h"
#include "comment.h"
#include "lp.h"

int ch_title( struct papfile *, struct papfile * );

int ch_title( in, out )
    struct papfile	*in, *out;
{
    char		*start, *stop, *p, *q, c;
    int			linelength, crlflength;

    switch ( markline( in, &start, &linelength, &crlflength )) {
    case 0 :
	return( 0 );

    case -1 :
	return( CH_MORE );
    }

    stop = start + linelength;
    for ( p = start; p < stop; p++ ) {
	if ( *p == ':' ) {
	    break;
	}
    }

    for ( ; p < stop; p++ ) {
	if ( *p == '(' ) {
	    break;
	}
    }

    for ( q = p; q < stop; q++ ) {
	if ( *q == ')' ) {
	    break;
	}
    }

    if ( q < stop && p < stop ) {
	p++;
	c = *q;
	*q = '\0';
	lp_job( p );
	*q = c;
    }

    lp_write( start, linelength + crlflength );
    compop();
    CONSUME( in, linelength + crlflength );
    return( CH_DONE );
}

/*
 * "Header" comments.
 */
struct papd_comment	headers[] = {
    { "%%Title:",			0,		ch_title,	0 },
    { 0 },
};
