/*
 * $Id: volume.c,v 1.51.2.4 2003-05-26 17:02:47 didg Exp $
 *
 * Copyright (c) 1990,1993 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <sys/time.h>
#include <atalk/logger.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netatalk/endian.h>
#include <atalk/asp.h>
#include <atalk/dsi.h>
#include <atalk/adouble.h>
#include <atalk/afp.h>
#include <atalk/util.h>
#ifdef CNID_DB
#include <atalk/cnid.h>
#endif /* CNID_DB*/
#include <dirent.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif /* HAVE_FCNTL_H */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

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

#include <pwd.h>
#include <grp.h>
#include <utime.h>
#include <errno.h>

#include "directory.h"
#include "file.h"
#include "volume.h"
#include "globals.h"
#include "unix.h"

extern int afprun(int root, char *cmd, int *outfd);

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif /* ! MIN */

#ifndef NO_LARGE_VOL_SUPPORT
#if BYTE_ORDER == BIG_ENDIAN
#define hton64(x)       (x)
#define ntoh64(x)       (x)
#else /* BYTE_ORDER == BIG_ENDIAN */
#define hton64(x)       ((u_int64_t) (htonl(((x) >> 32) & 0xffffffffLL)) | \
                         (u_int64_t) ((htonl(x) & 0xffffffffLL) << 32))
#define ntoh64(x)       (hton64(x))
#endif /* BYTE_ORDER == BIG_ENDIAN */
#endif /* ! NO_LARGE_VOL_SUPPORT */

static struct vol *Volumes = NULL;
static int		lastvid = 0;
#ifndef CNID_DB
static char		*Trash = "\02\024Network Trash Folder";
#endif /* CNID_DB */

static struct extmap	*Extmap = NULL, *Defextmap = NULL;
static int              Extmap_cnt;
static void             free_extmap(void);

#define VOLOPT_ALLOW      0  /* user allow list */
#define VOLOPT_DENY       1  /* user deny list */
#define VOLOPT_RWLIST     2  /* user rw list */
#define VOLOPT_ROLIST     3  /* user ro list */
#define VOLOPT_CODEPAGE   4  /* codepage */
#define VOLOPT_PASSWORD   5  /* volume password */
#define VOLOPT_CASEFOLD   6  /* character case mangling */
#define VOLOPT_FLAGS      7  /* various flags */
#define VOLOPT_DBPATH     8  /* path to database */
#define VOLOPT_MAPCHARS   9  /* does mtou and utom mappings. syntax:
m and u can be double-byte hex
strings if necessary.
m=u -> map both ways
  m>u -> map m to u
  m<u -> map u to m
  !u  -> make u illegal always
  ~u  -> make u illegal only as the first
  part of a double-byte character.
  */
#define VOLOPT_VETO          10  /* list of veto filespec */
#define VOLOPT_PREEXEC       11  /* preexec command */
#define VOLOPT_ROOTPREEXEC   12  /* root preexec command */

#define VOLOPT_POSTEXEC      13  /* postexec command */
#define VOLOPT_ROOTPOSTEXEC  14  /* root postexec command */
#ifdef FORCE_UIDGID
#warning UIDGID
#include "uid.h"

#define VOLOPT_FORCEUID  15  /* force uid for username x */
#define VOLOPT_FORCEGID  16  /* force gid for group x */
#define VOLOPT_UMASK     17
#else 
#define VOLOPT_UMASK     15
#endif /* FORCE_UIDGID */

#define VOLOPT_MAX       (VOLOPT_UMASK +1)

#define VOLOPT_NUM        (VOLOPT_MAX + 1)

#define VOLPASSLEN  8
#define VOLOPT_DEFAULT     ":DEFAULT:"
#define VOLOPT_DEFAULT_LEN 9
  struct vol_option {
      char *c_value;
      int i_value;
  };

static __inline__ void volfree(struct vol_option *options,
                               const struct vol_option *save)
{
    int i;

    if (save) {
        for (i = 0; i < VOLOPT_MAX; i++) {
            if (options[i].c_value && (options[i].c_value != save[i].c_value))
                free(options[i].c_value);
        }
    } else {
        for (i = 0; i < VOLOPT_MAX; i++) {
            if (options[i].c_value)
                free(options[i].c_value);
        }
    }
}


/* handle variable substitutions. here's what we understand:
 * $b   -> basename of path
 * $c   -> client ip/appletalk address
 * $d   -> volume pathname on server
 * $f   -> full name (whatever's in the gecos field)
 * $g   -> group
 * $h   -> hostname 
 * $s   -> server name (hostname if it doesn't exist)
 * $u   -> username (guest is usually nobody)
 * $v   -> volume name (ADEID_NAME or basename if ADEID_NAME is empty)
 * $z   -> zone (may not exist)
 * $$   -> $
 */
#define is_var(a, b) (strncmp((a), (b), 2) == 0)

static char *volxlate(AFPObj *obj, char *dest, size_t destlen,
                     char *src, struct passwd *pwd, char *path)
{
    char *p, *q;
    int len;
    char *ret;
    
    if (!src) {
        return NULL;
    }
    if (!dest) {
        dest = calloc(destlen +1, 1);
    }
    ret = dest;
    if (!ret) {
        return NULL;
    }
    strncpy(dest, src, destlen);
    if ((p = strchr(src, '$')) == NULL) /* nothing to do */
        return ret;

    /* first part of the path. just forward to the next variable. */
    len = MIN(p - src, destlen);
    if (len > 0) {
        destlen -= len;
        dest += len;
    }

    while (p && destlen > 0) {
        /* now figure out what the variable is */
        q = NULL;
        if (is_var(p, "$b")) {
            if (path) {
                if ((q = strrchr(path, '/')) == NULL)
                    q = path;
                else if (*(q + 1) != '\0')
                    q++;
            }
        } else if (is_var(p, "$c")) {
            if (obj->proto == AFPPROTO_ASP) {
                ASP asp = obj->handle;

                len = sprintf(dest, "%u.%u", ntohs(asp->asp_sat.sat_addr.s_net),
                              asp->asp_sat.sat_addr.s_node);
                dest += len;
                destlen -= len;

            } else if (obj->proto == AFPPROTO_DSI) {
                DSI *dsi = obj->handle;

                len = sprintf(dest, "%s:%u", inet_ntoa(dsi->client.sin_addr),
                              ntohs(dsi->client.sin_port));
                dest += len;
                destlen -= len;
            }
        } else if (is_var(p, "$d")) {
             q = path;
        } else if (is_var(p, "$f")) {
            if ((q = strchr(pwd->pw_gecos, ',')))
                *q = '\0';
            q = pwd->pw_gecos;
        } else if (is_var(p, "$g")) {
            struct group *grp = getgrgid(pwd->pw_gid);
            if (grp)
                q = grp->gr_name;
        } else if (is_var(p, "$h")) {
            q = obj->options.hostname;
        } else if (is_var(p, "$s")) {
            if (obj->Obj)
                q = obj->Obj;
            else if (obj->options.server) {
                q = obj->options.server;
            } else
                q = obj->options.hostname;
        } else if (is_var(p, "$u")) {
            q = obj->username;
        } else if (is_var(p, "$v")) {
            if (path) {
                struct adouble ad;

                memset(&ad, 0, sizeof(ad));
                if (ad_open(path, ADFLAGS_HF, O_RDONLY, 0, &ad) < 0)
                    goto no_volname;

                if ((len = MIN(ad_getentrylen(&ad, ADEID_NAME), destlen)) > 0) {
                    memcpy(dest, ad_entry(&ad, ADEID_NAME), len);
                    ad_close(&ad, ADFLAGS_HF);
                    dest += len;
                    destlen -= len;
                } else {
                    ad_close(&ad, ADFLAGS_HF);
no_volname: /* simple basename */
                    if ((q = strrchr(path, '/')) == NULL)
                        q = path;
                    else if (*(q + 1) != '\0')
                        q++;
                }
            }
        } else if (is_var(p, "$z")) {
            q = obj->Zone;
        } else if (is_var(p, "$$")) {
            q = "$";
        } else
            q = p;

        /* copy the stuff over. if we don't understand something that we
         * should, just skip it over. */
        if (q) {
            len = MIN(p == q ? 2 : strlen(q), destlen);
            strncpy(dest, q, len);
            dest += len;
            destlen -= len;
        }

        /* stuff up to next $ */
        src = p + 2;
        p = strchr(src, '$');
        len = p ? MIN(p - src, destlen) : destlen;
        if (len > 0) {
            strncpy(dest, src, len);
            dest += len;
            destlen -= len;
        }
    }
    return ret;
}

