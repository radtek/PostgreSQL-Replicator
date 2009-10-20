/*
 * $PostgreSQL$
 */
#ifndef __PTHREAD_H
#define __PTHREAD_H

typedef ULONG pthread_key_t;
typedef CRITICAL_SECTION *pthread_mutex_t;
typedef int pthread_once_t;

DWORD		pthread_self(void);

void		pthread_setspecific(pthread_key_t, void *);
void	   *pthread_getspecific(pthread_key_t);

int			pthread_mutex_init(pthread_mutex_t *, void *attr);
int			pthread_mutex_lock(pthread_mutex_t *);

/* blocking */
int			pthread_mutex_unlock(pthread_mutex_t *);

#endif
