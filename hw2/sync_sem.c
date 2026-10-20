//синхронизация на мьютексе и семафорах (основная программа)
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hotel.h"

const char *sync_name = "мьютекс + семафоры";

//семафор администратора: сколько раз ему сообщили о новой работе
static sem_t admin_sem;

//семафор main: сколько клиентов покинуло модель
static sem_t main_sem;

//создает семафор или завершает программу, на macOS безымянные семафоры не поддерживаются
static void sem_create(sem_t *s)
{
    if (sem_init(s, 0, 0) != 0)
    {
        fprintf(stderr, "sem_init: %s (программа рассчитана на Linux)\n", strerror(errno));
        exit(1);
    }
}

//ждет семафор, повторяя ожидание после прерывания сигналом
static void sem_wait_retry(sem_t *s)
{
    while (sem_wait(s) != 0 && errno == EINTR)
    {
    }
}

//создает семафоры администратора и main
void sync_init(void)
{
    sem_create(&admin_sem);
    sem_create(&main_sem);
}

//у каждого клиента свой семафор "решение принято"
void sync_client_init(client_t *c)
{
    c->sync = malloc(sizeof(sem_t));
    sem_create(c->sync);
}

//удаляет семафор клиента
void sync_client_free(client_t *c)
{
    sem_destroy(c->sync);
    free(c->sync);
}

//ждать на семафоре под мьютексом нельзя - отпускаем его на время ожидания
static void wait_unlocked(sem_t *s)
{
    pthread_mutex_unlock(&mtx);
    sem_wait_retry(s);
    pthread_mutex_lock(&mtx);
}

//клиент ждет решения администратора
void wait_client(client_t *c)
{
    wait_unlocked(c->sync);
}

//администратор сообщает клиенту о решении
void wake_client(client_t *c)
{
    sem_post(c->sync);
}

//администратор ждет работы
void wait_admin(void)
{
    wait_unlocked(&admin_sem);
}

//сообщает администратору о новой работе
void wake_admin(void)
{
    sem_post(&admin_sem);
}

//main ждет ухода очередного клиента
void wait_main(void)
{
    wait_unlocked(&main_sem);
}

//клиент сообщает main, что покинул модель
void wake_main(void)
{
    sem_post(&main_sem);
}

//точка входа программы
int main(int argc, char **argv)
{
    return hotel_run(argc, argv);
}
