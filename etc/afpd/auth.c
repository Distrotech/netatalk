/*
 * $Id: auth.c,v 1.19.2.2 2002-02-07 23:55:35 srittau Exp $
 *
 * Copyright (c) 1990,1993 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <netatalk/endian.h>
#include <atalk/afp.h>
#include <atalk/compat.h>
#include <atalk/util.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>

#ifdef SHADOWPW
#include <shadow.h>
#endif /* SHADOWPW */

#include <pwd.h>
#include <grp.h>
#include <syslog.h>

#ifdef TRU64
#include <netdb.h>
#include <arpa/inet.h>
#include <sia.h>
#include <siad.h>

extern void afp_get_cmdline( int *ac, char ***av );
#endif /* TRU64 */

#include "globals.h"
#include "auth.h"
#include "uam_auth.h"
#include "switch.h"
#include "status.h"

int	afp_version = 11;
static int afp_version_index;

uid_t	uuid;
#if defined( __svr4__ ) && !defined( NGROUPS )
#define NGROUPS NGROUPS_MAX
#endif /* __svr4__ NGROUPS */
#if defined( sun ) && !defined( __svr4__ ) || defined( ultrix )
int	groups[ NGROUPS ];
#else /* sun __svr4__ ultrix */
#if defined( __svr4__ ) && !defined( NGROUPS )
#define NGROUPS	NGROUPS_MAX
#endif /* __svr4__ NGROUPS */
gid_t	groups[ NGROUPS ];
#endif /* sun ultrix */
int	ngroups;

/*
 * These numbers are scattered throughout the code.
 */
static struct afp_versions	afp_versions[] = {
            { "AFPVersion 1.1",	11 },
            { "AFPVersion 2.0",	20 },
            { "AFPVersion 2.1",	21 },
            { "AFP2.2",	22 }
        };

static struct uam_mod uam_modules = {NULL, NULL, &uam_modules, &uam_modules};
static struct uam_obj uam_login = {"", "", 0, {{NULL}}, &uam_login,
                                      &uam_login};
static struct uam_obj uam_changepw = {"", "", 0, {{NULL}}, &uam_changepw,
                                         &uam_changepw};

static struct uam_obj *afp_uam = NULL;


void status_versions( data )
char	*data;
{
    char                *start = data;
    u_int16_t           status;
    int			len, num, i;

    memcpy(&status, start + AFPSTATUS_VERSOFF, sizeof(status));
    num = sizeof( afp_versions ) / sizeof( afp_versions[ 0 ] );
    data += ntohs( status );
    *data++ = num;
    for ( i = 0; i < num; i++ ) {
        len = strlen( afp_versions[ i ].av_name );
        *data++ = len;
        memcpy( data, afp_versions[ i ].av_name , len );
        data += len;
    }
    status = htons( data - start );
    memcpy(start + AFPSTATUS_UAMSOFF, &status, sizeof(status));
}

void status_uams(char *data, const char *authlist)
{
    char                *start = data;
    u_int16_t           status;
    struct uam_obj      *uams;
    int			len, num = 0;

    memcpy(&status, start + AFPSTATUS_UAMSOFF, sizeof(status));
    uams = &uam_login;
    while ((uams = uams->uam_prev) != &uam_login) {
        if (strstr(authlist, uams->uam_path))
            num++;
    }

    data += ntohs( status );
    *data++ = num;
    while ((uams = uams->uam_prev) != &uam_login) {
        if (strstr(authlist, uams->uam_path)) {
            syslog(LOG_INFO, "uam: \"%s\" available", uams->uam_name);
            len = strlen( uams->uam_name);
            *data++ = len;
            memcpy( data, uams->uam_name, len );
            data += len;
        }
    }

    /* icon offset */
    status = htons(data - start);
    memcpy(start + AFPSTATUS_ICONOFF, &status, sizeof(status));
}

/* handle errors by closing the connection. this is only needed
 * by the afp_* functions. */
static int send_reply(const AFPObj *obj, const int err)
{
    if ((err == AFP_OK) || (err == AFPERR_AUTHCONT))
        return err;

    obj->reply(obj->handle, err);
    obj->exit(0);

    return AFP_OK;
}

