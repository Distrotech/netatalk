/*
 * $Id: filedir.c,v 1.15.2.3 2002-03-12 15:09:20 srittau Exp $
 *
 * Copyright (c) 1990,1993 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <errno.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netatalk/endian.h>
#include <atalk/adouble.h>
#include <atalk/afp.h>
#include <atalk/util.h>
#ifdef CNID_DB
#include <atalk/cnid.h>
#endif /* CNID_DB */
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif /* HAVE_FCNTL_H */
#include <dirent.h>

/* STDC check */
#if STDC_HEADERS
#include <string.h>
#else /* STDC_HEADERS */
#ifndef HAVE_STRCHR
#define strchr index
#define strrchr index
#endif /* HAVE_STRCHR */
char *strchr (), *strrchr ();
#ifndef HAVE_MEMCPY
#define memcpy(d,s,n) bcopy ((s), (d), (n))
#define memmove(d,s,n) bcopy ((s), (d), (n))
#endif /* ! HAVE_MEMCPY */
#endif /* STDC_HEADERS */

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include "directory.h"
#include "desktop.h"
#include "volume.h"
#include "fork.h"
#include "file.h"
#include "globals.h"
#include "filedir.h"

int matchfile2dirperms(upath, vol, did)
/* Since it's kinda' big; I decided against an
inline function */
char	*upath;
struct vol  *vol;
int		did;
/* The below code changes the way file ownership is determined in the name of
fixing dropboxes.  It has known security problem.  See the netatalk FAQ for
more information */
{
    struct stat	st, sb;
    struct dir	*dir;
    char	adpath[50];
    int		uid;

#ifdef DEBUG
    syslog (LOG_INFO, "begin matchfile2dirperms:");
#endif /* DEBUG */

    if (stat(upath, &st ) < 0)
        syslog(LOG_ERR, "Could not stat %s: %s", upath, strerror(errno));
    strcpy (adpath, "./.AppleDouble/");
    strcat (adpath, upath);
    if (( dir = dirsearch( vol, did )) == NULL ) {
        syslog (LOG_ERR, "matchfile2dirperms: Unable to get directory info.");
        return( AFPERR_NOOBJ );
    }
    else if (stat(".", &sb) < 0) {
        syslog (LOG_ERR,
                "matchfile2dirperms: Error checking directory \"%s\": %s",
                dir->d_name, strerror(errno));
        return(AFPERR_NOOBJ );
    }
    else {
        uid=geteuid();
        if ( uid != sb.st_uid )
        {
            seteuid(0);
            if (lchown(upath, sb.st_uid, sb.st_gid) < 0)
            {
                syslog (LOG_ERR,
                        "matchfile2dirperms: Error changing owner/gid of %s: %s",
                        upath, strerror(errno));
                return (AFPERR_ACCESS);
            }
            if (chmod(upath,(st.st_mode&0777&~default_options.umask)| S_IRGRP| S_IROTH) < 0)
            {
                syslog (LOG_ERR,
                        "matchfile2dirperms:  Error adding file read permissions: %s",
                        strerror(errno));
                return (AFPERR_ACCESS);
            }
#ifdef DEBUG
            else
                syslog (LOG_INFO,
                        "matchfile2dirperms:  Added S_IRGRP and S_IROTH: %s",
                        strerror(errno));
#endif /* DEBUG */
            if (lchown(adpath, sb.st_uid, sb.st_gid) < 0)
            {
                syslog (LOG_ERR,
                        "matchfile2dirperms: Error changing AppleDouble owner/gid %s: %s",
                        adpath, strerror(errno));
                return (AFPERR_ACCESS);
            }
            if (chmod(adpath, (st.st_mode&0777&~default_options.umask)| S_IRGRP| S_IROTH) < 0)
            {
                syslog (LOG_ERR,
                        "matchfile2dirperms:  Error adding AD file read permissions: %s",
                        strerror(errno));
                return (AFPERR_ACCESS);
            }
#ifdef DEBUG
            else
                syslog (LOG_INFO,
                        "matchfile2dirperms:  Added S_IRGRP and S_IROTH to AD: %s",
                        strerror(errno));
#endif /* DEBUG */
        }
#ifdef DEBUG
        else
            syslog (LOG_INFO,
                    "matchfile2dirperms: No ownership change necessary.");
#endif /* DEBUG */
    } /* end else if stat success */
    seteuid(uid); /* Restore process ownership to normal */
#ifdef DEBUG
    syslog (LOG_INFO, "end matchfile2dirperms:");
#endif /* DEBUG */

    return (AFP_OK);

}


