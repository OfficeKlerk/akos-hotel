//общая часть ИДЗ-3: параметры, вывод, разделяемая память, администратор и клиент
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "hotel.h"

#define SHM_NAME "/hotel12_shm"

//параметры со значениями по умолчанию
params_t P = {
    .rooms = 3,
    .queue_cap = -1,
    .clients = 12,
    .arr_min = 2,
    .arr_max = 14,
    .stay_min = 1,
    .stay_max = 4,
    .day_ms = 300,
};

//явные сроки клиентов из -S, нужны только родителю, поэтому лежат не в разделяемой памяти
int *stays = NULL;
int stays_len = 0;

//разделяемая память
shm_t *g = NULL;

//сколько раз пришел SIGINT/SIGTERM; в обработчике можно только менять такой флаг
volatile sig_atomic_t interrupts = 0;

//процесс, который создал общие объекты и должен их удалить
static pid_t owner = 0;

//дескриптор журнала, -1 если журнал не ведется
static int log_fd = -1;

//маска сигналов до lock(), восстанавливается в unlock()
static sigset_t saved_mask;

//состояние генератора splitmix64
static unsigned long long rng_state;

//=============================== вывод ===============================

//записывает весь буфер, write может записать не все сразу или прерваться сигналом
static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0)
    {
        ssize_t n = write(fd, buf, len);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

//одна строка на экран и в журнал; строка пишется одним write, поэтому строки процессов не перемешиваются
static void put(const char *buf, int n)
{
    if (n <= 0)
    {
        return;
    }
    write_all(STDOUT_FILENO, buf, (size_t)n);
    if (log_fd >= 0)
    {
        write_all(log_fd, buf, (size_t)n);
    }
}

//печатает ошибку и завершает процесс
void die(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dprintf(STDERR_FILENO, "Ошибка: %s\n", buf);
    exit(2);
}

//строка без отметки времени
void out(const char *fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

//событие с модельным временем "[сутки N ЧЧ:ММ]", вызывать под lock()
void event(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    long ms = now_ms();
    long min = (ms % P.day_ms) * 24 * 60 / P.day_ms;
    out("[сутки %3ld %02ld:%02ld] %s\n", ms / P.day_ms + 1, min / 60, min % 60, msg);
}

//сколько мс прошло с запуска; CLOCK_MONOTONIC общий для всех процессов
long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - g->t0.tv_sec) * 1000L + (t.tv_nsec - g->t0.tv_nsec) / 1000000L;
}

//сон, досыпает после прерывания сигналом
void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
    {
    }
}

//мс в модельные часы
static double hours(long ms)
{
    return (double)ms * 24.0 / P.day_ms;
}

//случайное число от lo до hi; свой генератор, чтобы seed давал одинаковый прогон на любой libc
int rng_range(int lo, int hi)
{
    unsigned long long z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return lo + (int)(z % (unsigned long long)(hi - lo + 1));
}

//=============================== синхронизация ===============================

//захват общего мьютекса; на это время SIGINT/SIGTERM блокируются, иначе процесс,
//убитый внутри критической секции, оставил бы семафор закрытым навсегда
void lock(void)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGTERM);
    sigprocmask(SIG_BLOCK, &s, &saved_mask);
    while (sem_wait(sem_mtx) == -1 && errno == EINTR)
    {
    }
}

//освобождение общего мьютекса, отложенные сигналы приходят после него
void unlock(void)
{
    sem_post(sem_mtx);
    sigprocmask(SIG_SETMASK, &saved_mask, NULL);
}

//ждать семафор не дольше ms; вызывающий код проверяет свое условие в цикле
static void sem_wait_ms(sem_t *s, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    sem_timedwait(s, &ts);
}

//жив ли процесс: сигнал 0 ничего не посылает, только проверяет; зомби (завершился, но его
//еще не забрал родитель) kill тоже считает существующим, поэтому смотрим состояние в /proc
int pid_alive(pid_t pid)
{
    if (pid <= 0 || (kill(pid, 0) != 0 && errno != EPERM))
    {
        return 0;
    }
    char path[32];
    char buf[256];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return 1;
    }
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    char *st = strrchr(buf, ')'); //после имени программы в скобках идет буква состояния
    return !(st && st[1] == ' ' && st[2] == 'Z');
}