static int login(AFPObj *obj, struct passwd *pwd, void (*logout)(void))
{
#ifdef ADMIN_GRP
    int admin = 0;
#endif /* ADMIN_GRP */

    /* UAM had syslog control; afpd needs to reassert itself */
    openlog( "afpd", LOG_NDELAY|LOG_PID, LOG_DAEMON);

    if ( pwd->pw_uid == 0 ) {	/* don't allow root login */
        syslog( LOG_ERR, "login: root login denied!" );
        return AFPERR_NOTAUTH;
    }

    syslog( LOG_INFO, "login %s (uid %d, gid %d) %s", pwd->pw_name,
            pwd->pw_uid, pwd->pw_gid, afp_versions[afp_version_index] );

    if (obj->proto == AFPPROTO_ASP) {
        ASP asp = obj->handle;
        int addr_net = ntohs( asp->asp_sat.sat_addr.s_net );
        int addr_node  = asp->asp_sat.sat_addr.s_node;

        if (obj->options.authprintdir) {
            if(addr_net && addr_node) { /* Do we have a valid Appletalk address? */
                char nodename[256];
                FILE *fp;
                struct stat stat_buf;

                sprintf(nodename, "%s/net%d.%dnode%d", obj->options.authprintdir,
                        addr_net / 256, addr_net % 256, addr_node);
                syslog (LOG_INFO, "registering %s (uid %d) on %u.%u as %s",
                        pwd->pw_name, pwd->pw_uid, addr_net, addr_node, nodename);

                if (stat(nodename, &stat_buf) == 0) { /* file exists */
                    if (S_ISREG(stat_buf.st_mode)) { /* normal file */
                        unlink(nodename);
                        fp = fopen(nodename, "w");
                        fprintf(fp, "%s\n", pwd->pw_name);
                        fclose(fp);
                        chown( nodename, pwd->pw_uid, -1 );
                    } else { /* somebody is messing with us */
                        syslog( LOG_ERR, "print authfile %s is not a normal file, it will not be modified", nodename );
                    }
                } else { /* file 'nodename' does not exist */
                    fp = fopen(nodename, "w");
                    fprintf(fp, "%s\n", pwd->pw_name);
                    fclose(fp);
                    chown( nodename, pwd->pw_uid, -1 );
                }
            } /* if (addr_net && addr_node ) */
        } /* if (options->authprintdir) */
    } /* if (obj->proto == AFPPROTO_ASP) */

    if (initgroups( pwd->pw_name, pwd->pw_gid ) < 0) {
#ifdef RUN_AS_USER
        syslog(LOG_INFO, "running with uid %d", geteuid());
#else /* RUN_AS_USER */
        syslog(LOG_ERR, "login: %s", strerror(errno));
        return AFPERR_BADUAM;
#endif /* RUN_AS_USER */

    }

    /* Basically if the user is in the admin group, we stay root */

    if (( ngroups = getgroups( NGROUPS, groups )) < 0 ) {
        syslog( LOG_ERR, "login: getgroups: %s", strerror(errno) );
        return AFPERR_BADUAM;
    }
#ifdef ADMIN_GRP
#ifdef DEBUG
    syslog(LOG_INFO, "obj->options.admingid == %d", obj->options.admingid);
#endif /* DEBUG */
    if (obj->options.admingid != 0) {
        int i;
        for (i = 0; i < ngroups; i++) {
            if (groups[i] == obj->options.admingid) admin = 1;
        }
    }
    if (admin) syslog( LOG_INFO, "admin login -- %s", pwd->pw_name );
    if (!admin)
#endif /* DEBUG */
#ifdef TRU64
    {
        struct DSI *dsi = obj->handle;
        struct hostent *hp;
        char *clientname;
        int argc;
        char **argv;
        char hostname[256];

        afp_get_cmdline( &argc, &argv );

        hp = gethostbyaddr( (char *) &dsi->client.sin_addr,
                            sizeof( struct in_addr ),
                            dsi->client.sin_family );

        if( hp )
            clientname = hp->h_name;
        else
            clientname = inet_ntoa( dsi->client.sin_addr );

        sprintf( hostname, "%s@%s", pwd->pw_name, clientname );

        if( sia_become_user( NULL, argc, argv, hostname, pwd->pw_name,
                             NULL, FALSE, NULL, NULL,
                             SIA_BEU_REALLOGIN ) != SIASUCCESS )
            return AFPERR_BADUAM;

        syslog( LOG_INFO, "session from %s (%s)", hostname,
                inet_ntoa( dsi->client.sin_addr ) );

        if (setegid( pwd->pw_gid ) < 0 || seteuid( pwd->pw_uid ) < 0) {
            syslog( LOG_ERR, "login: %m" );
            return AFPERR_BADUAM;
        }
    }
#else /* TRU64 */
        if (setegid( pwd->pw_gid ) < 0 || seteuid( pwd->pw_uid ) < 0) {
            syslog( LOG_ERR, "login: %s", strerror(errno) );
            return AFPERR_BADUAM;
        }
#endif /* TRU64 */

    /* There's probably a better way to do this, but for now, we just
    play root */

#ifdef ADMIN_GRP
    if (admin) uuid = 0;
    else
#endif /* ADMIN_GRP */
        uuid = pwd->pw_uid;

    afp_switch = postauth_switch;
    obj->logout = logout;

    return( AFP_OK );
}