int afp_getfildirparams(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct stat		st;
    struct vol		*vol;
    struct dir		*dir;
    u_int32_t           did;
    int			buflen, ret;
    char		*path;
    u_int16_t		fbitmap, dbitmap, vid;

#ifdef DEBUG
    syslog(LOG_INFO, "begin afp_getfildirparams:");
#endif /* DEBUG */

    *rbuflen = 0;
    ibuf += 2;

    memcpy( &vid, ibuf, sizeof( vid ));
    ibuf += sizeof( vid );
    if (( vol = getvolbyvid( vid )) == NULL ) {
        return( AFPERR_PARAM );
    }

    memcpy( &did, ibuf, sizeof( did ));
    ibuf += sizeof( did );

    if (( dir = dirsearch( vol, did )) == NULL ) {
        return( AFPERR_NOOBJ );
    }

    memcpy( &fbitmap, ibuf, sizeof( fbitmap ));
    fbitmap = ntohs( fbitmap );
    ibuf += sizeof( fbitmap );
    memcpy( &dbitmap, ibuf, sizeof( dbitmap ));
    dbitmap = ntohs( dbitmap );
    ibuf += sizeof( dbitmap );

    if (( path = cname( vol, dir, &ibuf )) == NULL) {
        return( AFPERR_NOOBJ );
    }

    if ( stat( mtoupath(vol, path ), &st ) < 0 ) {
        return( AFPERR_NOOBJ );
    }

    buflen = 0;
    if (S_ISDIR(st.st_mode)) {
        if (dbitmap) {
            ret = getdirparams(vol, dbitmap, ".", curdir,
                               &st, rbuf + 3 * sizeof( u_int16_t ), &buflen );
            if (ret != AFP_OK )
                return( ret );
        }
        /* this is a directory */
        *(rbuf + 2 * sizeof( u_int16_t )) = (char) FILDIRBIT_ISDIR;
    } else {
        if (fbitmap && ( ret = getfilparams(vol, fbitmap, path, curdir, &st,
                                            rbuf + 3 * sizeof( u_int16_t ), &buflen )) != AFP_OK ) {
            return( ret );
        }
        /* this is a file */
        *(rbuf + 2 * sizeof( u_int16_t )) = FILDIRBIT_ISFILE;
    }
    *rbuflen = buflen + 3 * sizeof( u_int16_t );
    fbitmap = htons( fbitmap );
    memcpy( rbuf, &fbitmap, sizeof( fbitmap ));
    rbuf += sizeof( fbitmap );
    dbitmap = htons( dbitmap );
    memcpy( rbuf, &dbitmap, sizeof( dbitmap ));
    rbuf += sizeof( dbitmap ) + sizeof( u_char );
    *rbuf = 0;

#ifdef DEBUG
    syslog(LOG_INFO, "end afp_getfildirparams:");
#endif /* DEBUG */

    return( AFP_OK );
}