/* to make sure that val is valid, make sure to select an opt that
   includes val */
static int optionok(const char *buf, const char *opt, const char *val) 
{
    if (!strstr(buf,opt))
        return 0;
    if (!val[1])
        return 0;
    return 1;    
}

static __inline__ char *get_codepage_path(const char *path, const char *name)
{
    char *page;
    int len;

    if (path) {
        page = (char *) malloc((len = strlen(path)) + strlen(name) + 2);
        if (page) {
            strcpy(page, path);
            if (path[len - 1] != '/') /* add a / */
                strcat(page, "/");
            strcat(page, name);
        }
    } else {
        page = strdup(name);
    }

    /* debug: show which codepage directory we are using */
    LOG(log_debug, logtype_afpd, "using codepage directory: %s", page);

    return page;
}

/* -------------------- */
static void setoption(struct vol_option *options, struct vol_option *save, int opt, const char *val)
{
    if (options[opt].c_value && (!save || options[opt].c_value != save[opt].c_value))
        free(options[opt].c_value);
    options[opt].c_value = strdup(val + 1);
}

/* ------------------------------------------
   handle all the options. tmp can't be NULL. */
static void volset(struct vol_option *options, struct vol_option *save, 
                   char *volname, int vlen,
                   const char *nlspath, const char *tmp)
{
    char *val;

    val = strchr(tmp, ':');
    if (!val) {
        /* we'll assume it's a volume name. */
        strncpy(volname, tmp, vlen);
        volname[vlen] = 0;
        return;
    }

    LOG(log_debug, logtype_afpd, "Parsing volset %s", val);

    if (optionok(tmp, "allow:", val)) {
        setoption(options, save, VOLOPT_ALLOW, val);

    } else if (optionok(tmp, "deny:", val)) {
        setoption(options, save, VOLOPT_DENY, val);

    } else if (optionok(tmp, "rwlist:", val)) {
        setoption(options, save, VOLOPT_RWLIST, val);

    } else if (optionok(tmp, "rolist:", val)) {
        setoption(options, save, VOLOPT_ROLIST, val);

    } else if (optionok(tmp, "codepage:", val)) {
        if (options[VOLOPT_CODEPAGE].c_value && 
                (!save || options[VOLOPT_CODEPAGE].c_value != save[VOLOPT_CODEPAGE].c_value)) {
            free(options[VOLOPT_CODEPAGE].c_value);
        }
        options[VOLOPT_CODEPAGE].c_value = get_codepage_path(nlspath, val + 1);

    } else if (optionok(tmp, "veto:", val)) {
        setoption(options, save, VOLOPT_VETO, val);
    } else if (optionok(tmp, "casefold:", val)) {
        if (strcasecmp(val + 1, "tolower") == 0)
            options[VOLOPT_CASEFOLD].i_value = AFPVOL_UMLOWER;
        else if (strcasecmp(val + 1, "toupper") == 0)
            options[VOLOPT_CASEFOLD].i_value = AFPVOL_UMUPPER;
        else if (strcasecmp(val + 1, "xlatelower") == 0)
            options[VOLOPT_CASEFOLD].i_value = AFPVOL_UUPPERMLOWER;
        else if (strcasecmp(val + 1, "xlateupper") == 0)
            options[VOLOPT_CASEFOLD].i_value = AFPVOL_ULOWERMUPPER;

    } else if (optionok(tmp, "options:", val)) {
        char *p;

        if ((p = strtok(val + 1, ",")) == NULL) /* nothing */
            return;

        while (p) {
            if (strcasecmp(p, "prodos") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_A2VOL;
            else if (strcasecmp(p, "mswindows") == 0) {
                options[VOLOPT_FLAGS].i_value |= AFPVOL_MSWINDOWS;
                if (!options[VOLOPT_CODEPAGE].c_value)
                    options[VOLOPT_CODEPAGE].c_value =
                        get_codepage_path(nlspath, MSWINDOWS_CODEPAGE);

            } else if (strcasecmp(p, "crlf") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_CRLF;
            else if (strcasecmp(p, "noadouble") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_NOADOUBLE;
            else if (strcasecmp(p, "ro") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_RO;
            else if (strcasecmp(p, "nohex") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_NOHEX;
            else if (strcasecmp(p, "usedots") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_USEDOTS;
            else if (strcasecmp(p, "limitsize") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_LIMITSIZE;
            /* support for either "dropbox" or "dropkludge" */
            else if (strcasecmp(p, "dropbox") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_DROPBOX;
            else if (strcasecmp(p, "dropkludge") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_DROPBOX;
            else if (strcasecmp(p, "nofileid") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_NOFILEID;
            else if (strcasecmp(p, "utf8") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_UTF8;
            else if (strcasecmp(p, "nostat") == 0)
                options[VOLOPT_FLAGS].i_value |= AFPVOL_NOSTAT;
            else if (strcasecmp(p, "preexec_close") == 0)
		options[VOLOPT_PREEXEC].i_value = 1;
            else if (strcasecmp(p, "root_preexec_close") == 0)
		options[VOLOPT_ROOTPREEXEC].i_value = 1;
            p = strtok(NULL, ",");
        }

#ifdef CNID_DB

    } else if (optionok(tmp, "dbpath:", val)) {
        setoption(options, save, VOLOPT_DBPATH, val);
#endif /* CNID_DB */

    } else if (optionok(tmp, "umask:", val)) {
	options[VOLOPT_UMASK].i_value = (int)strtol(val, (char **)NULL, 8);
    } else if (optionok(tmp, "mapchars:",val)) {
        setoption(options, save, VOLOPT_MAPCHARS, val);

    } else if (optionok(tmp, "password:", val)) {
        setoption(options, save, VOLOPT_PASSWORD, val);

#ifdef FORCE_UIDGID

        /* this code allows forced uid/gid per volume settings */
    } else if (optionok(tmp, "forceuid:", val)) {
        setoption(options, save, VOLOPT_FORCEUID, val);
    } else if (optionok(tmp, "forcegid:", val)) {
        setoption(options, save, VOLOPT_FORCEGID, val);

#endif /* FORCE_UIDGID */
    } else if (optionok(tmp, "root_preexec:", val)) {
        setoption(options, save, VOLOPT_ROOTPREEXEC, val);

    } else if (optionok(tmp, "preexec:", val)) {
        setoption(options, save, VOLOPT_PREEXEC, val);

    } else if (optionok(tmp, "root_postexec:", val)) {
        setoption(options, save, VOLOPT_ROOTPOSTEXEC, val);

    } else if (optionok(tmp, "postexec:", val)) {
        setoption(options, save, VOLOPT_POSTEXEC, val);

    } else {
        /* ignore unknown options */
        LOG(log_debug, logtype_afpd, "ignoring unknown volume option: %s", tmp);

    } 
}

/* ----------------- */
static void showvol(const char *name)
{
    struct vol	*volume;
    for ( volume = Volumes; volume; volume = volume->v_next ) {
        if ( !strcasecmp( volume->v_name, name ) && volume->v_hide) {
            volume->v_hide = 0;
            return;
        }
    }
}

/* ------------------------------- */
static int creatvol(AFPObj *obj, struct passwd *pwd, 
                    char *path, char *name, 
                    struct vol_option *options, 
                    const int user /* user defined volume */
                    )
{
    struct vol	*volume;
    int		vlen;
    int         hide = 0;

    if ( name == NULL || *name == '\0' ) {
        if ((name = strrchr( path, '/' )) == NULL) {
            return -1;	/* Obviously not a fully qualified path */
        }

        /* if you wish to share /, you need to specify a name. */
        if (*++name == '\0')
            return -1;
    }

    for ( volume = Volumes; volume; volume = volume->v_next ) {
        if ( strcasecmp( volume->v_name, name ) == 0 ) {
           if (volume->v_deleted) {
               hide = 1;
           }
           else {
               return -1;	/* Won't be able to access it, anyway... */
           }
        }
    }

    vlen = strlen( name );
    if ( vlen > AFPVOL_NAMELEN ) {
        vlen = AFPVOL_NAMELEN;
        name[AFPVOL_NAMELEN] = '\0';
    }

    if (!( volume = (struct vol *)calloc(1, sizeof( struct vol ))) ) {
        LOG(log_error, logtype_afpd, "creatvol: malloc: %s", strerror(errno) );
        return -1;
    }
    if (! ( volume->v_name = (char *)malloc( vlen + 1 ) ) ) {
        LOG(log_error, logtype_afpd, "creatvol: malloc: %s", strerror(errno) );
        free(volume);
        return -1;
    }
    if (!( volume->v_path = (char *)malloc( strlen( path ) + 1 )) ) {
        LOG(log_error, logtype_afpd, "creatvol: malloc: %s", strerror(errno) );
        free(volume->v_name);
        free(volume);
        return -1;
    }
    volume->v_hide = hide;
    strcpy( volume->v_name, name);
    strcpy( volume->v_path, path );

#ifdef __svr4__
    volume->v_qfd = -1;
#endif /* __svr4__ */
    volume->v_vid = lastvid++;
    volume->v_lastdid = 17;

    /* handle options */
    if (options) {
        /* should we casefold? */
        volume->v_casefold = options[VOLOPT_CASEFOLD].i_value;

        /* shift in some flags */
        volume->v_flags = options[VOLOPT_FLAGS].i_value;

        /* read in the code pages */
        if (options[VOLOPT_CODEPAGE].c_value)
            codepage_read(volume, options[VOLOPT_CODEPAGE].c_value);

        if (options[VOLOPT_PASSWORD].c_value)
            volume->v_password = strdup(options[VOLOPT_PASSWORD].c_value);

        if (options[VOLOPT_VETO].c_value)
            volume->v_veto = strdup(options[VOLOPT_VETO].c_value);

#ifdef CNID_DB
        if (options[VOLOPT_DBPATH].c_value)
            volume->v_dbpath = volxlate(obj, NULL, MAXPATHLEN, options[VOLOPT_DBPATH].c_value, pwd, path);
#endif

	if (options[VOLOPT_UMASK].i_value)
	    volume->v_umask = (mode_t)options[VOLOPT_UMASK].i_value;

#ifdef FORCE_UIDGID
        if (options[VOLOPT_FORCEUID].c_value) {
            volume->v_forceuid = strdup(options[VOLOPT_FORCEUID].c_value);
        } else {
            volume->v_forceuid = NULL; /* set as null so as to return 0 later on */
        }

        if (options[VOLOPT_FORCEGID].c_value) {
            volume->v_forcegid = strdup(options[VOLOPT_FORCEGID].c_value);
        } else {
            volume->v_forcegid = NULL; /* set as null so as to return 0 later on */
        }
#endif
        if (!user) {
            if (options[VOLOPT_PREEXEC].c_value)
                volume->v_preexec = volxlate(obj, NULL, MAXPATHLEN, options[VOLOPT_PREEXEC].c_value, pwd, path);
            volume->v_preexec_close = options[VOLOPT_PREEXEC].i_value;

            if (options[VOLOPT_POSTEXEC].c_value)
                volume->v_postexec = volxlate(obj, NULL, MAXPATHLEN, options[VOLOPT_POSTEXEC].c_value, pwd, path);

            if (options[VOLOPT_ROOTPREEXEC].c_value)
                volume->v_root_preexec = volxlate(obj, NULL, MAXPATHLEN, options[VOLOPT_ROOTPREEXEC].c_value, pwd, path);
            volume->v_root_preexec_close = options[VOLOPT_ROOTPREEXEC].i_value;

            if (options[VOLOPT_ROOTPOSTEXEC].c_value)
                volume->v_root_postexec = volxlate(obj, NULL, MAXPATHLEN, options[VOLOPT_ROOTPOSTEXEC].c_value, pwd, path);
        }
    }

    volume->v_next = Volumes;
    Volumes = volume;
    return 0;
}

/* ---------------- */
static char *myfgets( buf, size, fp )
char	*buf;
int		size;
FILE	*fp;
{
    char	*p;
    int		c;

    p = buf;
    while ((EOF != ( c = getc( fp )) ) && ( size > 0 )) {
        if ( c == '\n' || c == '\r' ) {
            *p++ = '\n';
            break;
        } else {
            *p++ = c;
        }
        size--;
    }

    if ( p == buf ) {
        return( NULL );
    } else {
        *p = '\0';
        return( buf );
    }
}


/* check access list. this function wants something of the following
 * form:
 *        @group,name,name2,@group2,name3
 *
 * a NULL argument allows everybody to have access.
 * we return three things:
 *     -1: no list
 *      0: list exists, but name isn't in it
 *      1: in list
 */
static int accessvol(args, name)
const char *args;
const char *name;
{
    char buf[MAXPATHLEN + 1], *p;
    struct group *gr;

    if (!args)
        return -1;

    strncpy(buf, args, sizeof(buf));
    if ((p = strtok(buf, ",")) == NULL) /* nothing, return okay */
        return -1;

    while (p) {
        if (*p == '@') { /* it's a group */
            if ((gr = getgrnam(p + 1)) && gmem(gr->gr_gid))
                return 1;
        } else if (strcmp(p, name) == 0) /* it's a user name */
            return 1;
        p = strtok(NULL, ",");
    }

    return 0;
}

static void setextmap( ext, type, creator, user)
char		*ext, *type, *creator;
int			user;
{
    struct extmap	*em;
    int                 cnt;

    if (Extmap == NULL) {
        if (( Extmap = calloc(1, sizeof( struct extmap ))) == NULL ) {
            LOG(log_error, logtype_afpd, "setextmap: calloc: %s", strerror(errno) );
            return;
        }
    }
    ext++;
    for ( em = Extmap, cnt = 0; em->em_ext; em++, cnt++) {
        if ( (strdiacasecmp( em->em_ext, ext )) == 0 ) {
            break;
        }
    }

    if ( em->em_ext == NULL ) {
        if (!(Extmap  = realloc( Extmap, sizeof( struct extmap ) * (cnt +2))) ) {
            LOG(log_error, logtype_afpd, "setextmap: realloc: %s", strerror(errno) );
            return;
        }
        (Extmap +cnt +1)->em_ext = NULL;
        em = Extmap +cnt;
    } else if ( !user ) {
        return;
    }
    if (em->em_ext)
    	free(em->em_ext);

    if (!(em->em_ext = strdup(  ext))) {
        LOG(log_error, logtype_afpd, "setextmap: strdup: %s", strerror(errno) );
        return;
    }

    if ( *type == '\0' ) {
        memcpy(em->em_type, "????", sizeof( em->em_type ));
    } else {
        memcpy(em->em_type, type, sizeof( em->em_type ));
    }
    if ( *creator == '\0' ) {
        memcpy(em->em_creator, "UNIX", sizeof( em->em_creator ));
    } else {
        memcpy(em->em_creator, creator, sizeof( em->em_creator ));
    }
}

/* -------------------------- */
static int extmap_cmp(const void *map1, const void *map2)
{
    const struct extmap *em1 = map1;
    const struct extmap *em2 = map2;
    return strdiacasecmp(em1->em_ext, em2->em_ext);
}

static void sortextmap( void)
{
    struct extmap	*em;

    Extmap_cnt = 0;
    if ((em = Extmap) == NULL) {
        return;
    }
    while (em->em_ext) {
        em++;
        Extmap_cnt++;
    }
    if (Extmap_cnt) {
        qsort(Extmap, Extmap_cnt, sizeof(struct extmap), extmap_cmp);
        Defextmap = Extmap;
    }
}

/* ----------------------
*/
static void free_extmap( void)
{
    if (Extmap) {
        free(Extmap);
        Extmap = NULL;
        Defextmap = Extmap;
        Extmap_cnt = 0;
    }
}

/* ----------------------
*/
static int volfile_changed(struct afp_volume_name *p) 
{
    struct stat      st;
    char *name;
    
    if (p->full_name) 
    	name = p->full_name;
    else
        name = p->name;
        
    if (!stat( name, &st) && st.st_mtime > p->mtime) {
        p->mtime = st.st_mtime;
        return 1;
    }
    return 0;
}

/* ----------------------
 * Read a volume configuration file and add the volumes contained within to
 * the global volume list.  If p2 is non-NULL, the file that is opened is
 * p1/p2
 * 
 * Lines that begin with # and blank lines are ignored.
 * Volume lines are of the form:
 *		<unix path> [<volume name>] [allow:<user>,<@group>,...] \
 *                           [codepage:<file>] [casefold:<num>]
 *		<extension> TYPE [CREATOR]
 */
static int readvolfile(obj, p1, p2, user, pwent)
AFPObj      *obj;
struct afp_volume_name 	*p1;
char        *p2;
int		user;
struct passwd *pwent;
{
    FILE		*fp;
    char		path[ MAXPATHLEN + 1], tmp[ MAXPATHLEN + 1],
    volname[ AFPVOL_NAMELEN + 1 ], buf[ BUFSIZ ],
    type[ 5 ], creator[ 5 ];
    char		*u, *p;
    struct passwd	*pw;
    struct vol_option   options[VOLOPT_NUM], save_options[VOLOPT_NUM];
    int                 i;
    struct stat         st;
    int                 fd;

    if (!p1->name)
        return -1;
    p1->mtime = 0;
    strcpy( path, p1->name );
    if ( p2 != NULL ) {
        strcat( path, "/" );
        strcat( path, p2 );
        if (p1->full_name) {
            free(p1->full_name);
        }
        p1->full_name = strdup(path);
    }

    if (NULL == ( fp = fopen( path, "r" )) ) {
        return( -1 );
    }
    fd = fileno(fp);
    if (fd != -1 && !fstat( fd, &st) ) {
        p1->mtime = st.st_mtime;
    }

    memset(save_options, 0, sizeof(save_options));
    while ( myfgets( buf, sizeof( buf ), fp ) != NULL ) {
        initline( strlen( buf ), buf );
        parseline( sizeof( path ) - 1, path );
        switch ( *path ) {
        case '\0' :
        case '#' :
            continue;

        case ':':
            /* change the default options for this file */
            if (strncmp(path, VOLOPT_DEFAULT, VOLOPT_DEFAULT_LEN) == 0) {
                *tmp = '\0';
                for (i = 0; i < VOLOPT_NUM; i++) {
                    if (parseline( sizeof( path ) - VOLOPT_DEFAULT_LEN - 1,
                                   path + VOLOPT_DEFAULT_LEN) < 0)
                        break;
                    volset(save_options, NULL, tmp, sizeof(tmp) - 1,
                           obj->options.nlspath, path + VOLOPT_DEFAULT_LEN);
                }
            }
            break;

        case '~' :
            if (( p = strchr( path, '/' )) != NULL ) {
                *p++ = '\0';
            }
            u = path;
            u++;
            if ( *u == '\0' ) {
                u = obj->username;
            }
            if ( u == NULL || *u == '\0' || ( pw = getpwnam( u )) == NULL ) {
                continue;
            }
            strcpy( tmp, pw->pw_dir );
            if ( p != NULL && *p != '\0' ) {
                strcat( tmp, "/" );
                strcat( tmp, p );
            }
	    /* Tag a user's home directory with their umask.  Note, this will
	     * be overwritten if the user actually specifies a umask: option
	     * for a '~' volume. */
	    save_options[VOLOPT_UMASK].i_value = obj->options.save_mask;
            /* fall through */

        case '/' :
            /* send path through variable substitution */
            if (*path != '~') /* need to copy path to tmp */
                strcpy(tmp, path);
            if (!pwent)
                pwent = getpwnam(obj->username);
            volxlate(obj, path, sizeof(path) - 1, tmp, pwent, NULL);

            /* this is sort of braindead. basically, i want to be
             * able to specify things in any order, but i don't want to 
             * re-write everything. 
             *
             * currently we have options: 
             *   volname
             *   codepage:x
             *   casefold:x
             *   allow:x,y,@z
             *   deny:x,y,@z
             *   rwlist:x,y,@z
             *   rolist:x,y,@z
             *   options:prodos,crlf,noadouble,ro...
             *   dbpath:x
             *   password:x
             *   preexec:x
             *
             *   namemask:x,y,!z  (not implemented yet)
             */
            memcpy(options, save_options, sizeof(options));
            *volname = '\0';

            /* read in up to VOLOP_NUM possible options */
            for (i = 0; i < VOLOPT_NUM; i++) {
                if (parseline( sizeof( tmp ) - 1, tmp ) < 0)
                    break;

                volset(options, save_options, volname, sizeof(volname) - 1,obj->options.nlspath, tmp);
            }

            /* check allow/deny lists:
               allow -> either no list (-1), or in list (1)
               deny -> either no list (-1), or not in list (0) */
            if (accessvol(options[VOLOPT_ALLOW].c_value, obj->username) &&
                    (accessvol(options[VOLOPT_DENY].c_value, obj->username) < 1)) {

                /* handle read-only behaviour. semantics:
                 * 1) neither the rolist nor the rwlist exist -> rw
                 * 2) rolist exists -> ro if user is in it.
                 * 3) rwlist exists -> ro unless user is in it. */
                if (((options[VOLOPT_FLAGS].i_value & AFPVOL_RO) == 0) &&
                        ((accessvol(options[VOLOPT_ROLIST].c_value,
                                    obj->username) == 1) ||
                         !accessvol(options[VOLOPT_RWLIST].c_value,
                                    obj->username)))
                    options[VOLOPT_FLAGS].i_value |= AFPVOL_RO;

                /* do variable substitution for volname */
                volxlate(obj, tmp, sizeof(tmp) - 1, volname, pwent, path);
                creatvol(obj, pwent, path, tmp, options, p2 != NULL);
            }
            volfree(options, save_options);
            break;

        case '.' :
            parseline( sizeof( type ) - 1, type );
            parseline( sizeof( creator ) - 1, creator );
            setextmap( path, type, creator, user);
            break;

        default :
            break;
        }
    }
    volfree(save_options, NULL);
    sortextmap();
    if ( fclose( fp ) != 0 ) {
        LOG(log_error, logtype_afpd, "readvolfile: fclose: %s", strerror(errno) );
    }
    p1->loaded = 1;
    return( 0 );
}

/* ------------------------------- */
static void volume_free(struct vol *vol)
{
    free(vol->v_name);
    vol->v_name = NULL;
    free(vol->v_path);
    codepage_free(vol);
    free(vol->v_password);
    free(vol->v_veto);
#ifdef CNID_DB
    free(vol->v_dbpath);
#endif /* CNID_DB */
#ifdef FORCE_UIDGID
    free(vol->v_forceuid);
    free(vol->v_forcegid);
#endif /* FORCE_UIDGID */
}

/* ------------------------------- */
static void free_volumes(void )
{
    struct vol	*vol;
    struct vol  *nvol, *ovol;

    for ( vol = Volumes; vol; vol = vol->v_next ) {
        if (( vol->v_flags & AFPVOL_OPEN ) ) {
            vol->v_deleted = 1;
            continue;
        }
        volume_free(vol);
    }

    for ( vol = Volumes, ovol = NULL; vol; vol = nvol) {
        nvol = vol->v_next;

        if (vol->v_name == NULL) {
           if (Volumes == vol) {
               Volumes = nvol;
           }
           if (!ovol) {
               ovol = Volumes;
           }
           else {
              ovol->v_next = nvol;
           }
           free(vol);
        }
        else {
           ovol = vol;
        }
    }
}

/* ------------------------------- */
static void volume_unlink(struct vol *volume)
{
struct vol *vol, *ovol, *nvol;

    if (volume == Volumes) {
        Volumes = Volumes->v_next;
        return;
    }
    for ( vol = Volumes, ovol = NULL; vol; vol = nvol) {
        nvol = vol->v_next;

        if (vol == volume) {
           if (!ovol) {
               ovol = Volumes;
           }
           else {
              ovol->v_next = nvol;
           }
           break;
        }
        else {
           ovol = vol;
        }
    }
}


static int getvolspace( vol, bfree, btotal, xbfree, xbtotal, bsize )
struct vol	*vol;
u_int32_t	*bfree, *btotal, *bsize;
VolSpace    *xbfree, *xbtotal;
{
    int	        spaceflag, rc;
    u_int32_t   maxsize;
#ifndef NO_QUOTA_SUPPORT
    VolSpace	qfree, qtotal;
#endif

    spaceflag = AFPVOL_GVSMASK & vol->v_flags;
    /* report up to 2GB if afp version is < 2.2 (4GB if not) */
    maxsize = (vol->v_flags & AFPVOL_A2VOL) ? 0x01fffe00 :
              (((afp_version < 22) || (vol->v_flags & AFPVOL_LIMITSIZE))
               ? 0x7fffffffL : 0xffffffffL);

#ifdef AFS
    if ( spaceflag == AFPVOL_NONE || spaceflag == AFPVOL_AFSGVS ) {
        if ( afs_getvolspace( vol, xbfree, xbtotal, bsize ) == AFP_OK ) {
            vol->v_flags = ( ~AFPVOL_GVSMASK & vol->v_flags ) | AFPVOL_AFSGVS;
            goto getvolspace_done;
        }
    }
#endif

    if (( rc = ustatfs_getvolspace( vol, xbfree, xbtotal,
                                    bsize)) != AFP_OK ) {
        return( rc );
    }

#define min(a,b)	((a)<(b)?(a):(b))
#ifndef NO_QUOTA_SUPPORT
    if ( spaceflag == AFPVOL_NONE || spaceflag == AFPVOL_UQUOTA ) {
        if ( uquota_getvolspace( vol, &qfree, &qtotal, *bsize ) == AFP_OK ) {
            vol->v_flags = ( ~AFPVOL_GVSMASK & vol->v_flags ) | AFPVOL_UQUOTA;
            *xbfree = min(*xbfree, qfree);
            *xbtotal = min( *xbtotal, qtotal);
            goto getvolspace_done;
        }
    }
#endif
    vol->v_flags = ( ~AFPVOL_GVSMASK & vol->v_flags ) | AFPVOL_USTATFS;

getvolspace_done:
    *bfree = min( *xbfree, maxsize);
    *btotal = min( *xbtotal, maxsize);
    return( AFP_OK );
}

static int getvolparams( bitmap, vol, st, buf, buflen )
u_int16_t	bitmap;
struct vol	*vol;
struct stat	*st;
char	*buf;
int		*buflen;
{
    struct adouble	ad;
    int			bit = 0, isad = 1;
    u_int32_t		aint;
    u_short		ashort;
    u_int32_t		bfree, btotal, bsize;
    VolSpace            xbfree, xbtotal; /* extended bytes */
    char		*data, *nameoff = NULL;
    char                *slash;

    /* courtesy of jallison@whistle.com:
     * For MacOS8.x support we need to create the
     * .Parent file here if it doesn't exist. */

    memset(&ad, 0, sizeof(ad));
    if ( ad_open( vol->v_path, vol_noadouble(vol) |
                  ADFLAGS_HF|ADFLAGS_DIR, O_RDWR | O_CREAT,
                  0666, &ad) < 0 ) {
        isad = 0;

    } else if (ad_get_HF_flags( &ad ) & O_CREAT) {
        slash = strrchr( vol->v_path, '/' );
        if(slash)
            slash++;
        else
            slash = vol->v_path;

        ad_setentrylen( &ad, ADEID_NAME, strlen( slash ));
        memcpy(ad_entry( &ad, ADEID_NAME ), slash,
               ad_getentrylen( &ad, ADEID_NAME ));
	ad_setdate(&ad, AD_DATE_CREATE | AD_DATE_UNIX, st->st_mtime);
        ad_flush(&ad, ADFLAGS_HF);
    }

    if (( bitmap & ( (1<<VOLPBIT_BFREE)|(1<<VOLPBIT_BTOTAL) |
                     (1<<VOLPBIT_XBFREE)|(1<<VOLPBIT_XBTOTAL) |
                     (1<<VOLPBIT_BSIZE)) ) != 0 ) {
        if ( getvolspace( vol, &bfree, &btotal, &xbfree, &xbtotal,
                          &bsize) < 0 ) {
            if ( isad ) {
                ad_close( &ad, ADFLAGS_HF );
            }
            return( AFPERR_PARAM );
        }
    }

    data = buf;
    while ( bitmap != 0 ) {
        while (( bitmap & 1 ) == 0 ) {
            bitmap = bitmap>>1;
            bit++;
        }

        switch ( bit ) {
        case VOLPBIT_ATTR :
            ashort = 0;
#ifdef CNID_DB
            if (0 == (vol->v_flags & AFPVOL_NOFILEID) && vol->v_db != NULL) {
                ashort = VOLPBIT_ATTR_FILEID;
            }
#endif /* CNID_DB */
            /* check for read-only.
             * NOTE: we don't actually set the read-only flag unless
             *       it's passed in that way as it's possible to mount
             *       a read-write filesystem under a read-only one. */
            if ((vol->v_flags & AFPVOL_RO) ||
                    ((utime(vol->v_path, NULL) < 0) && (errno == EROFS))) {
                ashort |= VOLPBIT_ATTR_RO;
            }
            ashort |= VOLPBIT_ATTR_CATSEARCH;
            if (afp_version >= 30) {
                ashort |= VOLPBIT_ATTR_UTF8;
            }
            ashort = htons(ashort);
            memcpy(data, &ashort, sizeof( ashort ));
            data += sizeof( ashort );
            break;

        case VOLPBIT_SIG :
            ashort = htons( AFPVOLSIG_DEFAULT );
            memcpy(data, &ashort, sizeof( ashort ));
            data += sizeof( ashort );
            break;

        case VOLPBIT_CDATE :
            if (!isad || (ad_getdate(&ad, AD_DATE_CREATE, &aint) < 0))
                aint = AD_DATE_FROM_UNIX(st->st_mtime);
            memcpy(data, &aint, sizeof( aint ));
            data += sizeof( aint );
            break;

        case VOLPBIT_MDATE :
            if ( st->st_mtime > vol->v_time ) {
                vol->v_time = st->st_mtime;
                aint = AD_DATE_FROM_UNIX(st->st_mtime);
            } else {
                aint = AD_DATE_FROM_UNIX(vol->v_time);
            }
            memcpy(data, &aint, sizeof( aint ));
            data += sizeof( aint );
            break;

        case VOLPBIT_BDATE :
            if (!isad ||  (ad_getdate(&ad, AD_DATE_BACKUP, &aint) < 0))
                aint = AD_DATE_START;
            memcpy(data, &aint, sizeof( aint ));
            data += sizeof( aint );
            break;

        case VOLPBIT_VID :
            memcpy(data, &vol->v_vid, sizeof( vol->v_vid ));
            data += sizeof( vol->v_vid );
            break;

        case VOLPBIT_BFREE :
            bfree = htonl( bfree );
            memcpy(data, &bfree, sizeof( bfree ));
            data += sizeof( bfree );
            break;

        case VOLPBIT_BTOTAL :
            btotal = htonl( btotal );
            memcpy(data, &btotal, sizeof( btotal ));
            data += sizeof( btotal );
            break;

#ifndef NO_LARGE_VOL_SUPPORT
        case VOLPBIT_XBFREE :
            xbfree = hton64( xbfree );
#if defined(__GNUC__) && defined(HAVE_GCC_MEMCPY_BUG)
            bcopy(&xbfree, data, sizeof(xbfree));
#else /* __GNUC__ && HAVE_GCC_MEMCPY_BUG */
            memcpy(data, &xbfree, sizeof( xbfree ));
#endif /* __GNUC__ && HAVE_GCC_MEMCPY_BUG */
            data += sizeof( xbfree );
            break;

        case VOLPBIT_XBTOTAL :
            xbtotal = hton64( xbtotal );
#if defined(__GNUC__) && defined(HAVE_GCC_MEMCPY_BUG)
            bcopy(&xbtotal, data, sizeof(xbtotal));
#else /* __GNUC__ && HAVE_GCC_MEMCPY_BUG */
            memcpy(data, &xbtotal, sizeof( xbtotal ));
#endif /* __GNUC__ && HAVE_GCC_MEMCPY_BUG */
            data += sizeof( xbfree );
            break;
#endif /* ! NO_LARGE_VOL_SUPPORT */

        case VOLPBIT_NAME :
            nameoff = data;
            data += sizeof( u_int16_t );
            break;

        case VOLPBIT_BSIZE:  /* block size */
            bsize = htonl(bsize);
            memcpy(data, &bsize, sizeof(bsize));
            data += sizeof(bsize);
            break;

        default :
            if ( isad ) {
                ad_close( &ad, ADFLAGS_HF );
            }
            return( AFPERR_BITMAP );
        }
        bitmap = bitmap>>1;
        bit++;
    }
    if ( nameoff ) {
        ashort = htons( data - buf );
        memcpy(nameoff, &ashort, sizeof( ashort ));
        aint = strlen( vol->v_name );
        *data++ = aint;
        memcpy(data, vol->v_name, aint );
        data += aint;
    }
    if ( isad ) {
        ad_close( &ad, ADFLAGS_HF );
    }
    *buflen = data - buf;
    return( AFP_OK );
}

/* ------------------------------- */
void load_volumes(AFPObj *obj)
{
    struct passwd	*pwent;

    if (Volumes) {
        int changed = 0;
        
        /* check files date */
        if (obj->options.defaultvol.loaded) {
            changed = volfile_changed(&obj->options.defaultvol);
        }
        if (obj->options.systemvol.loaded) {
            changed |= volfile_changed(&obj->options.systemvol);
        }
        if (obj->options.uservol.loaded) {
            changed |= volfile_changed(&obj->options.uservol);
        }
        if (!changed)
            return;
        
        free_extmap();
        free_volumes();
    }
    
    pwent = getpwnam(obj->username);
    if ( (obj->options.flags & OPTION_USERVOLFIRST) == 0 ) {
        readvolfile(obj, &obj->options.systemvol, NULL, 0, pwent);
    }

    if ((*obj->username == '\0') || (obj->options.flags & OPTION_NOUSERVOL)) {
        readvolfile(obj, &obj->options.defaultvol, NULL, 1, pwent);
    } else if (pwent) {
        /*
        * Read user's AppleVolumes or .AppleVolumes file
        * If neither are readable, read the default volumes file. if
        * that doesn't work, create a user share.
        */
        obj->options.uservol.name = strdup(pwent->pw_dir);
        if ( readvolfile(obj, &obj->options.uservol,    "AppleVolumes", 1, pwent) < 0 &&
                readvolfile(obj, &obj->options.uservol, ".AppleVolumes", 1, pwent) < 0 &&
                readvolfile(obj, &obj->options.uservol, "applevolumes", 1, pwent) < 0 &&
                readvolfile(obj, &obj->options.uservol, ".applevolumes", 1, pwent) < 0 &&
                obj->options.defaultvol.name != NULL ) {
            if (readvolfile(obj, &obj->options.defaultvol, NULL, 1, pwent) < 0)
                creatvol(obj, pwent, pwent->pw_dir, NULL, NULL, 1);
        }
    }
    if ( obj->options.flags & OPTION_USERVOLFIRST ) {
        readvolfile(obj, &obj->options.systemvol, NULL, 0, pwent );
    }
}

/* ------------------------------- */
int afp_getsrvrparms(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int 	ibuflen, *rbuflen;
{
    struct timeval	tv;
    struct stat		st;
    struct vol		*volume;
    char	*data;
    int			vcnt, len;


    load_volumes(obj);

    data = rbuf + 5;
    for ( vcnt = 0, volume = Volumes; volume; volume = volume->v_next ) {
        if (!(volume->v_flags & AFPVOL_NOSTAT)) {
            if ( stat( volume->v_path, &st ) < 0 ) {
                LOG(log_info, logtype_afpd, "afp_getsrvrparms: stat %s: %s",
                    volume->v_path, strerror(errno) );
                continue;		/* can't access directory */
            }
            if (!S_ISDIR(st.st_mode)) {
                continue;		/* not a dir */
            }
        }
        if (volume->v_hide) {
            continue;		/* config file changed but the volume was mounted */
        
        }
        /* set password bit if there's a volume password */
        *data = (volume->v_password) ? AFPSRVR_PASSWD : 0;

        /* Apple 2 clients running ProDOS-8 expect one volume to have
           bit 0 of this byte set.  They will not recognize anything
           on the server unless this is the case.  I have not
           completely worked this out, but it's related to booting
           from the server.  Support for that function is a ways
           off.. <shirsch@ibm.net> */
        *data++ |= (volume->v_flags & AFPVOL_A2VOL) ? AFPSRVR_CONFIGINFO : 0;
        len = strlen( volume->v_name );
        *data++ = len;
        memcpy(data, volume->v_name, len );
        data += len;
        vcnt++;
    }

    *rbuflen = data - rbuf;
    data = rbuf;
    if ( gettimeofday( &tv, 0 ) < 0 ) {
        LOG(log_error, logtype_afpd, "afp_getsrvrparms: gettimeofday: %s", strerror(errno) );
        *rbuflen = 0;
        return AFPERR_PARAM;
    }
    tv.tv_sec = AD_DATE_FROM_UNIX(tv.tv_sec);
    memcpy(data, &tv.tv_sec, sizeof( u_int32_t));
    data += sizeof( u_int32_t);
    *data = vcnt;
    return( AFP_OK );
}

/* ------------------------- 
 * we are the user here
*/
int afp_openvol(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct stat	st;
    char	*volname;
#ifndef CNID_DB
    char        *p;
#else
    int         opened = 0;
#endif /* CNID_DB */
    struct vol	*volume;
    struct dir	*dir;
    int		len, ret, buflen;
    u_int16_t	bitmap;

    ibuf += 2;
    memcpy(&bitmap, ibuf, sizeof( bitmap ));
    bitmap = ntohs( bitmap );
    ibuf += sizeof( bitmap );
    if (( bitmap & (1<<VOLPBIT_VID)) == 0 ) {
        ret = AFPERR_BITMAP;
        goto openvol_err;
    }

    len = (unsigned char)*ibuf++;
    volname = obj->oldtmp;
    memcpy(volname, ibuf, len );
    *(volname +  len) = '\0';
    ibuf += len;
    if ((len + 1) & 1) /* pad to an even boundary */
        ibuf++;

    load_volumes(obj);

    for ( volume = Volumes; volume; volume = volume->v_next ) {
        if ( strcasecmp( volname, volume->v_name ) == 0 ) {
            break;
        }
    }

    if ( volume == NULL ) {
        ret = AFPERR_PARAM;
        goto openvol_err;
    }

    /* check for a volume password */
    if (volume->v_password && strncmp(ibuf, volume->v_password, VOLPASSLEN)) {
        ret = AFPERR_ACCESS;
        goto openvol_err;
    }
    /* FIXME 
    */
    if (afp_version >= 30) {
        volume->max_filename = 255;
    }
    else {
        volume->max_filename = MACFILELEN;
    }
    if (( volume->v_flags & AFPVOL_OPEN  ) == 0 ) {
        /* FIXME unix name != mac name */
        if ((dir = dirnew(volume->v_name, volume->v_name) ) == NULL) {
            LOG(log_error, logtype_afpd, "afp_openvol: malloc: %s", strerror(errno) );
            ret = AFPERR_MISC;
            goto openvol_err;
        }
        dir->d_did = DIRDID_ROOT;
        dir->d_color = DIRTREE_COLOR_BLACK; /* root node is black */
        volume->v_dir = volume->v_root = dir;
        volume->v_flags |= AFPVOL_OPEN;
#ifdef CNID_DB
        volume->v_db = NULL;
        opened = 1;
#endif
	if (volume->v_root_preexec) {
	    if ((ret = afprun(1, volume->v_root_preexec, NULL)) && volume->v_root_preexec_close) {
                LOG(log_error, logtype_afpd, "afp_openvol: root preexec : %d", ret );
                ret = AFPERR_MISC;
                goto openvol_err;
	    }
	}
	if (volume->v_preexec) {
	    if ((ret = afprun(0, volume->v_preexec, NULL)) && volume->v_preexec_close) {
                LOG(log_error, logtype_afpd, "afp_openvol: preexec : %d", ret );
                ret = AFPERR_MISC;
                goto openvol_err;
	    }
	}
    }
    else {
       /* FIXME */
    }    
#ifdef FORCE_UIDGID
    set_uidgid ( volume );
#endif /* FORCE_UIDGID */

    if ( stat( volume->v_path, &st ) < 0 ) {
        ret = AFPERR_PARAM;
        goto openvol_err;
    }

    if ( chdir( volume->v_path ) < 0 ) {
        ret = AFPERR_PARAM;
        goto openvol_err;
    }
    curdir = volume->v_dir;

#ifdef CNID_DB
    if (opened) {
        if (volume->v_dbpath)
            volume->v_db = cnid_open (volume->v_dbpath, volume->v_umask);
        if (volume->v_db == NULL)
            volume->v_db = cnid_open (volume->v_path, volume->v_umask);
        if (volume->v_db == NULL) {
           /* config option? 
            * - mount the volume readonly
            * - return an error
            * - read/write with other scheme
            */
        }
    }
#endif /* CNID_DB */

    buflen = *rbuflen - sizeof( bitmap );
    if (( ret = getvolparams( bitmap, volume, &st,
                              rbuf + sizeof(bitmap), &buflen )) != AFP_OK ) {
        goto openvol_err;
    }
#ifdef AFP3x
    volume->v_utf8toucs2 = (iconv_t)(-1);
    volume->v_ucs2toutf8 = (iconv_t)(-1);
    volume->v_mactoutf8  = (iconv_t)(-1);
    volume->v_ucs2tomac  = (iconv_t)(-1);
    
    if (vol_utf8(volume)) {
        if ((iconv_t)(-1) == (volume->v_utf8toucs2 = iconv_open("UCS-2LE", "UTF-8"))) {
            LOG(log_error, logtype_afpd, "openvol: no UTF8 to UCS-2LE");
            goto openvol_err;
        }
        if ((iconv_t)(-1) == (volume->v_ucs2toutf8 = iconv_open("UTF-8", "UCS-2LE"))) {
            LOG(log_error, logtype_afpd, "openvol: no UCS-2LE to UTF-8");
            goto openvol_err_iconv;
        }
        if ((iconv_t)(-1) == (volume->v_mactoutf8 = iconv_open("UTF-8", "MAC"))) {
            LOG(log_error, logtype_afpd, "openvol: no MAC to UTF-8");
            goto openvol_err_iconv;
        }
        if ((iconv_t)(-1) == (volume->v_ucs2tomac = iconv_open("MAC", "UCS-2LE"))) {
            LOG(log_error, logtype_afpd, "openvol:  no UCS-2LE to MAC");
            goto openvol_err_iconv;
        }
    }
#endif
    *rbuflen = buflen + sizeof( bitmap );
    bitmap = htons( bitmap );
    memcpy(rbuf, &bitmap, sizeof( bitmap ));

#ifndef CNID_DB
    /*
     * If you mount a volume twice, the second time the trash appears on
     * the desk-top.  That's because the Mac remembers the DID for the
     * trash (even for volumes in different zones, on different servers).
     * Just so this works better, we prime the DID cache with the trash,
     * fixing the trash at DID 17.
     */
    p = Trash;
    cname( volume, volume->v_dir, &p );
#endif /* CNID_DB */

    return( AFP_OK );
#ifdef AFP3x
openvol_err_iconv:
    if (volume->v_utf8toucs2 != (iconv_t)(-1))
        iconv_close(volume->v_utf8toucs2);
    if (volume->v_ucs2toutf8 != (iconv_t)(-1))
        iconv_close(volume->v_ucs2toutf8);
    if (volume->v_mactoutf8  != (iconv_t)(-1))
        iconv_close(volume->v_mactoutf8);    
    if (volume->v_ucs2tomac  != (iconv_t)(-1))
        iconv_close(volume->v_ucs2tomac);
#endif        
openvol_err:
#ifdef CNID_DB
    if (opened && volume->v_db != NULL) {
        cnid_close(volume->v_db);
        volume->v_db = NULL;
    }
#endif
    *rbuflen = 0;
    return ret;
}

/* ------------------------- */
static void closevol(struct vol	*vol)
{
    if (!vol)
        return;

    dirfree( vol->v_root );
    vol->v_dir = NULL;
#ifdef CNID_DB
    cnid_close(vol->v_db);
    vol->v_db = NULL;
#endif /* CNID_DB */

#ifdef AFP3x
    if (vol->v_utf8toucs2 != (iconv_t)(-1))
        iconv_close(vol->v_utf8toucs2);
    if (vol->v_ucs2toutf8 != (iconv_t)(-1))
        iconv_close(vol->v_ucs2toutf8);
    if (vol->v_mactoutf8  != (iconv_t)(-1))
        iconv_close(vol->v_mactoutf8);
    if (vol->v_ucs2tomac  != (iconv_t)(-1))
        iconv_close(vol->v_ucs2tomac);
#endif

    if (vol->v_postexec) {
	afprun(0, vol->v_postexec, NULL);
    }
    if (vol->v_root_postexec) {
	afprun(1, vol->v_root_postexec, NULL);
    }
}

/* ------------------------- */
void close_all_vol(void)
{
    struct vol	*ovol;
    curdir = NULL;
    for ( ovol = Volumes; ovol; ovol = ovol->v_next ) {
        if ( ovol->v_flags & AFPVOL_OPEN ) {
            ovol->v_flags &= ~AFPVOL_OPEN;
            closevol(ovol);
        }
    }
}

/* ------------------------- */
int afp_closevol(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct vol	*vol, *ovol;
    u_int16_t	vid;

    *rbuflen = 0;
    ibuf += 2;
    memcpy(&vid, ibuf, sizeof( vid ));
    if (NULL == ( vol = getvolbyvid( vid )) ) {
        return( AFPERR_PARAM );
    }

    vol->v_flags &= ~AFPVOL_OPEN;
    for ( ovol = Volumes; ovol; ovol = ovol->v_next ) {
        if ( ovol->v_flags & AFPVOL_OPEN ) {
            break;
        }
    }
    if ( ovol != NULL ) {
        /* Even if chdir fails, we can't say afp_closevol fails. */
        if ( chdir( ovol->v_path ) == 0 ) {
            curdir = ovol->v_dir;
        }
    }

    closevol(vol);
    if (vol->v_deleted) {
	showvol(vol->v_name);
	volume_free(vol);
	volume_unlink(vol);
    }
    return( AFP_OK );
}

/* ------------------------- */
struct vol *getvolbyvid(const u_int16_t vid )
{
    struct vol	*vol;

    for ( vol = Volumes; vol; vol = vol->v_next ) {
        if ( vid == vol->v_vid ) {
            break;
        }
    }
    if ( vol == NULL || ( vol->v_flags & AFPVOL_OPEN ) == 0 ) {
        return( NULL );
    }

#ifdef FORCE_UIDGID
    set_uidgid ( vol );
#endif /* FORCE_UIDGID */

    return( vol );
}

/* ------------------------ */
static int ext_cmp_key(const void *key, const void *obj)
{
    const char          *p = key;
    const struct extmap *em = obj;
    return strdiacasecmp(p, em->em_ext);
}
struct extmap *getextmap(const char *path)
{
    char	  *p;
    struct extmap *em;

    if (NULL == ( p = strrchr( path, '.' )) ) {
        return( Defextmap );
    }
    p++;
    if (!*p || !Extmap_cnt) {
        return( Defextmap );
    }
    em = bsearch(p, Extmap, Extmap_cnt, sizeof(struct extmap), ext_cmp_key);
    if (em) {
        return( em );
    } else {
        return( Defextmap );
    }
}

/* ------------------------- */
struct extmap *getdefextmap(void)
{
    return( Defextmap );
}

/* --------------------------
   poll if a volume is changed by other processes.
*/
int  pollvoltime(obj)
AFPObj *obj;
{
    struct vol	     *vol;
    struct timeval   tv;
    struct stat      st;
    
    if (!(afp_version > 21 && obj->options.server_notif)) 
         return 0;

    if ( gettimeofday( &tv, 0 ) < 0 ) 
         return 0;

    for ( vol = Volumes; vol; vol = vol->v_next ) {
        if ( (vol->v_flags & AFPVOL_OPEN)  && vol->v_time + 30 < tv.tv_sec) {
            if ( !stat( vol->v_path, &st ) && vol->v_time != st.st_mtime ) {
                vol->v_time = st.st_mtime;
                if (!obj->attention(obj->handle, AFPATTN_NOTIFY | AFPATTN_VOLCHANGED))
                    return -1;
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------- */
void setvoltime(obj, vol )
AFPObj *obj;
struct vol	*vol;
{
    struct timeval	tv;

    /* just looking at vol->v_time is broken seriously since updates
     * from other users afpd processes never are seen.
     * This is not the most elegant solution (a shared memory between
     * the afpd processes would come closer)
     * [RS] */

    if ( gettimeofday( &tv, 0 ) < 0 ) {
        LOG(log_error, logtype_afpd, "setvoltime: gettimeofday: %s", strerror(errno) );
        return;
    }
    if( utime( vol->v_path, NULL ) < 0 ) {
        /* write of time failed ... probably a read only filesys,
         * where no other users can interfere, so there's no issue here
         */
    }

    /* a little granularity */
    if (vol->v_time < tv.tv_sec) {
        vol->v_time = tv.tv_sec;
        if (afp_version > 21 && obj->options.server_notif) {
            obj->attention(obj->handle, AFPATTN_NOTIFY | AFPATTN_VOLCHANGED);
        }
    }
}

/* ------------------------- */
int afp_getvolparams(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct stat	st;
    struct vol	*vol;
    int		buflen, ret;
    u_int16_t	vid, bitmap;

    ibuf += 2;
    memcpy(&vid, ibuf, sizeof( vid ));
    ibuf += sizeof( vid );
    memcpy(&bitmap, ibuf, sizeof( bitmap ));
    bitmap = ntohs( bitmap );

    if (NULL == ( vol = getvolbyvid( vid )) ) {
        *rbuflen = 0;
        return( AFPERR_PARAM );
    }

    if ( stat( vol->v_path, &st ) < 0 ) {
        *rbuflen = 0;
        return( AFPERR_PARAM );
    }

    buflen = *rbuflen - sizeof( bitmap );
    if (( ret = getvolparams( bitmap, vol, &st,
                              rbuf + sizeof( bitmap ), &buflen )) != AFP_OK ) {
        *rbuflen = 0;
        return( ret );
    }
    *rbuflen = buflen + sizeof( bitmap );
    bitmap = htons( bitmap );
    memcpy(rbuf, &bitmap, sizeof( bitmap ));
    return( AFP_OK );
}

/* ------------------------- */
int afp_setvolparams(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct adouble ad;
    struct vol	*vol;
    u_int16_t	vid, bitmap;
    u_int32_t   aint;

    ibuf += 2;
    *rbuflen = 0;

    memcpy(&vid, ibuf, sizeof( vid ));
    ibuf += sizeof( vid );
    memcpy(&bitmap, ibuf, sizeof( bitmap ));
    bitmap = ntohs( bitmap );
    ibuf += sizeof(bitmap);

    if (( vol = getvolbyvid( vid )) == NULL ) {
        return( AFPERR_PARAM );
    }

    if (vol->v_flags & AFPVOL_RO)
        return AFPERR_VLOCK;

    /* we can only set the backup date. */
    if (bitmap != (1 << VOLPBIT_BDATE))
        return AFPERR_BITMAP;

    memset(&ad, 0, sizeof(ad));
    if ( ad_open( vol->v_path, ADFLAGS_HF|ADFLAGS_DIR, O_RDWR,
                  0666, &ad) < 0 ) {
        if (errno == EROFS)
            return AFPERR_VLOCK;

        return AFPERR_ACCESS;
    }

    memcpy(&aint, ibuf, sizeof(aint));
    ad_setdate(&ad, AD_DATE_BACKUP, aint);
    ad_flush(&ad, ADFLAGS_HF);
    ad_close(&ad, ADFLAGS_HF);
    return( AFP_OK );
}

/* ------------------------- */
int wincheck(const struct vol *vol, const char *path)
{
    int len;

    if (!(vol->v_flags & AFPVOL_MSWINDOWS))
        return 1;

    /* empty paths are not allowed */
    if ((len = strlen(path)) == 0)
        return 0;

    /* leading or trailing whitespaces are not allowed, carriage returns
     * and probably other whitespace is okay, tabs are not allowed
     */
    if ((path[0] == ' ') || (path[len-1] == ' '))
        return 0;

    /* certain characters are not allowed */
    if (strpbrk(path, MSWINDOWS_BADCHARS))
        return 0;

    /* everything else is okay */
    return 1;
}
