/*
 * $Id: cnid_close.c,v 1.12.2.5 2002-06-17 21:09:01 jmarcus Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#ifdef CNID_DB
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif /* HAVE_FCNTL_H */
#include <stdlib.h>
#include <syslog.h>
#include <db.h>
#include <errno.h>
#include <string.h>

#include <atalk/cnid.h>

#include "cnid_private.h"

void cnid_close(void *CNID) {
    CNID_private *db;
    int rc;

    if (!(db = CNID)) {
        return;
    }

    /* Flush the transaction log and delete the log file if we can. */
    if ((db->lockfd > -1) && ((db->flags & CNIDFLAG_DB_RO) == 0)) {
        struct flock lock;

        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = lock.l_len = 0;
        if (fcntl(db->lockfd, F_SETLK, &lock) == 0) {
            char **list, **first;
            int cfd = -1;


            /* Checkpoint the databases until we can checkpoint no
             * more. */
#if DB_VERSION_MAJOR >= 4
	    rc = db->dbenv->txn_checkpoint(db->dbenv, 0, 0, 0);
#else
            rc = txn_checkpoint(db->dbenv, 0, 0, 0);
#endif
            while (rc == DB_INCOMPLETE) {
#if DB_VERSION_MAJOR >= 4
		rc = db->dbenv->txn_checkpoint(db->dbenv, 0, 0, 0);
#else
                rc = txn_checkpoint(db->dbenv, 0, 0, 0);
#endif
            }

#if DB_VERSION_MAJOR >= 4
            if ((rc = db->dbenv->log_archive(db->dbenv, &list, DB_ARCH_ABS)) != 0) {
#elif DB_VERSION_MINOR > 2
            if ((rc = log_archive(db->dbenv, &list, DB_ARCH_ABS)) != 0) {
#else /* DB_VERSION_MINOR < 2 */
            if ((rc = log_archive(db->dbenv, &list, DB_ARCH_ABS, NULL)) != 0) {
#endif /* DB_VERSION_MINOR */
                syslog(LOG_ERR, "cnid_close: Unable to archive logfiles: %s", db_strerror(rc));
            }

            if (list != NULL) {
                for (first = list; *list != NULL; ++list) {
                    if ((rc = remove(*list)) != 0) {
#ifdef DEBUG
                        syslog(LOG_INFO, "cnid_close: failed to remove %s: %s", *list, strerror(rc));
#endif
                    }
                }
                free(first);
            }
        }
        (void)remove(db->lock_file);
    }

    db->db_didname->close(db->db_didname, 0);
    db->db_devino->close(db->db_devino, 0);
    db->db_cnid->close(db->db_cnid, 0);
    db->dbenv->close(db->dbenv, 0);

    if (db->lockfd > -1) {
        close(db->lockfd); /* This will also release any locks we have. */
    }

    free(db);
}
#endif /* CNID_DB */