int afp_login(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct passwd *pwd = NULL;
    int		len, i, num;

    *rbuflen = 0;


    if ( nologin & 1)
        return send_reply(obj, AFPERR_SHUTDOWN );

    if (ibuflen <= 1)
        return send_reply(obj, AFPERR_BADVERS );

    ibuf++;
    len = (unsigned char) *ibuf++;

    ibuflen -= 2;
    if (!len || len > ibuflen)
        return send_reply(obj, AFPERR_BADVERS );

    num = sizeof( afp_versions ) / sizeof( afp_versions[ 0 ]);
    for ( i = 0; i < num; i++ ) {
        if ( strncmp( ibuf, afp_versions[ i ].av_name , len ) == 0 ) {
            afp_version = afp_versions[ i ].av_number;
            afp_version_index = i;
            break;
        }
    }
    if ( i == num ) 				/* An inappropo version */
        return send_reply(obj, AFPERR_BADVERS );
    ibuf += len;
    ibuflen -= len;

    if (ibuflen <= 1)
        return send_reply(obj, AFPERR_BADUAM);

    len = (unsigned char) *ibuf++;
    ibuflen--;

    if (!len || len > ibuflen)
        return send_reply(obj, AFPERR_BADUAM);

    if ((afp_uam = auth_uamfind(UAM_SERVER_LOGIN, ibuf, len)) == NULL)
        return send_reply(obj, AFPERR_BADUAM);
    ibuf += len;
    ibuflen -= len;

    i = afp_uam->u.uam_login.login(obj, &pwd, ibuf, ibuflen, rbuf, rbuflen);
    if (i || !pwd)
        return send_reply(obj, i);

    return send_reply(obj, login(obj, pwd, afp_uam->u.uam_login.logout));
}


int afp_logincont(obj, ibuf, ibuflen, rbuf, rbuflen)
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    struct passwd *pwd = NULL;
    int err;

    if ( afp_uam == NULL || afp_uam->u.uam_login.logincont == NULL ) {
        *rbuflen = 0;
        return send_reply(obj, AFPERR_NOTAUTH );
    }

    ibuf += 2;
    err = afp_uam->u.uam_login.logincont(obj, &pwd, ibuf, ibuflen,
                                         rbuf, rbuflen);
    if (err || !pwd)
        return send_reply(obj, err);

    return send_reply(obj, login(obj, pwd, afp_uam->u.uam_login.logout));
}


int afp_logout(obj, ibuf, ibuflen, rbuf, rbuflen)
AFPObj     *obj;
char       *ibuf, *rbuf;
int        ibuflen, *rbuflen;
{
    syslog(LOG_INFO, "logout %s", obj->username);
    obj->exit(0);
    return AFP_OK;
}



/* change password  --
 * NOTE: an FPLogin must already have completed successfully for this
 *       to work. this also does a little pre-processing before it hands
 *       it off to the uam. 
 */
int afp_changepw(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    char username[MACFILELEN + 1], *start = ibuf;
    struct uam_obj *uam;
    struct passwd *pwd;
    int len;

    *rbuflen = 0;
    ibuf += 2;

    /* make sure we can deal w/ this uam */
    len = (unsigned char) *ibuf++;
    if ((uam = auth_uamfind(UAM_SERVER_CHANGEPW, ibuf, len)) == NULL)
        return AFPERR_BADUAM;

    ibuf += len;
    if ((len + 1) & 1) /* pad byte */
        ibuf++;

    len = (unsigned char) *ibuf++;
    if ( len > sizeof(username) - 1) {
        return AFPERR_PARAM;
    }
    memcpy(username, ibuf, len);
    username[ len ] = '\0';
    ibuf += len;
    if ((len + 1) & 1) /* pad byte */
        ibuf++;

    syslog(LOG_INFO, "changing password for <%s>", username);

    if (( pwd = uam_getname( username, sizeof(username))) == NULL )
        return AFPERR_PARAM;

    /* send it off to the uam. we really don't use ibuflen right now. */
    ibuflen -= (ibuf - start);
    len = uam->u.uam_changepw(obj, username, pwd, ibuf, ibuflen,
                              rbuf, rbuflen);
    syslog(LOG_INFO, "password change %s.",
           (len == AFPERR_AUTHCONT) ? "continued" :
           (len ? "failed" : "succeeded"));
    return len;
}


