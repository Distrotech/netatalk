/*
 * $Id: enumerate.c,v 1.23.2.1 2003-02-04 19:10:30 didg Exp $
 *
 * Copyright (c) 1990,1993 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>

#include <atalk/logger.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/param.h>

#include <netatalk/endian.h>
#include <atalk/afp.h>
#include <atalk/adouble.h>
#ifdef CNID_DB
#include <atalk/cnid.h>
#endif /* CNID_DB */
#include "desktop.h"
#include "directory.h"
#include "volume.h"
#include "globals.h"
#include "file.h"
#include "filedir.h"

#define min(a,b)	((a)<(b)?(a):(b))

struct dir *
            adddir( vol, dir, name, namlen, upath, upathlen, st )
            struct vol	*vol;
struct dir	*dir;
char	*name, *upath;
int		namlen, upathlen;
struct stat *st;
{
    struct dir	*cdir, *edir;
#if AD_VERSION > AD_VERSION1
    struct adouble ad;
#endif /* AD_VERSION > AD_VERSION1 */

#ifndef USE_LASTDID
    struct stat lst, *lstp;
#endif /* USE_LASTDID */

    if ((cdir = dirnew(namlen + 1)) == NULL) {
        LOG(log_error, logtype_afpd, "adddir: malloc: %s", strerror(errno) );
        return NULL;
    }
    strcpy( cdir->d_name, name );
    cdir->d_name[namlen] = '\0';

    cdir->d_did = 0;

#if AD_VERSION > AD_VERSION1
    /* look in AD v2 header */
    memset(&ad, 0, sizeof(ad));
    if (ad_open(upath, ADFLAGS_HF|ADFLAGS_DIR, O_RDONLY, 0, &ad) >= 0) {
        /* if we can parse the AppleDouble header, retrieve the DID entry into cdir->d_did */
        memcpy(&cdir->d_did, ad_entry(&ad, ADEID_DID), sizeof(cdir->d_did));
        ad_close(&ad, ADFLAGS_HF);
    }
#endif /* AD_VERSION */

#ifdef CNID_DB
    /* add to cnid db */
    cdir->d_did = cnid_add(vol->v_db, st, dir->d_did, upath,
                           upathlen, cdir->d_did);
    /* Fail out if things go bad with CNID. */
    if (cdir->d_did == CNID_INVALID) {
        switch (errno) {
        case CNID_ERR_PARAM:
            LOG(log_error, logtype_afpd, "adddir: Incorrect parameters passed to cnid_add");
            return NULL;
        case CNID_ERR_PATH:
        case CNID_ERR_DB:
        case CNID_ERR_MAX:
            return NULL;
        }
    }
#endif /* CNID_DB */

    if (cdir->d_did == 0) {
#ifdef USE_LASTDID
        /* last way of doing DIDs */
        cdir->d_did = htonl( vol->v_lastdid++ );
#else /* USE_LASTDID */
        lstp = lstat(upath, &lst) < 0 ? st : &lst;
        /* the old way of doing DIDs (default) */
        cdir->d_did = htonl( CNID(lstp, 0) );
#endif /* USE_LASTDID */
    }

    if ((edir = dirinsert( vol, cdir ))) {
#ifndef CNID_DB
        if (edir->d_name) {
            if (strcmp(edir->d_name, cdir->d_name)) {
                LOG(log_info, logtype_afpd, "WARNING: DID conflict for '%s' and '%s'. Are these the same file?", edir->d_name, cdir->d_name);
            }
            free(cdir->d_name);
            free(cdir);
            return edir;
        }
#endif /* CNID_DB */
        /* it's not possible with LASTDID
           for CNID:
           - someone else have moved the directory.
           - it's a symlink inside the share.
           - it's an ID reused, the old directory was deleted but not
             the cnid record and the server reused the inode for 
             the new dir.
           for HASH (we should get ride of HASH) 
           - someone else have moved the directory.
           - it's an ID reused as above
           - it's a hash duplicate and we are in big trouble
        */
        edir->d_name = cdir->d_name;
        free(cdir);
        cdir = edir;
        if (!cdir->d_parent || cdir->d_parent == dir) 
            return cdir;
            
        /* the old was not in the same folder, remove and add to the new */
    	dirchildremove(cdir->d_parent, cdir);
    }

    /* parent/child directories */
    cdir->d_parent = dir;
    dirchildadd(dir, cdir);
    return( cdir );
}

/*
 * Struct to save directory reading context in. Used to prevent
 * O(n^2) searches on a directory.
 */
struct savedir {
    u_short	sd_vid;
    int		sd_did;
    int		sd_buflen;
    char	*sd_buf;
    char	*sd_last;
    int		sd_sindex;
};
#define SDBUFBRK	1024

