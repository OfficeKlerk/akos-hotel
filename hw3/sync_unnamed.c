//синхронизация на безымянных семафорах POSIX, лежащих в разделяемой памяти
#include <errno.h>
#include <string.h>
#include "hotel.h"

const char *sync_name = "безымянные семафоры POSIX в разделяемой памяти POSIX";
sem_t *sem_mtx;
sem_t *sem_admin;

//семафоры уже лежат в разделяемой памяти, остается только указать на них
void sync_attach(void)
{
    sem_mtx = &g->sems[0];
    sem_admin = &g->sems[1];
}

//инициализирует все семафоры; второй аргумент sem_init = 1 - семафор общий для процессов
void sync_create(void)
{
    for (int i = 0; i < 2 + MAX_SLOTS; ++i)
    {
        if (sem_init(&g->sems[i], 1, i == 0 ? 1 : 0) != 0)
        {
            die("sem_init: %s (нужен Linux)", strerror(errno));
        }
    }
    sync_attach();
}

//уничтожает семафоры перед удалением разделяемой памяти
void sync_remove(void)
{
    for (int i = 0; i < 2 + MAX_SLOTS; ++i)
    {
        sem_destroy(&g->sems[i]);
    }
}

//семафор клиента в ячейке slot
sem_t *client_sem(int slot)
{
    return &g->sems[2 + slot];
}
