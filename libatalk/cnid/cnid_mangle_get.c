/*
 * $Id: cnid_mangle_get.c,v 1.6.2.1 2003-02-08 03:16:53 jmarcus Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#ifdef FILE_MANGLING
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <string.h>
#include <atalk/logger.h>
#include <errno.h>

#include <db.h>
#include <netatalk/endian.h>
#include <atalk/adouble.h>
#include <atalk/cnid.h>

#include "cnid_private.h"

/* Find a mangled filename entry. */
char *
cnid_mangle_get(void *CNID, char *mfilename)
{
    CNID_private *db;
    DBT key, data;
    cnid_t id;
    struct stat st;
    char *filename;
    int rc;

    if (!(db = CNID)) {
        return NULL;
    }

    memset(&key, 0, sizeof(key));
    memset(&data, 0, sizeof(data));

    key.data = mfilename;
    key.size = strlen(mfilename);

    while ((rc = db->db_mangle->get(db->db_mangle, NULL, &key, &data, 0))) {
        if (rc == DB_LOCK_DEADLOCK) {
            continue;
        }

        if (rc == DB_NOTFOUND) {
            LOG(log_debug, logtype_default, "cnid_mangle_get: Failed to find mangled entry for %s", mfilename);
            return NULL;

        }

        LOG(log_error, logtype_default, "cnid_mangle_get: Failed to get mangle entry from the database: %s", db_strerror(rc));
        return NULL;
    }

    filename = (char *)data.data;

    return filename;
}
#endif /* FILE_MANGLING */