int afp_setfildirparams(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct stat	st;
    struct vol	*vol;
    struct dir	*dir;
    char	*path;
    u_int16_t	vid, bitmap;
    int		did, rc;

#ifdef DEBUG
    syslog(LOG_INFO, "begin afp_setfildirparams:");
#endif /* DEBUG */

    *rbuflen = 0;
    ibuf += 2;
    memcpy( &vid, ibuf, sizeof(vid));
    ibuf += sizeof( vid );

    if (( vol = getvolbyvid( vid )) == NULL ) {
        return( AFPERR_PARAM );
    }

    if (vol->v_flags & AFPVOL_RO)
        return AFPERR_VLOCK;

    memcpy( &did, ibuf, sizeof( did));
    ibuf += sizeof( did);

    if (( dir = dirsearch( vol, did )) == NULL ) {
        return( AFPERR_NOOBJ );
    }

    memcpy( &bitmap, ibuf, sizeof( bitmap ));
    bitmap = ntohs( bitmap );
    ibuf += sizeof( bitmap );

    if (( path = cname( vol, dir, &ibuf )) == NULL ) {
        return( AFPERR_NOOBJ );
    }

    if ( stat( mtoupath(vol, path ), &st ) < 0 ) {
        return( AFPERR_NOOBJ );
    }

    /*
     * If ibuf is odd, make it even.
     */
    if ((u_long)ibuf & 1 ) {
        ibuf++;
    }

    if (S_ISDIR(st.st_mode)) {
        rc = setdirparams(vol, path, bitmap, ibuf );
    } else {
        rc = setfilparams(vol, path, bitmap, ibuf );
    }
    if ( rc == AFP_OK ) {
        setvoltime(obj, vol );
    }

#ifdef DEBUG
    syslog(LOG_INFO, "end afp_setfildirparams:");
#endif /* DEBUG */

    return( rc );
}

