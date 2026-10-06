//задача 12 "гостиница" - последовательная (дискретно-событийная) имитация
//все сущности модели (гостиница, администратор, клиенты) - это типы данных и функции,
//а их взаимодействие происходит через события, которые обрабатываются по порядку модельного времени

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

//модельное время считаем в минутах
#define MIN_PER_HOUR 60L
#define MIN_PER_DAY (24L * 60L)

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
    int day_ms; //задержка отображения: сколько мс реального времени длятся модельные сутки

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
    .duration = 0,
    .day_ms = 300,
};

//название стратегии для вывода
static const char *strat_name(strategy_t s)
{
    if (s == STRAT_SHORT)
    {
        return "short (сначала короткие сроки)";
    }
    if (s == STRAT_LONG)
    {
        return "long (сначала длинные сроки)";
    }
    return "fifo (в порядке очереди)";
}

//=============================== вывод ===============================

//дескриптор файла журнала, -1 если журнал не ведется
static int log_fd = -1;

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

//форматирует строку и пишет ее на экран и в журнал
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

//пишет сообщение об ошибке в stderr
static void err(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        write_all(STDERR_FILENO, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
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
    err("Ошибка: %s\n", buf);
    exit(EXIT_BAD_ARGS);
}

//текущее модельное время в минутах
static long now_t = 0;

//пишет событие с отметкой модельного времени "[сутки N ЧЧ:ММ]"
static void event(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    long day = now_t / MIN_PER_DAY + 1;
    long min = now_t % MIN_PER_DAY;
    out("[сутки %3ld %02ld:%02ld] %s\n", day, min / 60, min % 60, buf);
}

//=============================== случайные числа ===============================

//состояние генератора, используем splitmix64: rand() на разных libc дает разные
//последовательности, и одинаковый seed давал бы разные прогоны
static uint64_t rng_state;

//следующее случайное 64-битное число
static uint64_t rng_next(void)
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

//случайное число от lo до hi включительно
static int rng_range(int lo, int hi)
{
    return lo + (int)(rng_next() % (uint64_t)(hi - lo + 1));
}

//=============================== сущности модели ===============================

//состояние клиента
typedef enum
{
    C_REQUESTING, //прибыл и обращается к администратору
    C_WAITING, //стоит в очереди
    C_ASSIGNED, //номер назначен, но еще не заселился
    C_LIVING, //проживает
    C_GONE //покинул гостиницу
} cstate_t;

//клиент гостиницы
typedef struct client
{
    int id;
    int stay; //заказанный срок в сутках
    int days; //сколько суток уже прожито
    int room; //номер комнаты или -1
    cstate_t state;
    long arrive_t; //время прибытия
    long enqueue_t; //время постановки в очередь
    struct client *next; //следующий в очереди ожидания
} client_t;

//гостиница: номера и зона ожидания
typedef struct
{
    client_t **room; //room[r] - кто занимает номер r, NULL если свободен
    int *freed_by; //кто последним освободил номер r, -1 если никто
    int occupied; //сколько номеров занято
    client_t *wait_head; //очередь ожидания
    client_t *wait_tail;
    int wait_len;
    int inside; //сколько клиентов сейчас в модели (прибыли и еще не ушли)
} hotel_t;

//администратор: принимает решения о заселении и ведет учет
typedef struct
{
    int arrivals_closed; //прием новых клиентов прекращен
    int next_id; //номер следующего клиента

    //статистика
    int arrived;
    int served;
    int rejected;
    int max_wait_len;
    int max_occupied;
    long room_days; //сколько номеро-суток прожито всеми клиентами
    long total_wait; //суммарное ожидание в очереди, минуты
    long max_wait;
    int from_queue; //сколько клиентов заселено из очереди
} admin_t;

//гостиница
static hotel_t H;

//администратор
static admin_t A;

//=============================== события ===============================

//типы событий
typedef enum
{
    EV_ARRIVAL, //прибытие очередного клиента
    EV_REQUEST, //клиент запрашивает номер
    EV_CHECKIN, //клиент заселяется в назначенный номер
    EV_DAY_END, //у клиента закончились очередные сутки
    EV_CHECKOUT, //клиент выезжает
    EV_STOP_ARRIVALS //истекла продолжительность приема клиентов
} ev_type_t;

//событие модели
typedef struct
{
    long time; //модельное время события
    long seq; //порядковый номер: события в одно время обрабатываются в порядке создания
    ev_type_t type;
    client_t *c; //клиент, к которому относится событие
} event_t;

//очередь событий - двоичная куча по (time, seq)
static struct
{
    event_t *a;
    int len;
    int cap;
    long seq;
} Q;

//true, если событие x должно произойти раньше y
static int ev_less(const event_t *x, const event_t *y)
{
    if (x->time != y->time)
    {
        return x->time < y->time;
    }
    return x->seq < y->seq;
}

//добавляет событие в очередь
static void schedule(long time, ev_type_t type, client_t *c)
{
    if (Q.len == Q.cap)
    {
        Q.cap = Q.cap ? Q.cap * 2 : 64;
        Q.a = realloc(Q.a, sizeof(event_t) * (size_t)Q.cap);
        if (!Q.a)
        {
            die("недостаточно памяти");
        }
    }
    event_t e = {time, Q.seq++, type, c};
    //просеиваем новый элемент вверх
    int i = Q.len++;
    while (i > 0 && ev_less(&e, &Q.a[(i - 1) / 2]))
    {
        Q.a[i] = Q.a[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    Q.a[i] = e;
}

//достает ближайшее событие из очереди
static event_t pop_event(void)
{
    event_t top = Q.a[0];
    event_t last = Q.a[--Q.len];
    //просеиваем последний элемент вниз от корня
    int i = 0;
    for (;;)
    {
        int l = 2 * i + 1;
        if (l >= Q.len)
        {
            break;
        }
        int m = l;
        if (l + 1 < Q.len && ev_less(&Q.a[l + 1], &Q.a[l]))
        {
            m = l + 1;
        }
        if (!ev_less(&Q.a[m], &last))
        {
            break;
        }
        Q.a[i] = Q.a[m];
        i = m;
    }
    if (Q.len > 0)
    {
        Q.a[i] = last;
    }
    return top;
}

//=============================== инварианты ===============================

//сообщает о нарушении инварианта и завершает программу
static void violation(const char *what)
{
    event("НАРУШЕНИЕ ИНВАРИАНТА: %s", what);
    err("НАРУШЕНИЕ ИНВАРИАНТА: %s\n", what);
    exit(EXIT_INVARIANT);
}

//проверка всех инвариантов, вызывается после каждого события
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
        //у клиента одно поле room, и оно должно указывать на этот номер,
        //значит один клиент не может занимать два номера
        if (c->room != r)
        {
            violation("клиент записан не в тот номер, который занимает");
        }
        if (c->state != C_ASSIGNED && c->state != C_LIVING)
        {
            violation("номер занят клиентом, которому он не назначен");
        }
    }
    if (busy != H.occupied)
    {
        violation("счетчик занятых номеров не совпадает с фактом");
    }
    if (H.occupied > P.rooms)
    {
        violation("занято больше номеров, чем есть в гостинице");
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

//=============================== администратор ===============================

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

//ставит клиента в конец очереди ожидания
static void queue_push(client_t *c)
{
    c->next = NULL;
    if (H.wait_tail)
    {
        H.wait_tail->next = c;
    }
    else
    {
        H.wait_head = c;
    }
    H.wait_tail = c;
    ++H.wait_len;
}

//достает из очереди следующего клиента по стратегии
static client_t *queue_pick(void)
{
    client_t *best = H.wait_head;
    client_t *best_prev = NULL;
    if (P.strat != STRAT_FIFO)
    {
        long best_key = 0;
        client_t *prev = NULL;
        for (client_t *c = H.wait_head; c; prev = c, c = c->next)
        {
            //старение: каждые сутки ожидания повышают приоритет на 1,
            //иначе при бесконечном потоке клиентов кто-то мог бы ждать вечно
            long aged = (now_t - c->enqueue_t) / MIN_PER_DAY;
            long key = (P.strat == STRAT_SHORT ? c->stay : -c->stay) - aged;
            if (c == H.wait_head || key < best_key)
            {
                best = c;
                best_prev = prev;
                best_key = key;
            }
        }
    }
    //вырезаем выбранного из списка
    if (best_prev)
    {
        best_prev->next = best->next;
    }
    else
    {
        H.wait_head = best->next;
    }
    if (H.wait_tail == best)
    {
        H.wait_tail = best_prev;
    }
    best->next = NULL;
    --H.wait_len;
    return best;
}

//назначает клиенту номер r, заселение происходит отдельным событием клиента
static void admin_assign(client_t *c, int r)
{
    if (H.room[r])
    {
        violation("номер назначен, не будучи освобожденным");
    }
    if (c->room != -1)
    {
        violation("клиенту назначается второй номер");
    }
    H.room[r] = c;
    c->room = r;
    c->state = C_ASSIGNED;
    ++H.occupied;
    if (H.occupied > A.max_occupied)
    {
        A.max_occupied = H.occupied;
    }
    schedule(now_t, EV_CHECKIN, c);
}

//администратор обрабатывает запрос клиента на номер
static void admin_on_request(client_t *c)
{
    int r = find_free_room();
    //номер сразу даем только если никто не ждет, иначе новый клиент влез бы вперед очереди
    if (r >= 0 && !H.wait_head)
    {
        event("Администратор: назначает клиенту %d свободный номер %d", c->id, r + 1);
        admin_assign(c, r);
        return;
    }
    if (P.queue_cap < 0 || H.wait_len < P.queue_cap)
    {
        c->state = C_WAITING;
        c->enqueue_t = now_t;
        queue_push(c);
        if (H.wait_len > A.max_wait_len)
        {
            A.max_wait_len = H.wait_len;
        }
        if (P.queue_cap < 0)
        {
            event("Администратор: свободных номеров нет, клиент %d поставлен в очередь (в очереди %d)",
                c->id, H.wait_len);
        }
        else
        {
            event("Администратор: свободных номеров нет, клиент %d поставлен в очередь (в очереди %d из %d мест)",
                c->id, H.wait_len, P.queue_cap);
        }
        return;
    }
    //ждать негде - клиент уходит
    event("Администратор: номеров нет и зона ожидания заполнена (%d/%d), клиенту %d отказано",
        H.wait_len, P.queue_cap, c->id);
    event("Клиент %d: уходит без заселения", c->id);
    c->state = C_GONE;
    ++A.rejected;
    --H.inside;
    free(c);
}

//администратор узнал, что номер r освободился, и приглашает следующего из очереди
static void admin_on_room_freed(int r)
{
    if (!H.wait_head || H.room[r])
    {
        return;
    }
    client_t *c = queue_pick();
    long waited = now_t - c->enqueue_t;
    A.total_wait += waited;
    if (waited > A.max_wait)
    {
        A.max_wait = waited;
    }
    ++A.from_queue;
    event("Администратор: номер %d, освобожденный клиентом %d, передается клиенту %d из очереди "
          "(ждал %.1f ч, в очереди осталось %d)",
        r + 1, H.freed_by[r], c->id, (double)waited / MIN_PER_HOUR, H.wait_len);
    event("Администратор: назначает клиенту %d номер %d", c->id, r + 1);
    admin_assign(c, r);
}

//прекращает прием новых клиентов
static void admin_close_arrivals(const char *reason)
{
    if (A.arrivals_closed)
    {
        return;
    }
    A.arrivals_closed = 1;
    event("Прием новых клиентов прекращен (%s), прибыло %d. Ждем выезда проживающих и обработки очереди",
        reason, A.arrived);
}

//=============================== клиенты ===============================

//прибытие клиента: создаем его и планируем следующее прибытие
static void on_arrival(void)
{
    if (A.arrivals_closed)
    {
        return;
    }
    client_t *c = calloc(1, sizeof *c);
    if (!c)
    {
        die("недостаточно памяти");
    }
    int i = A.next_id++;
    c->id = i + 1;
    c->stay = P.stays ? P.stays[i] : rng_range(P.stay_min, P.stay_max);
    c->room = -1;
    c->state = C_REQUESTING;
    c->arrive_t = now_t;
    ++A.arrived;
    ++H.inside;
    event("Клиент %d: прибыл в гостиницу (желаемый срок %d сут.)", c->id, c->stay);
    schedule(now_t, EV_REQUEST, c);

    //следующий клиент
    if (P.clients > 0 && A.next_id >= P.clients)
    {
        admin_close_arrivals("прибыли все клиенты");
        return;
    }
    long next = now_t + (long)rng_range(P.arr_min, P.arr_max) * MIN_PER_HOUR;
    schedule(next, EV_ARRIVAL, NULL);
}

//клиент обращается к администратору за номером
static void on_request(client_t *c)
{
    event("Клиент %d: запрашивает одноместный номер у администратора", c->id);
    admin_on_request(c);
}

//клиент заселяется в назначенный номер
static void on_checkin(client_t *c)
{
    c->state = C_LIVING;
    event("Клиент %d: заселился в номер %d на %d сут.", c->id, c->room + 1, c->stay);
    schedule(now_t + MIN_PER_DAY, EV_DAY_END, c);
}

//закончились очередные сутки проживания
static void on_day_end(client_t *c)
{
    ++c->days;
    ++A.room_days;
    event("Клиент %d: номер %d, прожиты сутки %d из %d", c->id, c->room + 1, c->days, c->stay);
    if (c->days < c->stay)
    {
        schedule(now_t + MIN_PER_DAY, EV_DAY_END, c);
    }
    else
    {
        schedule(now_t, EV_CHECKOUT, c);
    }
}

//клиент выезжает и освобождает номер
static void on_checkout(client_t *c)
{
    int r = c->room;
    if (H.room[r] != c)
    {
        violation("выезжает клиент, который не занимает номер");
    }
    if (c->days != c->stay)
    {
        violation("срок проживания учтен не полностью");
    }
    H.room[r] = NULL;
    H.freed_by[r] = c->id;
    --H.occupied;
    ++A.served;
    --H.inside;
    event("Клиент %d: выехал, номер %d свободен (занято %d/%d, ожидают %d)",
        c->id, r + 1, H.occupied, P.rooms, H.wait_len);
    c->state = C_GONE;
    free(c);
    admin_on_room_freed(r);
}

//=============================== прерывание ===============================

//сколько раз нажали Ctrl+C; в обработчике сигнала можно только менять такой флаг
static volatile sig_atomic_t interrupts = 0;

//обработчик SIGINT/SIGTERM
static void on_signal(int sig)
{
    (void)sig;
    ++interrupts;
}

//ставит обработчик через sigaction без SA_RESTART, чтобы задержка прерывалась сразу
static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

//=============================== статистика ===============================

//итоговая статистика
static void print_stats(void)
{
    double days = (double)now_t / MIN_PER_DAY;
    double load = now_t ? 100.0 * (double)A.room_days * MIN_PER_DAY / ((double)now_t * P.rooms) : 0.0;
    double avg = A.from_queue ? (double)A.total_wait / A.from_queue / MIN_PER_HOUR : 0.0;
    out("==================== ИТОГИ ====================\n");
    out("модельное время: %.1f сут.\n", days);
    out("прибыло клиентов: %d, обслужено: %d, отказов: %d, еще в гостинице: %d, в очереди: %d\n",
        A.arrived, A.served, A.rejected, H.occupied, H.wait_len);
    out("прожито номеро-суток: %ld, загрузка номеров: %.0f%%\n", A.room_days, load);
    out("максимум занятых номеров: %d из %d, максимальная длина очереди: %d\n",
        A.max_occupied, P.rooms, A.max_wait_len);
    out("заселено из очереди: %d, ожидание в среднем %.1f ч, максимум %.1f ч\n",
        A.from_queue, avg, (double)A.max_wait / MIN_PER_HOUR);
}

//=============================== разбор параметров ===============================

//парсит целое и проверяет диапазон
static int parse_int(const char *key, const char *s, int lo, int hi)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
    {
        ++end;
    }
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
        *a = parse_int(key, buf, lo, hi);
        *b = parse_int(key, dash + 1, lo, hi);
    }
    else
    {
        *a = parse_int(key, buf, lo, hi);
        *b = *a;
    }
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
        P.rooms = parse_int(key, val, 1, 100000);
    }
    else if (!strcmp(key, "clients"))
    {
        P.clients = parse_int(key, val, 0, 10000000);
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
        P.queue_cap = parse_int(key, val, -1, 10000000);
    }
    else if (!strcmp(key, "duration"))
    {
        P.duration = parse_int(key, val, 0, 100000);
    }
    else if (!strcmp(key, "delay"))
    {
        P.day_ms = parse_int(key, val, 0, 600000);
    }
    else if (!strcmp(key, "seed"))
    {
        P.seed = strtoull(val, NULL, 10);
        P.seed_given = 1;
    }
    else if (!strcmp(key, "log"))
    {
        free(P.log_path);
        P.log_path = strdup(val);
    }
    else if (!strcmp(key, "strategy"))
    {
        if (!strcmp(val, "fifo"))
        {
            P.strat = STRAT_FIFO;
        }
        else if (!strcmp(val, "short"))
        {
            P.strat = STRAT_SHORT;
        }
        else if (!strcmp(val, "long"))
        {
            P.strat = STRAT_LONG;
        }
        else
        {
            die("стратегия должна быть fifo, short или long, получено \"%s\"", val);
        }
    }
    else if (!strcmp(key, "stays"))
    {
        free(P.stays);
        P.stays = NULL;
        P.stays_len = 0;
        char *copy = strdup(val);
        char *save = NULL;
        for (char *t = strtok_r(copy, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
        {
            P.stays = realloc(P.stays, sizeof(int) * (size_t)(P.stays_len + 1));
            P.stays[P.stays_len++] = parse_int("stays", t, 1, 365);
        }
        free(copy);
        if (!P.stays_len)
        {
            die("параметр stays пуст");
        }
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
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
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
    char *text = NULL;
    size_t len = 0;
    char chunk[4096];
    for (;;)
    {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            break;
        }
        text = realloc(text, len + (size_t)n + 1);
        memcpy(text + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    close(fd);
    if (!text)
    {
        return;
    }
    text[len] = '\0';

    int line_no = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        ++line_no;
        char *s = trim(line);
        if (!*s || *s == '#')
        {
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
        {
            die("%s:%d: ожидается строка вида ключ=значение", path, line_no);
        }
        *eq = '\0';
        set_param(trim(s), trim(eq + 1));
    }
    free(text);
}

//читает строку с клавиатуры через read, возвращает 0 при конце ввода
static int read_line(char *buf, size_t size)
{
    size_t n = 0;
    while (n + 1 < size)
    {
        char ch;
        ssize_t r = read(STDIN_FILENO, &ch, 1);
        if (r < 0 && errno == EINTR)
        {
            continue;
        }
        if (r <= 0)
        {
            break;
        }
        if (ch == '\n')
        {
            buf[n] = '\0';
            return 1;
        }
        buf[n++] = ch;
    }
    buf[n] = '\0';
    return n > 0;
}

//спрашивает один параметр у пользователя
static void ask(const char *prompt, const char *key, const char *def)
{
    char buf[256];
    out("%s [%s]: ", prompt, def);
    if (!read_line(buf, sizeof buf))
    {
        set_param(key, def);
        return;
    }
    char *s = trim(buf);
    set_param(key, *s ? s : def);
}

//интерактивный ввод всех параметров
static void interactive(void)
{
    char d[64];
    char buf[4096];
    out("Интерактивный ввод параметров (Enter - значение по умолчанию)\n");
    snprintf(d, sizeof d, "%d", P.rooms);
    ask("Количество номеров", "rooms", d);
    snprintf(d, sizeof d, "%d", P.clients);
    ask("Количество клиентов (0 - без ограничения)", "clients", d);
    snprintf(d, sizeof d, "%d-%d", P.arr_min, P.arr_max);
    ask("Интервал между прибытиями, часы (мин-макс)", "arrival", d);
    snprintf(d, sizeof d, "%d-%d", P.stay_min, P.stay_max);
    ask("Срок проживания, сутки (мин-макс)", "stay", d);
    out("Сроки для каждого клиента через запятую (Enter - случайные): ");
    if (read_line(buf, sizeof buf) && *trim(buf))
    {
        set_param("stays", trim(buf));
    }
    snprintf(d, sizeof d, "%d", P.queue_cap);
    ask("Вместимость зоны ожидания (-1 - без ограничения)", "queue", d);
    ask("Стратегия заселения (fifo/short/long)", "strategy", "fifo");
    snprintf(d, sizeof d, "%d", P.duration);
    ask("Продолжительность приема клиентов, сутки (0 - без ограничения)", "duration", d);
    snprintf(d, sizeof d, "%d", P.day_ms);
    ask("Задержка отображения, мс на сутки (0 - без задержки)", "delay", d);
}

//справка по опциям
static void usage(const char *prog)
{
    out("Модель гостиницы (задача 12), последовательная имитация\n\n"
        "Использование: %s [опции]\n"
        "  -r, --rooms N        количество номеров (по умолчанию %d)\n"
        "  -n, --clients N      количество клиентов, 0 - без ограничения (%d)\n"
        "  -a, --arrival A-B    интервал между прибытиями, часы (%d-%d)\n"
        "  -s, --stay A-B       срок проживания, сутки, случайный (%d-%d)\n"
        "  -S, --stays L        сроки для каждого клиента явно, например 3,1,5,2\n"
        "  -q, --queue N        вместимость зоны ожидания, -1 - без ограничения (%d)\n"
        "  -p, --strategy S     стратегия заселения: fifo | short | long (fifo)\n"
        "  -d, --duration N     продолжительность приема клиентов, сутки, 0 - без ограничения (%d)\n"
        "  -t, --delay N        задержка отображения, мс на модельные сутки, 0 - без задержки (%d)\n"
        "  -x, --seed N         начальное значение генератора (по умолчанию от времени)\n"
        "  -l, --log FILE       сохранить журнал событий и статистику в файл\n"
        "  -c, --config FILE    читать параметры из файла ключ=значение\n"
        "  -i, --interactive    ввести параметры интерактивно\n"
        "  -h, --help           эта справка\n\n"
        "Моделирование заканчивается, когда прием клиентов прекращен (прибыли все,\n"
        "истекла продолжительность или нажато Ctrl+C), все проживающие выехали и\n"
        "очередь обработана. Повторное Ctrl+C - немедленный выход.\n",
        prog, P.rooms, P.clients, P.arr_min, P.arr_max, P.stay_min, P.stay_max,
        P.queue_cap, P.duration, P.day_ms);
}

//разбор аргументов командной строки и проверка параметров
static void parse_args(int argc, char **argv)
{
    static const struct option opts[] =
    {
        {"rooms", 1, 0, 'r'},
        {"clients", 1, 0, 'n'},
        {"arrival", 1, 0, 'a'},
        {"stay", 1, 0, 's'},
        {"stays", 1, 0, 'S'},
        {"queue", 1, 0, 'q'},
        {"strategy", 1, 0, 'p'},
        {"duration", 1, 0, 'd'},
        {"delay", 1, 0, 't'},
        {"seed", 1, 0, 'x'},
        {"log", 1, 0, 'l'},
        {"config", 1, 0, 'c'},
        {"interactive", 0, 0, 'i'},
        {"help", 0, 0, 'h'},
        {0, 0, 0, 0},
    };
    int inter = 0;
    int o;
    while ((o = getopt_long(argc, argv, "r:n:a:s:S:q:p:d:t:x:l:c:ih", opts, NULL)) != -1)
    {
        switch (o)
        {
        case 'r':
            set_param("rooms", optarg);
            break;
        case 'n':
            set_param("clients", optarg);
            break;
        case 'a':
            set_param("arrival", optarg);
            break;
        case 's':
            set_param("stay", optarg);
            break;
        case 'S':
            set_param("stays", optarg);
            break;
        case 'q':
            set_param("queue", optarg);
            break;
        case 'p':
            set_param("strategy", optarg);
            break;
        case 'd':
            set_param("duration", optarg);
            break;
        case 't':
            set_param("delay", optarg);
            break;
        case 'x':
            set_param("seed", optarg);
            break;
        case 'l':
            set_param("log", optarg);
            break;
        case 'c':
            load_config(optarg);
            break;
        case 'i':
            inter = 1;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            usage(argv[0]);
            exit(EXIT_BAD_ARGS);
        }
    }
    if (optind < argc)
    {
        die("лишний аргумент \"%s\" (см. --help)", argv[optind]);
    }
    if (inter)
    {
        interactive();
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
    //при нулевом интервале и бесконечном числе клиентов модельное время никогда не сдвинется
    if (P.arr_max == 0 && P.clients == 0)
    {
        die("при нулевом интервале прибытия число клиентов должно быть ограничено");
    }
}

//=============================== главный цикл ===============================

//ждет, пока пройдет реальное время, соответствующее dt модельных минут; прерывается по Ctrl+C
static void display_delay(long dt)
{
    if (P.day_ms <= 0 || dt <= 0)
    {
        return;
    }
    long ms = dt * P.day_ms / MIN_PER_DAY;
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    int seen = interrupts;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
    {
        //новое нажатие Ctrl+C - перестаем ждать, чтобы сразу на него отреагировать
        if (interrupts != seen)
        {
            break;
        }
    }
}

//запуск: инициализация, цикл обработки событий и итоги
int main(int argc, char **argv)
{
    parse_args(argc, argv);
    rng_state = P.seed;

    if (P.log_path)
    {
        log_fd = open(P.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd < 0)
        {
            die("не удалось открыть файл журнала %s: %s", P.log_path, strerror(errno));
        }
    }

    H.room = calloc((size_t)P.rooms, sizeof *H.room);
    H.freed_by = malloc(sizeof(int) * (size_t)P.rooms);
    if (!H.room || !H.freed_by)
    {
        die("недостаточно памяти");
    }
    for (int r = 0; r < P.rooms; ++r)
    {
        H.freed_by[r] = -1;
    }
    setup_signals();

    //параметры прогона
    char clients_s[32];
    char queue_s[32];
    char dur_s[32];
    snprintf(clients_s, sizeof clients_s, P.clients ? "%d" : "без ограничения", P.clients);
    snprintf(queue_s, sizeof queue_s, P.queue_cap >= 0 ? "%d" : "без ограничения", P.queue_cap);
    snprintf(dur_s, sizeof dur_s, P.duration ? "%d сут." : "без ограничения", P.duration);
    out("Параметры: номеров %d, клиентов %s, прибытие каждые %d-%d ч, ", P.rooms, clients_s, P.arr_min, P.arr_max);
    if (P.stays)
    {
        out("сроки из списка, ");
    }
    else
    {
        out("срок %d-%d сут., ", P.stay_min, P.stay_max);
    }
    out("зона ожидания %s,\n           стратегия %s, прием клиентов %s, seed %llu\n",
        queue_s, strat_name(P.strat), dur_s, (unsigned long long)P.seed);
    if (P.clients == 0 && P.duration == 0)
    {
        out("Режим без ограничения: моделирование идет до Ctrl+C\n");
    }

    //начальные события: первое прибытие и конец приема клиентов
    if (P.duration > 0)
    {
        schedule((long)P.duration * MIN_PER_DAY, EV_STOP_ARRIVALS, NULL);
    }
    schedule((long)rng_range(P.arr_min, P.arr_max) * MIN_PER_HOUR, EV_ARRIVAL, NULL);

    //главный цикл: берем ближайшее событие, сдвигаем модельное время и обрабатываем
    while (Q.len > 0)
    {
        if (interrupts >= 2)
        {
            event("Повторное прерывание: аварийное завершение");
            print_stats();
            if (log_fd >= 0)
            {
                close(log_fd);
            }
            return EXIT_INTERRUPTED;
        }
        if (interrupts == 1 && !A.arrivals_closed)
        {
            admin_close_arrivals("по прерыванию, проживающие досиживают свои сроки, повторное Ctrl+C - немедленный выход");
        }

        event_t e = pop_event();
        //после закрытия приема отложенное прибытие и конец приема уже не нужны,
        //время по ним не сдвигаем, иначе исказится статистика
        if ((e.type == EV_ARRIVAL || e.type == EV_STOP_ARRIVALS) && A.arrivals_closed)
        {
            continue;
        }
        display_delay(e.time - now_t);
        now_t = e.time;

        switch (e.type)
        {
        case EV_ARRIVAL:
            on_arrival();
            break;
        case EV_REQUEST:
            on_request(e.c);
            break;
        case EV_CHECKIN:
            on_checkin(e.c);
            break;
        case EV_DAY_END:
            on_day_end(e.c);
            break;
        case EV_CHECKOUT:
            on_checkout(e.c);
            break;
        case EV_STOP_ARRIVALS:
            admin_close_arrivals("истекла продолжительность приема");
            break;
        }
        check_invariants();
    }

    if (H.occupied || H.wait_head || H.inside)
    {
        violation("моделирование закончилось, а в гостинице остались клиенты");
    }
    event("Все клиенты выехали, очередь пуста, моделирование завершено");
    print_stats();

    if (log_fd >= 0)
    {
        close(log_fd);
        out("Журнал сохранен в %s\n", P.log_path);
    }
    free(H.room);
    free(H.freed_by);
    free(Q.a);
    free(P.stays);
    free(P.log_path);
    return 0;
}
