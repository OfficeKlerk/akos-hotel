//задача 12 "гостиница" - многопоточная имитация
//потоки: администратор, каждый клиент, генератор прибытий (main) и поток приема сигналов

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "hotel.h"

//коды завершения
#define EXIT_BAD_ARGS 2
#define EXIT_INVARIANT 3
#define EXIT_INTERRUPTED 130

//=============================== параметры ===============================

//стратегия выбора клиента из очереди
typedef enum
{
    STRAT_FIFO,
    STRAT_SHORT,
    STRAT_LONG
} strategy_t;

//параметры моделирования
typedef struct
{
    //гостиница
    int rooms;
    int queue_cap; //-1 - без ограничения
    strategy_t strat;

    //клиенты
    int clients; //0 - без ограничения
    int clients_given;
    int arr_min; //интервал прибытия в часах
    int arr_max;
    int stay_min; //срок проживания в сутках
    int stay_max;
    int *stays; //явные сроки для каждого клиента
    int stays_len;

    //время
    int duration; //продолжительность приема клиентов в сутках, 0 - без ограничения
    int day_ms; //сколько мс реального времени длятся модельные сутки

    //прочее
    uint64_t seed;
    int seed_given;
    char *log_path;
} params_t;

//текущие параметры со значениями по умолчанию
static params_t P = {
    .rooms = 3,
    .queue_cap = -1,
    .strat = STRAT_FIFO,
    .clients = 12,
    .arr_min = 2,
    .arr_max = 14,
    .stay_min = 1,
    .stay_max = 4,
    .day_ms = 300,
};

//=============================== вывод ===============================

//дескриптор файла журнала, -1 если журнал не ведется
static int log_fd = -1;

//момент запуска модели
static struct timespec t_start;

//записывает весь буфер в дескриптор, write может записать не все сразу или прерваться сигналом
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

//форматирует строку и пишет ее на экран и в журнал, вызывать под mtx
static void out(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }
    if ((size_t)n >= sizeof buf)
    {
        n = sizeof buf - 1;
    }
    write_all(STDOUT_FILENO, buf, (size_t)n);
    if (log_fd >= 0)
    {
        write_all(log_fd, buf, (size_t)n);
    }
}

//печатает ошибку в параметрах и завершает программу
static void die(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dprintf(STDERR_FILENO, "Ошибка: %s\n", buf);
    exit(EXIT_BAD_ARGS);
}

//сколько мс прошло с запуска модели
static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t_start.tv_sec) * 1000L + (t.tv_nsec - t_start.tv_nsec) / 1000000L;
}

//сон на ms миллисекунд, досыпает после прерывания сигналом
static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
    {
    }
}

//мс реального времени в модельные часы
static double ms_to_hours(long ms)
{
    return (double)ms * 24.0 / P.day_ms;
}

//пишет событие с модельным временем "[сутки N ЧЧ:ММ]", вызывать под mtx
static void event(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    long ms = now_ms();
    long min = (ms % P.day_ms) * 24 * 60 / P.day_ms;
    out("[сутки %3ld %02ld:%02ld] %s\n", ms / P.day_ms + 1, min / 60, min % 60, buf);
}

//=============================== случайные числа ===============================

//состояние генератора splitmix64, используется только в main
static uint64_t rng_state;

//случайное число от lo до hi включительно
static int rng_range(int lo, int hi)
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return lo + (int)(z % (uint64_t)(hi - lo + 1));
}

//=============================== состояние гостиницы ===============================

pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

//гостиница, все поля под mtx
static struct
{
    client_t **room; //room[r] - кто занимает номер r
    int *freed_by; //кто последним освободил номер r, -1 если никто
    int occupied;
    client_t *req_head; //запросы, которые еще не рассмотрел администратор
    client_t *req_tail;
    client_t *wait_head; //очередь ожидания
    client_t *wait_tail;
    int wait_len;

    int in_flight; //поток клиента создан, но запрос еще не подан
    int alive; //клиенты, еще не покинувшие модель
    int arrivals_closed; //прием новых клиентов прекращен
    int stop; //пользователь нажал Ctrl+C

    //статистика
    int arrived;
    int served;
    int rejected;
    int max_wait_len;
    int max_occupied;
    int from_queue;
    long room_days;
    long total_wait_ms;
    long max_wait_ms;
} H;

//сообщает о нарушении инварианта и завершает программу
static void violation(const char *what)
{
    event("НАРУШЕНИЕ ИНВАРИАНТА: %s", what);
    exit(EXIT_INVARIANT);
}