//обработчик SIGINT/SIGTERM
static void on_signal(int sig)
{
    (void)sig;
    ++interrupts;
}

//обработчик без SA_RESTART, чтобы ожидания прерывались сразу
void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

//=============================== разделяемая память ===============================

//открывает журнал; создатель обрезает его, остальные дописывают в конец
static void log_open(int truncate)
{
    if (P.log_path[0] && (log_fd = open(P.log_path, O_WRONLY | O_CREAT | O_APPEND | (truncate ? O_TRUNC : 0), 0644)) < 0)
    {
        die("не удалось открыть журнал %s: %s", P.log_path, strerror(errno));
    }
}

//отображает уже открытую разделяемую память, если она нужного размера
static shm_t *map_shm(int fd)
{
    struct stat st;
    shm_t *m = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(shm_t))
    {
        m = mmap(NULL, sizeof(shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    close(fd);
    return m == MAP_FAILED ? NULL : m;
}

//создает разделяемую память и семафоры; остатки упавшего запуска удаляет, живой запуск не трогает
void shm_create(void)
{
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd >= 0)
    {
        shm_t *old = map_shm(fd);
        pid_t busy = old && pid_alive(old->owner_pid) ? old->owner_pid : 0;
        if (old)
        {
            munmap(old, sizeof(shm_t));
        }
        if (busy)
        {
            die("гостиница уже работает (pid %d)", (int)busy);
        }
        shm_unlink(SHM_NAME);
    }
    fd = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || ftruncate(fd, sizeof(shm_t)) < 0 || !(g = map_shm(fd)))
    {
        die("не удалось создать разделяемую память: %s", strerror(errno));
    }
    owner = getpid();
    atexit(hotel_cleanup);
    g->p = P;
    clock_gettime(CLOCK_MONOTONIC, &g->t0);
    for (int r = 0; r < MAX_ROOMS; ++r)
    {
        g->room[r] = -1;
        g->freed_by[r] = -1;
    }
    sync_create();
    log_open(1);
    g->admin_pid = owner;
    g->owner_pid = owner; //последним: пока его нет, другие процессы считают гостиницу не готовой
}

//подключается к разделяемой памяти работающей гостиницы, -1 если ее нет
int shm_attach(void)
{
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd < 0 || !(g = map_shm(fd)) || !pid_alive(g->owner_pid))
    {
        return -1;
    }
    P = g->p;
    sync_attach();
    log_open(0);
    return 0;
}

//удаляет семафоры и разделяемую память; делает это только создатель
void hotel_cleanup(void)
{
    if (owner != getpid())
    {
        return;
    }
    owner = 0;
    sync_remove();
    munmap(g, sizeof(shm_t));
    shm_unlink(SHM_NAME);
    if (log_fd >= 0)
    {
        close(log_fd);
    }
}

//=============================== администратор ===============================

//сообщает о нарушении инварианта и завершает процесс
void violation(const char *what)
{
    event("НАРУШЕНИЕ ИНВАРИАНТА: %s", what);
    exit(3);
}

//проверка всех инвариантов, вызывать под lock()
void check_invariants(void)
{
    int busy = 0;
    for (int r = 0; r < P.rooms; ++r)
    {
        int s = g->room[r];
        if (s < 0)
        {
            continue;
        }
        ++busy;
        //у клиента одно поле room, поэтому два номера он занимать не может
        if (g->c[s].room != r || g->c[s].state != C_ASSIGNED)
        {
            violation("номер занят клиентом, которому он не назначен");
        }
    }
    if (busy != g->occupied || g->occupied > P.rooms)
    {
        violation("неверное число занятых номеров");
    }
    for (int i = 0; i < g->wait_n; ++i)
    {
        if (g->c[g->wait[i]].room != -1 || g->c[g->wait[i]].state != C_WAITING)
        {
            violation("клиент в очереди занимает номер");
        }
    }
    if (P.queue_cap >= 0 && g->wait_n > P.queue_cap)
    {
        violation("переполнена зона ожидания");
    }
}

