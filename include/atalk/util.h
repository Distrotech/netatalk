/*
 * $Id: util.h,v 1.7.10.3 2004-02-14 00:30:52 didg Exp $
 */

#ifndef _ATALK_UTIL_H
#define _ATALK_UTIL_H 1

#include <sys/cdefs.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <netatalk/at.h>

extern int     sys_ftruncate __P((int fd, off_t length));

#ifdef WITH_SENDFILE
extern ssize_t sys_sendfile __P((int __out_fd, int __in_fd, off_t *__offset,size_t __count));
#endif

extern const int _diacasemap[], _dialowermap[];

extern char **getifacelist(void);
extern void freeifacelist(char **);

#define diatolower(x)     _dialowermap[(unsigned char) (x)]
#define diatoupper(x)     _diacasemap[(unsigned char) (x)]
extern int atalk_aton     __P((char *, struct at_addr *));
extern void bprint        __P((char *, int));
extern int strdiacasecmp  __P((const char *, const char *));
extern int strndiacasecmp __P((const char *, const char *, size_t));
extern pid_t server_lock  __P((char * /*program*/, char * /*file*/, 
			       int /*debug*/));
extern void fault_setup	  __P((void (*fn)(void *)));
#define server_unlock(x)  (unlink(x))

#ifndef HAVE_STRLCPY
size_t strlcpy(char *d, const char *s, size_t bufsize);
#endif
 
#ifndef HAVE_STRLCAT
size_t strlcat(char *d, const char *s, size_t bufsize);
#endif

#ifndef HAVE_DLFCN_H
extern void *mod_open    __P((const char *));
extern void *mod_symbol  __P((void *, const char *));
extern void mod_close    __P((void *));
#define mod_error()      ""
#else /* ! HAVE_DLFCN_H */
#include <dlfcn.h>

#ifndef RTLD_NOW
#define RTLD_NOW 1
#endif /* ! RTLD_NOW */

/* NetBSD doesn't like RTLD_NOW for dlopen (it fails). Use RTLD_LAZY. */
#ifdef __NetBSD__
#define mod_open(a)      dlopen(a, RTLD_LAZY)
#else /* ! __NetBSD__ */
#define mod_open(a)      dlopen(a, RTLD_NOW)
#endif /* __NetBSD__ */

#ifndef DLSYM_PREPEND_UNDERSCORE
#define mod_symbol(a, b) dlsym(a, b)
#else /* ! DLSYM_PREPEND_UNDERSCORE */
extern void *mod_symbol  __P((void *, const char *));
#endif /* ! DLSYM_PREPEND_UNDERSCORE */
#define mod_error()      dlerror()
#define mod_close(a)     dlclose(a)
#endif /* ! HAVE_DLFCN_H */

#endif