//проверка всех инвариантов, вызывается под mtx после каждого изменения
static void check_invariants(void)
{
    int busy = 0;
    for (int r = 0; r < P.rooms; ++r)
    {
        client_t *c = H.room[r];
        if (!c)
        {
            continue;
        }
        ++busy;
        //у клиента одно поле room, поэтому два номера он занимать не может
        if (c->room != r || c->state != C_ASSIGNED)
        {
            violation("номер занят клиентом, которому он не назначен");
        }
    }
    if (busy != H.occupied || H.occupied > P.rooms)
    {
        violation("неверное число занятых номеров");
    }
    for (client_t *c = H.wait_head; c; c = c->next)
    {
        if (c->room != -1 || c->state != C_WAITING)
        {
            violation("клиент в очереди занимает номер");
        }
    }
    if (P.queue_cap >= 0 && H.wait_len > P.queue_cap)
    {
        violation("переполнена зона ожидания");
    }
}

//добавляет клиента в конец списка
static void push(client_t **head, client_t **tail, client_t *c)
{
    c->next = NULL;
    if (*tail)
    {
        (*tail)->next = c;
    }
    else
    {
        *head = c;
    }
    *tail = c;
}

//вырезает клиента c из списка, prev - предыдущий элемент или NULL
static void unlink_client(client_t **head, client_t **tail, client_t *prev, client_t *c)
{
    if (prev)
    {
        prev->next = c->next;
    }
    else
    {
        *head = c->next;
    }
    if (*tail == c)
    {
        *tail = prev;
    }
    c->next = NULL;
}

//=============================== администратор ===============================

//достает из очереди следующего клиента по стратегии
static client_t *pick_waiting(void)
{
    client_t *best = H.wait_head;
    client_t *best_prev = NULL;
    long best_key = 0;
    client_t *prev = NULL;
    for (client_t *c = H.wait_head; c && P.strat != STRAT_FIFO; prev = c, c = c->next)
    {
        //старение: каждые сутки ожидания повышают приоритет на 1, чтобы никто не ждал вечно
        long aged = (now_ms() - c->enqueue_ms) / P.day_ms;
        long key = (P.strat == STRAT_SHORT ? c->stay : -c->stay) - aged;
        if (c == H.wait_head || key < best_key)
        {
            best = c;
            best_prev = prev;
            best_key = key;
        }
    }
    unlink_client(&H.wait_head, &H.wait_tail, best_prev, best);
    --H.wait_len;
    return best;
}

//первый свободный номер или -1
static int find_free_room(void)
{
    for (int r = 0; r < P.rooms; ++r)
    {
        if (!H.room[r])
        {
            return r;
        }
    }
    return -1;
}

//назначает клиенту номер r и будит его
static void assign(client_t *c, int r)
{
    if (H.room[r] || c->room != -1)
    {
        violation("номер назначен, не будучи освобожденным");
    }
    event("Администратор: назначает клиенту %d номер %d", c->id, r + 1);
    H.room[r] = c;
    c->room = r;
    c->state = C_ASSIGNED;
    if (++H.occupied > H.max_occupied)
    {
        H.max_occupied = H.occupied;
    }
    check_invariants();
    wake_client(c);
}

//рассматривает запрос нового клиента
static void handle_request(client_t *c)
{
    int r = find_free_room();
    //номер сразу даем только если никто не ждет, иначе новый клиент влез бы вперед очереди
    if (r >= 0 && !H.wait_head)
    {
        assign(c, r);
    }
    else if (P.queue_cap < 0 || H.wait_len < P.queue_cap)
    {
        c->state = C_WAITING;
        c->enqueue_ms = now_ms();
        push(&H.wait_head, &H.wait_tail, c);
        if (++H.wait_len > H.max_wait_len)
        {
            H.max_wait_len = H.wait_len;
        }
        event("Администратор: свободных номеров нет, клиент %d поставлен в очередь (в очереди %d)", c->id, H.wait_len);
        check_invariants();
    }
    else
    {
        c->state = C_REJECTED;
        event("Администратор: номеров нет и зона ожидания заполнена, клиенту %d отказано", c->id);
        wake_client(c);
    }
}

//есть ли работа у администратора
static int admin_has_work(void)
{
    return H.req_head || (H.wait_head && H.occupied < P.rooms);
}

//можно ли администратору заканчивать: in_flight нужен, потому что поток клиента
//мог быть уже создан, но еще не успеть подать запрос
static int admin_may_finish(void)
{
    return H.arrivals_closed && H.in_flight == 0 && !H.req_head && !H.wait_head;
}