//убирает значение v из массива a длины n
static void arr_remove(int *a, int *n, int v)
{
    for (int i = 0; i < *n; ++i)
    {
        if (a[i] == v)
        {
            memmove(a + i, a + i + 1, sizeof(int) * (size_t)(*n - i - 1));
            --*n;
            return;
        }
    }
}

//достает из очереди следующего по стратегии; старение: сутки ожидания повышают приоритет на 1,
//ожидание считается в дробных сутках, иначе порядок зависел бы от того, кто чуть раньше пересек границу суток
static int pick_waiting(void)
{
    int best = 0;
    double best_key = 0;
    for (int i = 0; i < g->wait_n && P.strat != 0; ++i)
    {
        client_t *c = &g->c[g->wait[i]];
        double key = (P.strat == 1 ? c->stay : -c->stay) - (double)(now_ms() - c->enqueue_ms) / P.day_ms;
        if (i == 0 || key < best_key)
        {
            best = i;
            best_key = key;
        }
    }
    int s = g->wait[best];
    arr_remove(g->wait, &g->wait_n, s);
    return s;
}

//первый свободный номер или -1
static int find_free_room(void)
{
    for (int r = 0; r < P.rooms; ++r)
    {
        if (g->room[r] < 0)
        {
            return r;
        }
    }
    return -1;
}

//назначает клиенту номер и будит его
static void assign(int s, int r)
{
    client_t *c = &g->c[s];
    if (g->room[r] >= 0 || c->room != -1)
    {
        violation("номер назначен, не будучи освобожденным");
    }
    event("Администратор: назначает клиенту %d номер %d", c->id, r + 1);
    g->room[r] = s;
    c->room = r;
    c->state = C_ASSIGNED;
    if (++g->occupied > g->max_occupied)
    {
        g->max_occupied = g->occupied;
    }
    check_invariants();
    sem_post(client_sem(s));
}

//рассматривает запрос нового клиента
static void handle_request(int s)
{
    client_t *c = &g->c[s];
    int r = find_free_room();
    //номер сразу даем только если никто не ждет, иначе новый клиент влез бы вперед очереди
    if (r >= 0 && g->wait_n == 0)
    {
        assign(s, r);
    }
    else if (P.queue_cap < 0 || g->wait_n < P.queue_cap)
    {
        c->state = C_WAITING;
        c->enqueue_ms = now_ms();
        g->wait[g->wait_n++] = s;
        if (g->wait_n > g->max_wait_len)
        {
            g->max_wait_len = g->wait_n;
        }
        event("Администратор: свободных номеров нет, клиент %d поставлен в очередь (в очереди %d)", c->id, g->wait_n);
        check_invariants();
    }
    else
    {
        c->state = C_REJECTED;
        event("Администратор: номеров нет и зона ожидания заполнена, клиенту %d отказано", c->id);
        sem_post(client_sem(s));
    }
}

//прекращает прием новых клиентов
void close_arrivals(const char *reason)
{
    if (g->arrivals_closed)
    {
        return;
    }
    g->arrivals_closed = 1;
    event("Прием новых клиентов прекращен (%s), прибыло %d. Ждем выезда проживающих и обработки очереди", reason, g->arrived);
    sem_post(sem_admin);
}

//убирает клиентов, чьи процессы завершились, не покинув гостиницу (прервали или убили)
static void reap_lost(void)
{
    for (int s = 0; s < MAX_SLOTS; ++s)
    {
        client_t *c = &g->c[s];
        if (c->state == C_FREE || pid_alive(c->pid) || c->pid <= 0)
        {
            continue;
        }
        arr_remove(g->req, &g->req_n, s);
        arr_remove(g->wait, &g->wait_n, s);
        if (c->room >= 0)
        {
            event("Администратор: процесс клиента %d (pid %d) прерван, номер %d освобожден", c->id, (int)c->pid, c->room + 1);
            g->room[c->room] = -1;
            g->freed_by[c->room] = c->id;
            --g->occupied;
        }
        else
        {
            event("Администратор: процесс клиента %d (pid %d) прерван до заселения", c->id, (int)c->pid);
        }
        ++g->lost;
        slot_free(s);
    }
}

