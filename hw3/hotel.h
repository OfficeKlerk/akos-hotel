//общая часть ИДЗ-3: гостиница на взаимодействующих процессах
#ifndef HOTEL_H
#define HOTEL_H

#include <semaphore.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>

#define MAX_ROOMS 64
#define MAX_SLOTS 128 //сколько клиентов может одновременно находиться в гостинице

//состояние клиента
typedef enum
{
    C_FREE,
    C_REQUESTED,
    C_WAITING,
    C_ASSIGNED,
    C_REJECTED
} cstate_t;

//клиент; в разделяемой памяти вместо указателей индексы: у разных процессов она лежит по разным адресам
typedef struct
{
    cstate_t state;
    pid_t pid;
    int id;
    int stay;
    int days;
    int room; //-1, если номера нет
    long enqueue_ms;
} client_t;

//параметры моделирования
typedef struct
{
    //гостиница
    int rooms;
    int queue_cap; //-1 - без ограничения
    int strat; //0 - fifo, 1 - short, 2 - long

    //клиенты
    int clients; //0 - без ограничения
    int arr_min; //интервал прибытия в часах
    int arr_max;
    int stay_min; //срок проживания в сутках
    int stay_max;

    //время
    int duration; //прием клиентов в сутках, 0 - без ограничения
    int day_ms; //длина модельных суток в мс

    //прочее
    unsigned long long seed;
    int seed_given;
    char log_path[256];
} params_t;

//все общее состояние гостиницы, лежит в разделяемой памяти
typedef struct
{
    params_t p;
    struct timespec t0; //момент запуска, общий для всех процессов
    pid_t owner_pid; //кто создал общие объекты и удалит их
    pid_t admin_pid;

    int room[MAX_ROOMS]; //индекс клиента в номере или -1
    int freed_by[MAX_ROOMS];
    int occupied;
    int req[MAX_SLOTS]; //запросы, которые еще не рассмотрел администратор
    int req_n;
    int wait[MAX_SLOTS]; //очередь ожидания
    int wait_n;
    client_t c[MAX_SLOTS];
    int alive; //клиенты, которые еще в модели
    int next_id;
    int arrivals_closed;

    //статистика
    int arrived;
    int served;
    int rejected;
    int lost;
    int max_wait_len;
    int max_occupied;
    int from_queue;
    long room_days;
    long total_wait_ms;
    long max_wait_ms;

    sem_t sems[2 + MAX_SLOTS]; //безымянные семафоры, используются только в hotel_unnamed
} shm_t;

extern params_t P;
extern shm_t *g;
extern int *stays;
extern int stays_len;
extern volatile sig_atomic_t interrupts;

//семафоры, у каждой программы своя реализация: sync_named.c или sync_unnamed.c
extern const char *sync_name;
extern sem_t *sem_mtx; //мьютекс на все общее состояние
extern sem_t *sem_admin; //администратору есть работа
void sync_create(void);
void sync_attach(void);
void sync_remove(void);
sem_t *client_sem(int slot); //клиенту принято решение

//common.c
void die(const char *fmt, ...);
void out(const char *fmt, ...);
void event(const char *fmt, ...);
long now_ms(void);
void sleep_ms(long ms);
int rng_range(int lo, int hi);
void parse_args(int argc, char **argv, const char *opts, const char *about);
void shm_create(void);
int shm_attach(void);
void hotel_cleanup(void);
void install_signals(void);
void lock(void);
void unlock(void);
int pid_alive(pid_t pid);
int slot_alloc(int stay);
void slot_free(int slot);
void close_arrivals(const char *reason);
void admin_run(void);
void client_run(int slot);
void check_invariants(void);
void violation(const char *what);
void print_stats(void);

#endif
