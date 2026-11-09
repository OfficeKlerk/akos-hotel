//синхронизация на именованных семафорах POSIX (sem_open)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "hotel.h"

const char *sync_name = "именованные семафоры POSIX + разделяемая память POSIX";
sem_t *sem_mtx;
sem_t *sem_admin;

//семафоры клиентов открываются по мере надобности, процессу нужны не все
static sem_t *cs[MAX_SLOTS];

//открывает семафор или завершает процесс
static sem_t *open_sem(const char *name, int flags, unsigned value)
{
    sem_t *s = sem_open(name, flags, 0600, value);
    if (s == SEM_FAILED)
    {
        die("sem_open %s: %s", name, strerror(errno));
    }
    return s;
}

//удаляет имена всех семафоров гостиницы, в том числе оставшиеся от упавшего запуска
void sync_remove(void)
{
    char name[32];
    sem_unlink("/hotel12_mtx");
    sem_unlink("/hotel12_admin");
    for (int i = 0; i < MAX_SLOTS; ++i)
    {
        snprintf(name, sizeof name, "/hotel12_c%d", i);
        sem_unlink(name);
    }
}

//создает семафоры заново
void sync_create(void)
{
    sync_remove();
    sem_mtx = open_sem("/hotel12_mtx", O_CREAT | O_EXCL, 1);
    sem_admin = open_sem("/hotel12_admin", O_CREAT | O_EXCL, 0);
}

//открывает семафоры, созданные администратором
void sync_attach(void)
{
    sem_mtx = open_sem("/hotel12_mtx", 0, 0);
    sem_admin = open_sem("/hotel12_admin", 0, 0);
}

//семафор клиента в ячейке slot; кто обратится первым, тот и создаст
sem_t *client_sem(int slot)
{
    if (!cs[slot])
    {
        char name[32];
        snprintf(name, sizeof name, "/hotel12_c%d", slot);
        cs[slot] = open_sem(name, O_CREAT, 0);
    }
    return cs[slot];
}