//процесс администратора: раздает номера, ведет очередь, следит за клиентами
void admin_run(void)
{
    lock();
    g->admin_pid = getpid();
    event("Администратор (pid %d): начинает смену, номеров %d", (int)getpid(), P.rooms);
    for (;;)
    {
        if (interrupts >= 2)
        {
            event("Повторное прерывание: аварийное завершение");
            print_stats();
            unlock();
            exit(130);
        }
        if (interrupts == 1)
        {
            close_arrivals("по прерыванию");
        }
        if (P.duration && now_ms() >= (long)P.duration * P.day_ms)
        {
            close_arrivals("истекла продолжительность приема");
        }
        reap_lost();
        //освободившиеся номера сначала отдаем очереди
        int r;
        while (g->wait_n > 0 && (r = find_free_room()) >= 0)
        {
            int s = pick_waiting();
            long waited = now_ms() - g->c[s].enqueue_ms;
            g->total_wait_ms += waited;
            if (waited > g->max_wait_ms)
            {
                g->max_wait_ms = waited;
            }
            ++g->from_queue;
            event("Администратор: номер %d, освобожденный клиентом %d, передается клиенту %d из очереди (ждал %.1f ч)",
                r + 1, g->freed_by[r], g->c[s].id, hours(waited));
            assign(s, r);
        }
        if (g->req_n > 0)
        {
            int s = g->req[0];
            arr_remove(g->req, &g->req_n, s);
            handle_request(s);
            continue;
        }
        if (g->arrivals_closed && g->alive == 0)
        {
            break;
        }
        unlock();
        sem_wait_ms(sem_admin, 100); //просыпаемся и по таймеру: проверить время, сигналы и живость клиентов
        lock();
    }
    event("Администратор: все клиенты выехали, смена окончена");
    unlock();
}

//=============================== клиент ===============================

//занимает место для нового клиента, вызывать под lock(); -1 если мест нет
int slot_alloc(int stay)
{
    for (int s = 0; s < MAX_SLOTS; ++s)
    {
        client_t *c = &g->c[s];
        if (c->state == C_FREE)
        {
            memset(c, 0, sizeof *c);
            c->state = C_REQUESTED;
            c->id = ++g->next_id;
            c->stay = stay;
            c->room = -1;
            ++g->alive;
            return s;
        }
    }
    return -1;
}

//клиент покидает модель, вызывать под lock()
void slot_free(int s)
{
    g->c[s].state = C_FREE;
    g->c[s].pid = 0;
    --g->alive;
    sem_post(sem_admin);
}

//процесс клиента: запрос номера, проживание по суткам, выезд
void client_run(int s)
{
    client_t *c = &g->c[s];
    lock();
    c->pid = getpid();
    ++g->arrived;
    event("Клиент %d (pid %d): прибыл в гостиницу (срок %d сут.)", c->id, (int)c->pid, c->stay);
    event("Клиент %d: запрашивает одноместный номер", c->id);
    g->req[g->req_n++] = s;
    sem_post(sem_admin);
    while (c->state == C_REQUESTED || c->state == C_WAITING)
    {
        unlock();
        sem_wait_ms(client_sem(s), 500);
        lock();
        if (!pid_alive(g->admin_pid))
        {
            event("Клиент %d: администратор не отвечает, клиент уходит", c->id);
            slot_free(s);
            unlock();
            return;
        }
    }
    if (c->state == C_REJECTED)
    {
        ++g->rejected;
        event("Клиент %d: уходит без заселения", c->id);
        slot_free(s);
        unlock();
        return;
    }
    int r = c->room;
    event("Клиент %d: заселился в номер %d", c->id, r + 1);
    unlock();

    //каждые сутки отмечаются отдельно, спим без мьютекса
    for (int d = 1; d <= c->stay; ++d)
    {
        sleep_ms(P.day_ms);
        lock();
        c->days = d;
        ++g->room_days;
        event("Клиент %d: номер %d, прожиты сутки %d из %d", c->id, r + 1, d, c->stay);
        unlock();
    }

    lock();
    if (g->room[r] != s || c->days != c->stay)
    {
        violation("срок проживания учтен не полностью");
    }
    g->room[r] = -1;
    g->freed_by[r] = c->id;
    c->room = -1;
    --g->occupied;
    ++g->served;
    event("Клиент %d: выехал, номер %d свободен (ожидают %d)", c->id, r + 1, g->wait_n);
    check_invariants();
    slot_free(s);
    unlock();
}