int afp_enumerate(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct stat			st;
    static struct savedir	sd = { 0, 0, 0, NULL, NULL, 0 };
    struct vol			*vol;
    struct dir			*dir;
    struct dirent		*de;
    DIR				*dp;
    int				did, ret, esz, len, first = 1;
    char			*path, *data, *end, *start;
    u_int16_t			vid, fbitmap, dbitmap, reqcnt, actcnt = 0;
    u_int16_t			sindex, maxsz, sz = 0;

    if ( sd.sd_buflen == 0 ) {
        if (( sd.sd_buf = (char *)malloc( SDBUFBRK )) == NULL ) {
            LOG(log_error, logtype_afpd, "afp_enumerate: malloc: %s", strerror(errno) );
            *rbuflen = 0;
            return AFPERR_MISC;
        }
        sd.sd_buflen = SDBUFBRK;
    }

    ibuf += 2;

    memcpy( &vid, ibuf, sizeof( vid ));
    ibuf += sizeof( vid );

    if (( vol = getvolbyvid( vid )) == NULL ) {
        *rbuflen = 0;
        return( AFPERR_PARAM );
    }

    memcpy( &did, ibuf, sizeof( did ));
    ibuf += sizeof( did );

    if (( dir = dirlookup( vol, did )) == NULL ) {
        *rbuflen = 0;
        return( AFPERR_NODIR );
    }

    memcpy( &fbitmap, ibuf, sizeof( fbitmap ));
    fbitmap = ntohs( fbitmap );
    ibuf += sizeof( fbitmap );

    memcpy( &dbitmap, ibuf, sizeof( dbitmap ));
    dbitmap = ntohs( dbitmap );
    ibuf += sizeof( dbitmap );

    /* check for proper bitmaps -- the stuff in comments is for
     * variable directory ids. */
    if (!(fbitmap || dbitmap)
            /*|| (fbitmap & (1 << FILPBIT_PDID)) ||
              (dbitmap & (1 << DIRPBIT_PDID))*/) {
        *rbuflen = 0;
        return AFPERR_BITMAP;
    }

    memcpy( &reqcnt, ibuf, sizeof( reqcnt ));
    reqcnt = ntohs( reqcnt );
    ibuf += sizeof( reqcnt );

    memcpy( &sindex, ibuf, sizeof( sindex ));
    sindex = ntohs( sindex );
    ibuf += sizeof( sindex );

    memcpy( &maxsz, ibuf, sizeof( maxsz ));
    maxsz = ntohs( maxsz );
    ibuf += sizeof( maxsz );

    maxsz = min(maxsz, *rbuflen);

    if (( path = cname( vol, dir, &ibuf )) == NULL ) {
        *rbuflen = 0;
        return( AFPERR_NODIR );
    }
    data = rbuf + 3 * sizeof( u_int16_t );
    sz = 3 * sizeof( u_int16_t );

    /*
     * Read the directory into a pre-malloced buffer, stored
     *		len <name> \0
     * The end is indicated by a len of 0.
     */
    if ( sindex == 1 || curdir->d_did != sd.sd_did || vid != sd.sd_vid ) {
        sd.sd_last = sd.sd_buf;

        if (( dp = opendir( mtoupath(vol, path ))) == NULL ) {
            *rbuflen = 0;
            return (errno == ENOTDIR) ? AFPERR_BADTYPE : AFPERR_NODIR;
        }

        end = sd.sd_buf + sd.sd_buflen;
        for ( de = readdir( dp ); de != NULL; de = readdir( dp )) {
            if (!strcmp(de->d_name, "..") || !strcmp(de->d_name, "."))
                continue;

            if (!(validupath(vol, de->d_name)))
                continue;

            /* check for vetoed filenames */
            if (veto_file(vol->v_veto, de->d_name))
                continue;

            /* now check against too big a file */
            if (strlen(utompath(vol, de->d_name)) > MACFILELEN)
                continue;

            len = strlen(de->d_name);
            *(sd.sd_last)++ = len;

            if ( sd.sd_last + len + 2 > end ) {
                char *buf;

                start = sd.sd_buf;
                if ((buf = (char *) realloc( sd.sd_buf, sd.sd_buflen +
                                             SDBUFBRK )) == NULL ) {
                    LOG(log_error, logtype_afpd, "afp_enumerate: realloc: %s",
                        strerror(errno) );
                    closedir(dp);
                    *rbuflen = 0;
                    return AFPERR_MISC;
                }
                sd.sd_buf = buf;
                sd.sd_buflen += SDBUFBRK;
                sd.sd_last = ( sd.sd_last - start ) + sd.sd_buf;
                end = sd.sd_buf + sd.sd_buflen;
            }

            memcpy( sd.sd_last, de->d_name, len + 1 );
            sd.sd_last += len + 1;
        }
        *sd.sd_last = 0;

        sd.sd_last = sd.sd_buf;
        sd.sd_sindex = 1;

        closedir( dp );
        sd.sd_vid = vid;
        sd.sd_did = did;
    }

    /*
     * Position sd_last as dictated by sindex.
     */
    if ( sindex < sd.sd_sindex ) {
        sd.sd_sindex = 1;
        sd.sd_last = sd.sd_buf;
    }
    while ( sd.sd_sindex < sindex ) {
        len = *(sd.sd_last)++;
        if ( len == 0 ) {
            sd.sd_did = -1;	/* invalidate sd struct to force re-read */
            *rbuflen = 0;
            return( AFPERR_NOOBJ );
        }
        sd.sd_last += len + 1;
        sd.sd_sindex++;
    }

    while (( len = *(sd.sd_last)) != 0 ) {
        /*
         * If we've got all we need, send it.
         */
        if ( actcnt == reqcnt ) {
            break;
        }

        /*
         * Save the start position, in case we exceed the buffer
         * limitation, and have to back up one.
         */
        start = sd.sd_last;
        sd.sd_last++;

        if (*sd.sd_last == 0) {
            /* stat() already failed on this one */
            sd.sd_last += len + 1;
            continue;
        }

        if (stat( sd.sd_last, &st ) < 0 ) {
            /*
             * Somebody else plays with the dir, well it can be us with 
            * "Empty Trash..."
            */

            /* so the next time it won't try to stat it again
             * another solution would be to invalidate the cache with 
             * sd.sd_did = -1 but if it's not ENOENT error it will start again
             */
            *sd.sd_last = 0;
            sd.sd_last += len + 1;
            continue;
        }

        /*
         * If a fil/dir is not a dir, it's a file. This is slightly
         * inaccurate, since that means /dev/null is a file, /dev/printer
         * is a file, etc.
         */
        if ( S_ISDIR(st.st_mode)) {
            if ( dbitmap == 0 ) {
                sd.sd_last += len + 1;
                continue;
            }
            path = utompath(vol, sd.sd_last);
            dir = curdir->d_child;
            while (dir) {
                if ( strcmp( dir->d_name, path ) == 0 ) {
                    break;
                }
                dir = (dir == curdir->d_child->d_prev) ? NULL : dir->d_next;
            }
            if (!dir && ((dir = adddir( vol, curdir, path, strlen( path ),
                                        sd.sd_last, len, &st)) == NULL)) {
                *rbuflen = 0;
                return AFPERR_MISC;
            }


            if (( ret = getdirparams(vol, dbitmap, sd.sd_last, dir,
                                     &st, data + 2 * sizeof( u_char ), &esz )) != AFP_OK ) {
                *rbuflen = 0;
                return( ret );
            }

        } else {
            if ( fbitmap == 0 ) {
                sd.sd_last += len + 1;
                continue;
            }

            if (( ret = getfilparams(vol, fbitmap, utompath(vol, sd.sd_last),
                                     curdir, &st, data + 2 * sizeof( u_char ), &esz )) !=
                    AFP_OK ) {
                *rbuflen = 0;
                return( ret );
            }
        }

        /*
         * Make sure entry is an even length, possibly with a null
         * byte on the end.
         */
        if ( esz & 1 ) {
            *(data + 2 * sizeof( u_char ) + esz ) = '\0';
            esz++;
        }

        /*
         * Check if we've exceeded the size limit.
         */
        if ( maxsz < sz + esz + 2 * sizeof( u_char )) {
            if (first) { /* maxsz can't hold a single reply */
                *rbuflen = 0;
                return AFPERR_PARAM;
            }
            sd.sd_last = start;
            break;
        }

        if (first)
            first = 0;

        sz += esz + 2 * sizeof( u_char );
        *data++ = esz + 2 * sizeof( u_char );
        *data++ = S_ISDIR(st.st_mode) ? FILDIRBIT_ISDIR : FILDIRBIT_ISFILE;
        data += esz;
        actcnt++;
        sd.sd_last += len + 1;
    }

    if ( actcnt == 0 ) {
        *rbuflen = 0;
        sd.sd_did = -1;		/* invalidate sd struct to force re-read */
        return( AFPERR_NOOBJ );
    }
    sd.sd_sindex = sindex + actcnt;

    /*
     * All done, fill in misc junk in rbuf
     */
    fbitmap = htons( fbitmap );
    memcpy( rbuf, &fbitmap, sizeof( fbitmap ));
    rbuf += sizeof( fbitmap );
    dbitmap = htons( dbitmap );
    memcpy( rbuf, &dbitmap, sizeof( dbitmap ));
    rbuf += sizeof( dbitmap );
    actcnt = htons( actcnt );
    memcpy( rbuf, &actcnt, sizeof( actcnt ));
    rbuf += sizeof( actcnt );
    *rbuflen = sz;
    return( AFP_OK );
}


