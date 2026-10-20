//общая часть модели гостиницы для двух программ:
//hotel_sem (мьютекс + семафоры) и hotel_cond (мьютекс + условные переменные)
#ifndef HOTEL_H
#define HOTEL_H

#include <pthread.h>

//состояние клиента
typedef enum
{
    C_REQUESTED, //подал запрос администратору
    C_WAITING, //стоит в очереди
    C_ASSIGNED, //номер назначен
    C_REJECTED //получил отказ
} cstate_t;

//клиент гостиницы, у каждого свой поток
typedef struct client
{
    int id;
    int stay; //заказанный срок в сутках
    int days; //сколько суток прожито
    int room; //номер комнаты или -1
    cstate_t state;
    long enqueue_ms; //когда встал в очередь
    struct client *next; //следующий в списке запросов или в очереди
    void *sync; //примитив, на котором клиент ждет решения администратора
} client_t;

//общий мьютекс, под ним все состояние гостиницы и весь вывод
extern pthread_mutex_t mtx;

//функции синхронизации, у каждой программы своя реализация (sync_sem.c, sync_cond.c)
//все вызываются под mtx; функции ожидания отпускают mtx на время сна и захватывают обратно
void sync_init(void);
void sync_client_init(client_t *c);
void sync_client_free(client_t *c);
void wait_client(client_t *c); //клиент ждет решения администратора
void wake_client(client_t *c);
void wait_admin(void); //администратор ждет работы
void wake_admin(void);
void wait_main(void); //main ждет, пока клиенты покинут модель
void wake_main(void);

//название программы для вывода
extern const char *sync_name;

//запуск модели, вызывается из main каждой программы
int hotel_run(int argc, char **argv);

#endif