//итоговая статистика, вызывать под lock()
void print_stats(void)
{
    long ms = now_ms();
    out("==================== ИТОГИ ====================\n");
    out("модельное время: %.1f сут.\n", (double)ms / P.day_ms);
    out("прибыло клиентов: %d, обслужено: %d, отказов: %d, прервано: %d, еще в гостинице: %d, в очереди: %d\n",
        g->arrived, g->served, g->rejected, g->lost, g->occupied, g->wait_n);
    out("прожито номеро-суток: %ld, загрузка номеров: %.0f%%\n", g->room_days,
        ms ? 100.0 * g->room_days * P.day_ms / ((double)ms * P.rooms) : 0.0);
    out("максимум занятых номеров: %d из %d, максимальная длина очереди: %d\n", g->max_occupied, P.rooms, g->max_wait_len);
    out("заселено из очереди: %d, ожидание в среднем %.1f ч, максимум %.1f ч\n", g->from_queue,
        g->from_queue ? hours(g->total_wait_ms) / g->from_queue : 0.0, hours(g->max_wait_ms));
}

//=============================== разбор параметров ===============================

//опции: буква, ключ в конфиге, строка справки
static const struct
{
    char o;
    const char *key;
    const char *help;
} OPTS[] = {
    {'r', "rooms", "-r N      количество номеров, не больше 64 (3)"},
    {'n', "clients", "-n N      количество клиентов, 0 - без ограничения (12)"},
    {'a', "arrival", "-a A-B    интервал между прибытиями, часы (2-14)"},
    {'s', "stay", "-s A-B    срок проживания, сутки, случайный (1-4)"},
    {'S', "stays", "-S L      сроки для каждого клиента явно, например 3,1,5"},
    {'q', "queue", "-q N      вместимость зоны ожидания, -1 - без ограничения (-1)"},
    {'p', "strategy", "-p S      стратегия заселения: fifo | short | long (fifo)"},
    {'d', "duration", "-d N      продолжительность приема клиентов, сутки, 0 - без ограничения (0)"},
    {'t', "delay", "-t N      длительность модельных суток в мс, не меньше 10 (300)"},
    {'x', "seed", "-x N      seed генератора (по умолчанию от времени)"},
    {'l', "log", "-l FILE   журнал событий и статистики"},
    {'c', NULL, "-c FILE   читать параметры из файла ключ=значение"},
};

//парсит целое и проверяет диапазон
static int parse_int(const char *key, const char *s, int lo, int hi)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi)
    {
        die("параметр %s: ожидается целое число от %d до %d, получено \"%s\"", key, lo, hi, s);
    }
    return (int)v;
}

//парсит диапазон вида A-B или одно число A
static void parse_range(const char *key, const char *s, int lo, int hi, int *a, int *b)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    char *dash = strchr(buf + 1, '-'); //с 1, чтобы не спутать с минусом
    if (dash)
    {
        *dash = '\0';
    }
    *a = parse_int(key, buf, lo, hi);
    *b = dash ? parse_int(key, dash + 1, lo, hi) : *a;
    if (*a > *b)
    {
        die("параметр %s: минимум больше максимума (%s)", key, s);
    }
}