//поток администратора: раздает номера и ведет очередь
static void *admin_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mtx);
    event("Администратор: начинает смену, номеров %d", P.rooms);
    for (;;)
    {
        //условие проверяем в цикле: пробуждение еще не значит, что работа есть
        while (!admin_has_work() && !admin_may_finish())
        {
            wait_admin();
        }
        if (!admin_has_work())
        {
            break;
        }
        //сначала освободившиеся номера отдаем очереди
        int r;
        while (H.wait_head && (r = find_free_room()) >= 0)
        {
            client_t *c = pick_waiting();
            long waited = now_ms() - c->enqueue_ms;
            H.total_wait_ms += waited;
            if (waited > H.max_wait_ms)
            {
                H.max_wait_ms = waited;
            }
            ++H.from_queue;
            event("Администратор: номер %d, освобожденный клиентом %d, передается клиенту %d из очереди (ждал %.1f ч)",
                r + 1, H.freed_by[r], c->id, ms_to_hours(waited));
            assign(c, r);
        }
        //потом по одному новые запросы
        if (H.req_head)
        {
            client_t *c = H.req_head;
            unlink_client(&H.req_head, &H.req_tail, NULL, c);
            handle_request(c);
        }
    }
    event("Администратор: новых клиентов нет, очередь пуста, заканчивает смену");
    pthread_mutex_unlock(&mtx);
    return NULL;
}

//=============================== клиент ===============================

//клиент покидает модель, вызывать под mtx
static void client_leaves(client_t *c)
{
    --H.alive;
    sync_client_free(c);
    free(c);
    wake_main();
    wake_admin(); //номер мог освободиться, пора пригласить следующего
}

//поток клиента: запрос номера, проживание по суткам, выезд
static void *client_thread(void *arg)
{
    client_t *c = arg;
    pthread_mutex_lock(&mtx);
    ++H.arrived;
    event("Клиент %d: прибыл в гостиницу (срок %d сут.)", c->id, c->stay);
    event("Клиент %d: запрашивает одноместный номер", c->id);
    push(&H.req_head, &H.req_tail, c);
    --H.in_flight;
    wake_admin();
    wake_main();

    while (c->state == C_REQUESTED || c->state == C_WAITING)
    {
        wait_client(c);
    }
    if (c->state == C_REJECTED)
    {
        ++H.rejected;
        event("Клиент %d: уходит без заселения", c->id);
        client_leaves(c);
        pthread_mutex_unlock(&mtx);
        return NULL;
    }
    int r = c->room;
    event("Клиент %d: заселился в номер %d", c->id, r + 1);
    pthread_mutex_unlock(&mtx);

    //каждые сутки отмечаются отдельно, спим без мьютекса
    for (int d = 1; d <= c->stay; ++d)
    {
        sleep_ms(P.day_ms);
        pthread_mutex_lock(&mtx);
        c->days = d;
        ++H.room_days;
        event("Клиент %d: номер %d, прожиты сутки %d из %d", c->id, r + 1, d, c->stay);
        pthread_mutex_unlock(&mtx);
    }

    pthread_mutex_lock(&mtx);
    if (H.room[r] != c || c->days != c->stay)
    {
        violation("срок проживания учтен не полностью");
    }
    H.room[r] = NULL;
    H.freed_by[r] = c->id;
    --H.occupied;
    ++H.served;
    event("Клиент %d: выехал, номер %d свободен (ожидают %d)", c->id, r + 1, H.wait_len);
    check_invariants();
    client_leaves(c);
    pthread_mutex_unlock(&mtx);
    return NULL;
}

//=============================== статистика и сигналы ===============================

//итоговая статистика, вызывать под mtx
static void print_stats(void)
{
    long ms = now_ms();
    out("==================== ИТОГИ ====================\n");
    out("модельное время: %.1f сут.\n", (double)ms / P.day_ms);
    out("прибыло клиентов: %d, обслужено: %d, отказов: %d, еще в гостинице: %d, в очереди: %d\n",
        H.arrived, H.served, H.rejected, H.occupied, H.wait_len);
    out("прожито номеро-суток: %ld, загрузка номеров: %.0f%%\n", H.room_days,
        ms ? 100.0 * H.room_days * P.day_ms / ((double)ms * P.rooms) : 0.0);
    out("максимум занятых номеров: %d из %d, максимальная длина очереди: %d\n", H.max_occupied, P.rooms, H.max_wait_len);
    out("заселено из очереди: %d, ожидание в среднем %.1f ч, максимум %.1f ч\n", H.from_queue,
        H.from_queue ? ms_to_hours(H.total_wait_ms) / H.from_queue : 0.0, ms_to_hours(H.max_wait_ms));
}