/* FPGetUserInfo */
int afp_getuserinfo(obj, ibuf, ibuflen, rbuf, rbuflen )
AFPObj      *obj;
char	*ibuf, *rbuf;
int		ibuflen, *rbuflen;
{
    u_int8_t  thisuser;
    u_int32_t id;
    u_int16_t bitmap;

    *rbuflen = 0;
    ibuf++;
    thisuser = *ibuf++;
    ibuf += sizeof(id); /* userid is not used in AFP 2.0 */
    memcpy(&bitmap, ibuf, sizeof(bitmap));
    bitmap = ntohs(bitmap);

    /* deal with error cases. we don't have to worry about
     * AFPERR_ACCESS or AFPERR_NOITEM as geteuid and getegid always
     * succeed. */
    if (!thisuser)
        return AFPERR_PARAM;
    if ((bitmap & USERIBIT_ALL) != bitmap)
        return AFPERR_BITMAP;

    /* copy the bitmap back to reply buffer */
    memcpy(rbuf, ibuf, sizeof(bitmap));
    rbuf += sizeof(bitmap);
    *rbuflen = sizeof(bitmap);

    /* copy the user/group info */
    if (bitmap & USERIBIT_USER) {
        id = htonl(geteuid());
        memcpy(rbuf, &id, sizeof(id));
        rbuf += sizeof(id);
        *rbuflen += sizeof(id);
    }

    if (bitmap & USERIBIT_GROUP) {
        id = htonl(getegid());
        memcpy(rbuf, &id, sizeof(id));
        rbuf += sizeof(id);
        *rbuflen += sizeof(id);
    }

    return AFP_OK;
}

#define UAM_LIST(type) (((type) == UAM_SERVER_LOGIN) ? &uam_login : \
			(((type) == UAM_SERVER_CHANGEPW) ? \
			 &uam_changepw : NULL))

/* just do a linked list search. this could be sped up with a hashed
 * list, but i doubt anyone's going to have enough uams to matter. */
struct uam_obj *auth_uamfind(const int type, const char *name,
                                         const int len)
{
    struct uam_obj *prev, *start;

    if (!name || !(start = UAM_LIST(type)))
        return NULL;

    prev = start;
    while ((prev = prev->uam_prev) != start)
        if (strndiacasecmp(prev->uam_name, name, len) == 0)
            return prev;

    return NULL;
}

int auth_register(const int type, struct uam_obj *uam)
{
    struct uam_obj *start;

    if (!uam || !uam->uam_name || (*uam->uam_name == '\0'))
        return -1;

    if (!(start = UAM_LIST(type)))
        return 0; /* silently fail */

    uam_attach(start, uam);
    return 0;
}

/* load all of the modules */
int auth_load(const char *path, const char *list)
{
    char name[MAXPATHLEN + 1], buf[MAXPATHLEN + 1], *p;
    struct uam_mod *mod;
    struct stat st;
    int len;

    if (!path || !list || (len = strlen(path)) > sizeof(name) - 2)
        return -1;

    strncpy(buf, list, sizeof(buf));
    if ((p = strtok(buf, ",")) == NULL)
        return -1;

    strcpy(name, path);
    if (name[len - 1] != '/') {
        strcat(name, "/");
        len++;
    }

    while (p) {
        strncpy(name + len, p, sizeof(name) - len);
        syslog(LOG_DEBUG, "uam: loading (%s)", name);
        /*
        if ((stat(name, &st) == 0) && (mod = uam_load(name, p))) {
        */
        if (stat(name, &st) == 0) {
            if ((mod = uam_load(name, p))) {
                uam_attach(&uam_modules, mod);
                syslog(LOG_INFO, "uam: %s loaded", p);
            } else {
                syslog(LOG_INFO, "uam: %s load failure",p);
            }
        } else {
            syslog(LOG_INFO, "uam: uam not found (status=%d)", stat(name, &st));
        }
        p = strtok(NULL, ",");
    }

    return 0;
}

/* get rid of all of the uams */
void auth_unload()
{
    struct uam_mod *mod, *prev, *start = &uam_modules;

    prev = start->uam_prev;
    while ((mod = prev) != start) {
        prev = prev->uam_prev;
        uam_detach(mod);
        uam_unload(mod);
    }
}
