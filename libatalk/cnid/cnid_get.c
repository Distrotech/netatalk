/*
 * $Id: cnid_get.c,v 1.9.2.2 2001-12-03 15:53:39 jmarcus Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#ifdef CNID_DB
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <syslog.h>
#include <errno.h>

#include <db.h>
#include <netatalk/endian.h>
#include <atalk/adouble.h>
#include <atalk/cnid.h>

#include "cnid_private.h"

/* Return CNID for a given did/name. */
cnid_t cnid_get(void *CNID, const cnid_t did, const char *name,
                const int len)
{
    char start[CNID_DID_LEN + MAXPATHLEN + 1], *buf;
    CNID_private *db;
    DBT key, data;
    cnid_t id;
    int rc;

    if (!(db = CNID) || (len > MAXPATHLEN)) {
        return 0;
    }

    memset(&key, 0, sizeof(key));
    memset(&data, 0, sizeof(data));

    buf = start;
    memcpy(buf, &did, sizeof(did));
    buf += sizeof(did);
    memcpy(buf, name, len);
    *(buf + len) = '\0'; /* Make it a C-string. */
    key.data = start;
    key.size = CNID_DID_LEN + len + 1;

    while ((rc = db->db_didname->get(db->db_didname, NULL, &key, &data, 0))) {
        if (rc == DB_LOCK_DEADLOCK) {
            continue;
        }

        if (rc != DB_NOTFOUND) {
            syslog(LOG_ERR, "cnid_get: Unable to get CNID %u, name %s: %s",
                   ntohl(did), name, db_strerror(rc));
        }

        return 0;
    }

    memcpy(&id, data.data, sizeof(id));
#ifdef DEBUG
    syslog(LOG_INFO, "cnid_get: Returning CNID for %u, name %s as %u",
           ntohl(did), name, ntohl(id));
#endif
    return id;
}
#endif /* CNID_DB */