//сигналы, которые принимает signal_thread
static sigset_t sigs;

//поток сигналов: SIGINT заблокирован во всех потоках и принимается через sigwait,
//поэтому здесь можно брать мьютекс и печатать, в обычном обработчике это запрещено
static void *signal_thread(void *arg)
{
    (void)arg;
    int sig;
    for (;;)
    {
        if (sigwait(&sigs, &sig) != 0)
        {
            continue;
        }
        pthread_mutex_lock(&mtx);
        if (!H.stop)
        {
            //первое прерывание меняет условие завершения: прием закрывается, остальные дообслуживаются
            H.stop = 1;
            event("Прерывание: прием новых клиентов прекращается, проживающие досиживают сроки. Повторное Ctrl+C - немедленный выход");
            pthread_mutex_unlock(&mtx);
            continue;
        }
        event("Повторное прерывание: аварийное завершение");
        print_stats();
        _exit(EXIT_INTERRUPTED);
    }
    return NULL;
}

//=============================== разбор параметров ===============================

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
        P.rooms = parse_int(key, val, 1, 10000);
    }
    else if (!strcmp(key, "clients"))
    {
        P.clients = parse_int(key, val, 0, 1000000);
        P.clients_given = 1;
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
        P.queue_cap = parse_int(key, val, -1, 1000000);
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
        P.log_path = strdup(val);
    }
    else if (!strcmp(key, "strategy"))
    {
        if (!strcmp(val, "fifo") || !strcmp(val, "short") || !strcmp(val, "long"))
        {
            P.strat = val[0] == 'f' ? STRAT_FIFO : val[0] == 's' ? STRAT_SHORT : STRAT_LONG;
        }
        else
        {
            die("стратегия должна быть fifo, short или long, получено \"%s\"", val);
        }
    }
    else if (!strcmp(key, "stays"))
    {
        char *copy = strdup(val);
        char *save = NULL;
        P.stays_len = 0;
        for (char *t = strtok_r(copy, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
        {
            P.stays = realloc(P.stays, sizeof(int) * (size_t)(P.stays_len + 1));
            P.stays[P.stays_len++] = parse_int("stays", t, 1, 365);
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

//справка по опциям
static void usage(const char *prog)
{
    printf("Модель гостиницы (задача 12), многопоточная версия: %s\n\n"
           "Использование: %s [опции]\n"
           "  -r N      количество номеров (3)\n"
           "  -n N      количество клиентов, 0 - без ограничения (12)\n"
           "  -a A-B    интервал между прибытиями, часы (2-14)\n"
           "  -s A-B    срок проживания, сутки, случайный (1-4)\n"
           "  -S L      сроки для каждого клиента явно, например 3,1,5\n"
           "  -q N      вместимость зоны ожидания, -1 - без ограничения (-1)\n"
           "  -p S      стратегия заселения: fifo | short | long (fifo)\n"
           "  -d N      продолжительность приема клиентов, сутки, 0 - без ограничения (0)\n"
           "  -t N      длительность модельных суток в мс, не меньше 10 (300)\n"
           "  -x N      seed генератора (по умолчанию от времени)\n"
           "  -l FILE   сохранить журнал и статистику в файл\n"
           "  -c FILE   читать параметры из файла ключ=значение\n\n"
           "Ctrl+C: первое - прием закрывается и все дообслуживаются, второе - немедленный выход\n",
        sync_name, prog);
}

//разбор аргументов командной строки и проверка параметров
static void parse_args(int argc, char **argv)
{
    //имя параметра для каждой буквы опции
    static const char *names[128] = {['r'] = "rooms", ['n'] = "clients", ['a'] = "arrival", ['s'] = "stay",
        ['S'] = "stays", ['q'] = "queue", ['p'] = "strategy", ['d'] = "duration", ['t'] = "delay",
        ['x'] = "seed", ['l'] = "log"};
    int o;
    while ((o = getopt(argc, argv, "r:n:a:s:S:q:p:d:t:x:l:c:h")) != -1)
    {
        if (o == 'c')
        {
            load_config(optarg);
        }
        else if (o > 0 && o < 128 && names[o])
        {
            set_param(names[o], optarg);
        }
        else
        {
            usage(argv[0]);
            exit(o == 'h' ? 0 : EXIT_BAD_ARGS);
        }
    }
    if (optind < argc)
    {
        die("лишний аргумент \"%s\"", argv[optind]);
    }
    if (!P.seed_given)
    {
        P.seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 16);
    }
    if (P.stays)
    {
        if (!P.clients_given || P.clients == 0)
        {
            P.clients = P.stays_len;
        }
        else if (P.clients > P.stays_len)
        {
            die("задано %d клиентов, но сроков в stays только %d", P.clients, P.stays_len);
        }
    }
    if (P.arr_max == 0 && P.clients == 0)
    {
        die("при нулевом интервале прибытия число клиентов должно быть ограничено");
    }
}

//=============================== запуск ===============================

//запуск: потоки администратора и сигналов, генерация клиентов, ожидание конца моделирования
int hotel_run(int argc, char **argv)
{
    parse_args(argc, argv);
    rng_state = P.seed;
    if (P.log_path && (log_fd = open(P.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
    {
        die("не удалось открыть файл журнала %s: %s", P.log_path, strerror(errno));
    }
    H.room = calloc((size_t)P.rooms, sizeof *H.room);
    H.freed_by = malloc(sizeof(int) * (size_t)P.rooms);
    for (int r = 0; r < P.rooms; ++r)
    {
        H.freed_by[r] = -1;
    }
    sync_init();

    //сигналы блокируем до создания потоков, чтобы маска унаследовалась;
    //диспозицию сбрасываем, иначе запущенный из скрипта в фоне процесс игнорировал бы SIGINT
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    out("Синхронизация: %s\n", sync_name);
    out("Номеров %d, клиентов %d, прибытие каждые %d-%d ч, очередь %d, прием %d сут., сутки = %d мс, seed %llu\n",
        P.rooms, P.clients, P.arr_min, P.arr_max, P.queue_cap, P.duration, P.day_ms, (unsigned long long)P.seed);
    out("(0 клиентов, 0 суток приема и очередь -1 - без ограничения)\n");
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    pthread_t tid;
    pthread_create(&tid, NULL, signal_thread, NULL);
    pthread_detach(tid);
    pthread_t admin;
    pthread_create(&admin, NULL, admin_thread, NULL);

    //потоки клиентов отсоединенные, их уход main отслеживает по счетчику alive
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 128 * 1024);

    pthread_mutex_lock(&mtx);
    long end_ms = (long)P.duration * P.day_ms;
    const char *reason = "прибыли все клиенты";
    for (int i = 0; P.clients == 0 || i < P.clients; ++i)
    {
        //ждем до следующего прибытия кусками, чтобы сразу заметить Ctrl+C и конец приема
        long target = now_ms() + (long)rng_range(P.arr_min, P.arr_max) * P.day_ms / 24;
        while (!H.stop && now_ms() < target && !(end_ms && now_ms() >= end_ms))
        {
            pthread_mutex_unlock(&mtx);
            sleep_ms(5);
            pthread_mutex_lock(&mtx);
        }
        if (H.stop || (end_ms && now_ms() >= end_ms))
        {
            reason = H.stop ? "по прерыванию" : "истекла продолжительность приема";
            break;
        }
        client_t *c = calloc(1, sizeof *c);
        c->id = i + 1;
        c->stay = P.stays ? P.stays[i] : rng_range(P.stay_min, P.stay_max);
        c->room = -1;
        sync_client_init(c);
        ++H.in_flight;
        ++H.alive;
        if (pthread_create(&tid, &attr, client_thread, c) != 0)
        {
            --H.in_flight;
            --H.alive;
            sync_client_free(c);
            free(c);
            reason = "не удалось создать поток клиента";
            break;
        }
    }
    //даем последним прибывшим подать запрос, потом закрываем прием
    while (H.in_flight > 0)
    {
        wait_main();
    }
    H.arrivals_closed = 1;
    event("Прием новых клиентов прекращен (%s), прибыло %d. Ждем выезда проживающих и обработки очереди", reason, H.arrived);
    wake_admin();
    pthread_mutex_unlock(&mtx);
    pthread_join(admin, NULL);

    pthread_mutex_lock(&mtx);
    while (H.alive > 0)
    {
        wait_main();
    }
    if (H.occupied || H.wait_head || H.req_head)
    {
        violation("моделирование закончилось, а в гостинице остались клиенты");
    }
    event("Все клиенты выехали, очередь пуста, моделирование завершено");
    print_stats();
    if (log_fd >= 0)
    {
        close(log_fd);
    }
    pthread_mutex_unlock(&mtx);
    return 0;
}