int afp_rename(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct adouble	ad;
    struct stat		st;
    struct vol		*vol;
    struct dir		*dir, *odir = NULL;
    char		*path, *buf, *upath, *newpath;
    char		*newadpath;
    u_int32_t		did;
    int			plen;
    u_int16_t		vid;
#ifdef CNID_DB
    cnid_t              id;
#endif /* CNID_DB */

#ifdef DEBUG
    syslog(LOG_INFO, "begin afp_rename:");
#endif /* DEBUG */

    *rbuflen = 0;
    ibuf += 2;

    memcpy( &vid, ibuf, sizeof( vid ));
    ibuf += sizeof( vid );
    if (( vol = getvolbyvid( vid )) == NULL ) {
        return( AFPERR_PARAM );
    }

    if (vol->v_flags & AFPVOL_RO)
        return AFPERR_VLOCK;

    memcpy( &did, ibuf, sizeof( did ));
    ibuf += sizeof( did );
    if (( dir = dirsearch( vol, did )) == NULL ) {
        return( AFPERR_NOOBJ );
    }

    if (( path = cname( vol, dir, &ibuf )) == NULL ) {
        return( AFPERR_NOOBJ );
    }

    /* another place where we know about the path type */
    if ( *ibuf++ != 2 ) {
        return( AFPERR_PARAM );
    }
    plen = (unsigned char) *ibuf++;
    *( ibuf + plen ) = '\0';

    if ( *path == '\0' ) {
        if ( curdir->d_parent == NULL ) { /* root directory */
            return( AFPERR_NORENAME );
        }
        odir = curdir;
        path = curdir->d_name;
        if ( movecwd( vol, curdir->d_parent ) < 0 ) {
            return( AFPERR_NOOBJ );
        }
    }

#ifdef notdef
    if ( strcasecmp( path, ibuf ) == 0 ) {
        return( AFP_OK );
    }
#endif /* notdef */

    /* if a curdir/newname ofork exists, return busy */
    if (of_findname(vol, curdir, ibuf))
        return AFPERR_BUSY;

    /* source == destination. just say okay. */
    if (strcmp(path, ibuf) == 0)
        return AFP_OK;

    /* check for illegal characters */
    if (!wincheck(vol, ibuf))
        return AFPERR_PARAM;

    newpath = obj->oldtmp;
    strcpy( newpath, mtoupath(vol, ibuf ));

    if ((vol->v_flags & AFPVOL_NOHEX) && strchr(newpath, '/'))
        return AFPERR_PARAM;

    if (!validupath(vol, newpath))
        return AFPERR_EXIST;

    /* check for vetoed filenames */
    if (veto_file(vol->v_veto, newpath))
        return AFPERR_EXIST;

    /* the strdiacasecmp deals with case-insensitive, case preserving
       filesystems */
    if (stat( newpath, &st ) == 0 && strdiacasecmp(path, ibuf))
        return( AFPERR_EXIST );

    upath = mtoupath(vol, path);

#ifdef CNID_DB
    id = cnid_get(vol->v_db, curdir->d_did, upath, strlen(upath));
#endif /* CNID_DB */

    if ( rename( upath, newpath ) < 0 ) {
        switch ( errno ) {
        case ENOENT :
            return( AFPERR_NOOBJ );
        case EACCES :
            return( AFPERR_ACCESS );
        default :
            return( AFPERR_PARAM );
        }
    }

#ifdef CNID_DB
    if (stat(newpath, &st) < 0) /* this shouldn't fail */
        return AFPERR_MISC;
    cnid_update(vol->v_db, id, &st, curdir->d_did, newpath, strlen(newpath));
#endif /* CNID_DB */

    if ( !odir ) {
        newadpath = obj->newtmp;
        strcpy( newadpath, ad_path( newpath, 0 ));
        if ( rename( ad_path( upath, 0 ), newadpath ) < 0 ) {
            if ( errno == ENOENT ) {	/* no adouble header file */
                if (( unlink( newadpath ) < 0 ) && ( errno != ENOENT )) {
                    return( AFPERR_PARAM );
                }
                goto out;
            }
            return( AFPERR_PARAM );
        }

        memset(&ad, 0, sizeof(ad));
        if ( ad_open( newpath, ADFLAGS_HF, O_RDWR|O_CREAT, 0666,
                      &ad) < 0 ) {
            return( AFPERR_PARAM );
        }
    } else {
        int isad = 1;

        memset(&ad, 0, sizeof(ad));
        if ( ad_open( newpath, vol_noadouble(vol)|ADFLAGS_HF|ADFLAGS_DIR,
                      O_RDWR|O_CREAT, 0666, &ad) < 0 ) {
            if (!((errno == ENOENT) && vol_noadouble(vol)))
                return( AFPERR_PARAM );
            isad = 0;
        }
        if ((buf = realloc( odir->d_name, plen + 1 )) == NULL ) {
            syslog( LOG_ERR, "afp_rename: realloc: %s", strerror(errno) );
            if (isad) {
                ad_flush(&ad, ADFLAGS_HF); /* in case of create */
                ad_close(&ad, ADFLAGS_HF);
            }
            return AFPERR_MISC;
        }
        odir->d_name = buf;
        strcpy( odir->d_name, ibuf );
        if (!isad)
            goto out;
    }

    ad_setentrylen( &ad, ADEID_NAME, plen );
    memcpy( ad_entry( &ad, ADEID_NAME ), ibuf, plen );
    ad_flush( &ad, ADFLAGS_HF );
    ad_close( &ad, ADFLAGS_HF );

out:
    setvoltime(obj, vol );

    /* if it's still open, rename the ofork as well. */
    if (of_rename(vol, curdir, path, curdir, ibuf) < 0)
        return AFPERR_MISC;

#ifdef DEBUG
    syslog(LOG_INFO, "end afp_rename:");
#endif /* DEBUG */

    return( AFP_OK );
}