//устанавливает параметр по имени
static void set_param(const char *key, const char *val)
{
    if (!strcmp(key, "rooms"))
    {
        P.rooms = parse_int(key, val, 1, MAX_ROOMS);
    }
    else if (!strcmp(key, "clients"))
    {
        P.clients = parse_int(key, val, 0, 1000000);
    }
    else if (!strcmp(key, "arrival"))
    {
        parse_range(key, val, 0, 24 * 365, &P.arr_min, &P.arr_max);
    }
    else if (!strcmp(key, "stay"))
    {
        parse_range(key, val, 1, 365, &P.stay_min, &P.stay_max);
    }
    else if (!strcmp(key, "queue"))
    {
        P.queue_cap = parse_int(key, val, -1, MAX_SLOTS);
    }
    else if (!strcmp(key, "duration"))
    {
        P.duration = parse_int(key, val, 0, 100000);
    }
    else if (!strcmp(key, "delay"))
    {
        P.day_ms = parse_int(key, val, 10, 600000);
    }
    else if (!strcmp(key, "seed"))
    {
        P.seed = strtoull(val, NULL, 10);
        P.seed_given = 1;
    }
    else if (!strcmp(key, "log"))
    {
        snprintf(P.log_path, sizeof P.log_path, "%s", val);
    }
    else if (!strcmp(key, "strategy"))
    {
        if (strcmp(val, "fifo") && strcmp(val, "short") && strcmp(val, "long"))
        {
            die("стратегия должна быть fifo, short или long, получено \"%s\"", val);
        }
        P.strat = val[0] == 'f' ? 0 : val[0] == 's' ? 1 : 2;
    }
    else if (!strcmp(key, "stays"))
    {
        char *copy = strdup(val);
        char *save = NULL;
        stays_len = 0;
        for (char *t = strtok_r(copy, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
        {
            stays = realloc(stays, sizeof(int) * (size_t)(stays_len + 1));
            stays[stays_len++] = parse_int("stays", t, 1, 365);
        }
        free(copy);
    }
    else
    {
        die("неизвестный параметр \"%s\"", key);
    }
}

//убирает пробелы по краям строки на месте
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
    {
        ++s;
    }
    char *e = s + strlen(s);
    while (e > s && strchr(" \t\r\n", e[-1]))
    {
        *--e = '\0';
    }
    return s;
}

//читает параметры из файла ключ=значение через open/read
static void load_config(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        die("не удалось открыть конфигурационный файл %s: %s", path, strerror(errno));
    }
    char text[65536];
    size_t len = 0;
    ssize_t n;
    while (len + 1 < sizeof text && (n = read(fd, text + len, sizeof text - 1 - len)) > 0)
    {
        len += (size_t)n;
    }
    close(fd);
    text[len] = '\0';
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        char *s = trim(line);
        if (!*s || *s == '#')
        {
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
        {
            die("%s: ожидается строка вида ключ=значение: \"%s\"", path, s);
        }
        *eq = '\0';
        set_param(trim(s), trim(eq + 1));
    }
}

//справка по опциям, которые принимает программа
static void usage(const char *prog, const char *opts, const char *about)
{
    out("%s\n\nИспользование: %s [опции]\n", about, prog);
    for (size_t i = 0; i < sizeof OPTS / sizeof OPTS[0]; ++i)
    {
        if (strchr(opts, OPTS[i].o))
        {
            out("  %s\n", OPTS[i].help);
        }
    }
    out("\nCtrl+C: первое - прием закрывается и все дообслуживаются, второе - немедленный выход\n");
}

//разбор аргументов: opts - строка getopt с опциями этой программы
void parse_args(int argc, char **argv, const char *opts, const char *about)
{
    int o;
    while ((o = getopt(argc, argv, opts)) != -1)
    {
        const char *key = NULL;
        for (size_t i = 0; i < sizeof OPTS / sizeof OPTS[0]; ++i)
        {
            if (OPTS[i].o == o)
            {
                key = OPTS[i].key;
            }
        }
        if (o == 'c')
        {
            load_config(optarg);
        }
        else if (key)
        {
            set_param(key, optarg);
        }
        else
        {
            usage(argv[0], opts, about);
            exit(o == 'h' ? 0 : 2);
        }
    }
    if (optind < argc)
    {
        die("лишний аргумент \"%s\"", argv[optind]);
    }
    if (!P.seed_given)
    {
        P.seed = (unsigned long long)time(NULL) ^ ((unsigned long long)getpid() << 16);
    }
    rng_state = P.seed;
    if (stays_len)
    {
        P.clients = P.clients && P.clients < stays_len ? P.clients : stays_len;
    }
    if (P.arr_max == 0 && P.clients == 0)
    {
        die("при нулевом интервале прибытия число клиентов должно быть ограничено");
    }
}
