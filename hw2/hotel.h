//общая часть модели для hotel_sem и hotel_cond
#ifndef HOTEL_H
#define HOTEL_H

#include <pthread.h>

//состояние клиента
typedef enum
{
    C_REQUESTED,
    C_WAITING,
    C_ASSIGNED,
    C_REJECTED
} cstate_t;

//клиент гостиницы
typedef struct client
{
    int id;
    int stay;
    int days;
    int room; //-1, если номера нет
    cstate_t state;
    long enqueue_ms;
    struct client *next;
    void *sync; //семафор или условная переменная клиента
} client_t;

//общий мьютекс для состояния гостиницы и вывода
extern pthread_mutex_t mtx;

//синхронизация, реализована в sync_sem.c и sync_cond.c, вызывается под mtx
void sync_init(void);
void sync_client_init(client_t *c);
void sync_client_free(client_t *c);
void wait_client(client_t *c);
void wake_client(client_t *c);
void wait_admin(void);
void wake_admin(void);
void wait_main(void);
void wake_main(void);

extern const char *sync_name;

int hotel_run(int argc, char **argv);

#endif