int afp_delete(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct vol		*vol;
    struct dir		*dir;
    char		*path, *upath;
    int			did, rc;
    u_int16_t		vid;

#ifdef DEBUG
    syslog(LOG_INFO, "begin afp_delete:");
#endif /* DEBUG */ 

    *rbuflen = 0;
    ibuf += 2;

    memcpy( &vid, ibuf, sizeof( vid ));
    ibuf += sizeof( vid );
    if (( vol = getvolbyvid( vid )) == NULL ) {
        return( AFPERR_PARAM );
    }

    if (vol->v_flags & AFPVOL_RO)
        return AFPERR_VLOCK;

    memcpy( &did, ibuf, sizeof( did ));
    ibuf += sizeof( int );
    if (( dir = dirsearch( vol, did )) == NULL ) {
        return( AFPERR_NOOBJ );
    }

    if (( path = cname( vol, dir, &ibuf )) == NULL ) {
        return( AFPERR_NOOBJ );
    }

    if ( *path == '\0' ) {
        rc = deletecurdir( vol, obj->oldtmp, AFPOBJ_TMPSIZ);
    } else if (of_findname(vol, curdir, path)) {
        rc = AFPERR_BUSY;
    } else if ((rc = deletefile( upath = mtoupath(vol, path ))) == AFP_OK) {
#ifdef CNID_DB /* get rid of entry */
        cnid_t id = cnid_get(vol->v_db, curdir->d_did, upath, strlen(upath));
        cnid_delete(vol->v_db, id);
#endif /* CNID_DB */
    }
    if ( rc == AFP_OK ) {
        setvoltime(obj, vol );
    }

#ifdef DEBUG
    syslog(LOG_INFO, "end afp_delete:");
#endif /* DEBUG */

    return( rc );
}

char *ctoupath( vol, dir, name )
const struct vol	*vol;
struct dir	*dir;
char	*name;
{
    struct dir	*d;
    static char	path[ MAXPATHLEN + 1];
    char	*p, *u;
    int		len;

    p = path + sizeof( path ) - 1;
    *p = '\0';
    u = mtoupath(vol, name );
    len = strlen( u );
    p -= len;
    strncpy( p, u, len );
    for ( d = dir; d->d_parent; d = d->d_parent ) {
        *--p = '/';
        u = mtoupath(vol, d->d_name );
        len = strlen( u );
        p -= len;
        strncpy( p, u, len );
    }
    *--p = '/';
    len = strlen( vol->v_path );
    p -= len;
    strncpy( p, vol->v_path, len );

    return( p );
}


int afp_moveandrename(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct vol	*vol;
    struct dir	*sdir, *ddir, *odir = NULL;
    struct stat st;
    char	*oldname, *newname;
    char        *path, *p, *upath;
    int		did, rc;
    int		plen;
    u_int16_t	vid;
#ifdef CNID_DB
    cnid_t      id;
#endif /* CNID_DB */
#ifdef DROPKLUDGE
    int		retvalue;
#endif /* DROPKLUDGE */

#ifdef DEBUG
    syslog(LOG_INFO, "begin afp_moveandrename:");
#endif /* DEBUG */

    *rbuflen = 0;
    ibuf += 2;

    memcpy( &vid, ibuf, sizeof( vid ));
    ibuf += sizeof( vid );
    if (( vol = getvolbyvid( vid )) == NULL ) {
        return( AFPERR_PARAM );
    }

    if (vol->v_flags & AFPVOL_RO)
        return AFPERR_VLOCK;

    /* source did followed by dest did */
    memcpy( &did, ibuf, sizeof( did ));
    ibuf += sizeof( int );
    if (( sdir = dirsearch( vol, did )) == NULL ) {
        return( AFPERR_PARAM );
    }

    memcpy( &did, ibuf, sizeof( did ));
    ibuf += sizeof( int );

    /* source pathname */
    if (( path = cname( vol, sdir, &ibuf )) == NULL ) {
        return( AFPERR_NOOBJ );
    }

    sdir = curdir;
    newname = obj->newtmp;
    oldname = obj->oldtmp;
    if ( *path != '\0' ) {
        /* not a directory */
        strcpy(newname, path);
        strcpy(oldname, path); /* an extra copy for of_rename */
#ifdef CNID_DB
        p = mtoupath(vol, path);
        id = cnid_get(vol->v_db, sdir->d_did, p, strlen(p));
#endif /* CNID_DB */
        p = ctoupath( vol, sdir, newname );
    } else {
        odir = curdir;
        strcpy( newname, odir->d_name );
        strcpy(oldname, odir->d_name);
        p = ctoupath( vol, odir->d_parent, newname );
#ifdef CNID_DB
        id = curdir->d_did; /* we already have the CNID */
#endif /* CNID_DB */
    }
    /*
     * p now points to the full pathname of the source fs object.
     */

    /* get the destination directory */
    if (( ddir = dirsearch( vol, did )) == NULL ) {
        return( AFPERR_PARAM );
    }
    if (( path = cname( vol, ddir, &ibuf )) == NULL ) {
        return( AFPERR_NOOBJ );
    }
    if ( *path != '\0' ) {
        return( AFPERR_BADTYPE );
    }

    /* one more place where we know about path type */
    if ( *ibuf++ != 2 ) {
        return( AFPERR_PARAM );
    }

    if (( plen = (unsigned char)*ibuf++ ) != 0 ) {
        strncpy( newname, ibuf, plen );
        newname[ plen ] = '\0';
    }

    /* check for illegal characters */
    if (!wincheck(vol, ibuf))
        return AFPERR_PARAM;

    upath = mtoupath(vol, newname);

    if ((vol->v_flags & AFPVOL_NOHEX) && strchr(upath, '/'))
        return AFPERR_PARAM;

    if (!validupath(vol, upath))
        return AFPERR_EXIST;

    /* check for vetoed filenames */
    if (veto_file(vol->v_veto, upath))
        return AFPERR_EXIST;

    /* source == destination. we just silently accept this. */
    if (curdir == sdir) {
        if (strcmp(oldname, newname) == 0)
            return AFP_OK;

        /* deal with case insensitive, case-preserving filesystems. */
        if ((stat(upath, &st) == 0) && strdiacasecmp(oldname, newname))
            return AFPERR_EXIST;

    } else if (stat(upath, &st ) == 0)
        return( AFPERR_EXIST );

    if ( !odir ) {
        if (of_findname(vol, curdir, newname)) {
            rc = AFPERR_BUSY;
        } else if ((rc = renamefile( p, upath, newname,
                                     vol_noadouble(vol) )) == AFP_OK) {
            /* if it's still open, rename the ofork as well. */
            rc = of_rename(vol, sdir, oldname, curdir, newname);
        }
    } else {
        rc = renamedir(p, upath, odir, curdir, newname, vol_noadouble(vol));
    }

#ifdef DROPKLUDGE
    if (vol->v_flags & AFPVOL_DROPBOX) {
        if (retvalue=matchfile2dirperms (newname, vol, did) != AFP_OK) {
            return retvalue;
        }
    }
#endif /* DROPKLUDGE */

    if ( rc == AFP_OK ) {
#ifdef CNID_DB
        /* renaming may have moved the file/dir across a filesystem */
        if (stat(upath, &st) < 0)
            return AFPERR_MISC;

        /* fix up the catalog entry */
        cnid_update(vol->v_db, id, &st, curdir->d_did, upath, strlen(upath));
#endif /* CNID_DB */
        setvoltime(obj, vol );
    }

#ifdef DEBUG
    syslog(LOG_INFO, "end afp_moveandrename:");
#endif /* DEBUG */

    return( rc );
}

int veto_file(const char*veto_str, const char*path)
/* given a veto_str like "abc/zxc/" and path "abc", return 1
 * veto_str should be '/' delimited
 * if path matches any one of the veto_str elements exactly, then 1 is returned
 * otherwise, 0 is returned.
 */
{
    int i;	/* index to veto_str */
    int j;	/* index to path */

    if ((veto_str == NULL) || (path == NULL))
        return 0;
    /*
    #ifdef DEBUG
    	syslog(LOG_DEBUG, "veto_file \"%s\", \"%s\"", veto_str, path);
    #endif
    */
    for(i=0, j=0; veto_str[i] != '\0'; i++) {
        if (veto_str[i] == '/') {
            if ((j>0) && (path[j] == '\0'))
                return 1;
            j = 0;
        } else {
            if (veto_str[i] != path[j]) {
                while ((veto_str[i] != '/')
                        && (veto_str[i] != '\0'))
                    i++;
                j = 0;
                continue;
            }
            j++;
        }
    }
    return 0;
}

