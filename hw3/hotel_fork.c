//ИДЗ-3, программы hotel_named и hotel_unnamed (4-6 баллов):
//родительский процесс через fork запускает администратора и клиентов
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hotel.h"

//настройка дочернего процесса: Ctrl+C приходит всей группе процессов, а детей корректно
//останавливает родитель, поэтому SIGINT они игнорируют; если родитель погибнет, ребенок получит SIGTERM
static void child_setup(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_DFL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

//аварийная остановка: завершить всех детей, вывести итоги и выйти; общие объекты удалит atexit
static void stop_all(const char *why, int code)
{
    lock();
    event("%s", why);
    kill(g->admin_pid, SIGTERM);
    for (int s = 0; s < MAX_SLOTS; ++s)
    {
        if (g->c[s].state != C_FREE && g->c[s].pid > 0)
        {
            kill(g->c[s].pid, SIGTERM);
        }
    }
    print_stats();
    unlock();
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR)
    {
    }
    exit(code);
}

//запуск: администратор, генерация клиентов, ожидание всех процессов
int main(int argc, char **argv)
{
    parse_args(argc, argv, "r:n:a:s:S:q:p:d:t:x:l:c:h", "Гостиница (задача 12): родитель запускает процессы администратора и клиентов");
    shm_create();
    install_signals();
    out("Синхронизация: %s\n", sync_name);
    out("Номеров %d, клиентов %d, прибытие каждые %d-%d ч, очередь %d, прием %d сут., сутки = %d мс, seed %llu\n",
        P.rooms, P.clients, P.arr_min, P.arr_max, P.queue_cap, P.duration, P.day_ms, P.seed);
    out("(0 клиентов, 0 суток приема и очередь -1 - без ограничения)\n");

    pid_t pid = fork();
    if (pid == 0)
    {
        child_setup();
        admin_run();
        _exit(0);
    }
    if (pid < 0)
    {
        die("не удалось создать процесс администратора");
    }
    g->admin_pid = pid; //до первого клиента, иначе клиент решит, что администратора нет

    long end_ms = (long)P.duration * P.day_ms;
    const char *reason = "прибыли все клиенты";
    for (int i = 0; P.clients == 0 || i < P.clients; ++i)
    {
        //ждем следующего прибытия короткими снами, чтобы сразу заметить Ctrl+C и конец приема
        long target = now_ms() + (long)rng_range(P.arr_min, P.arr_max) * P.day_ms / 24;
        while (!interrupts && !g->arrivals_closed && now_ms() < target && !(end_ms && now_ms() >= end_ms))
        {
            sleep_ms(5);
            while (waitpid(-1, NULL, WNOHANG) > 0) //забираем завершившихся детей, чтобы не копились зомби
            {
            }
        }
        if (interrupts)
        {
            reason = "по прерыванию";
            break;
        }
        if (end_ms && now_ms() >= end_ms)
        {
            reason = "истекла продолжительность приема";
            break;
        }
        lock();
        int s = g->arrivals_closed ? -1 : slot_alloc(stays_len ? stays[i] : rng_range(P.stay_min, P.stay_max));
        if (s < 0 && !g->arrivals_closed)
        {
            ++g->arrived;
            ++g->rejected;
            event("Клиент не принят: в гостинице уже %d человек, больше не помещается", MAX_SLOTS);
        }
        int closed = g->arrivals_closed;
        unlock();
        if (closed)
        {
            break;
        }
        if (s < 0)
        {
            continue;
        }
        pid = fork();
        if (pid == 0)
        {
            child_setup();
            client_run(s);
            _exit(0);
        }
        lock();
        if (pid < 0)
        {
            slot_free(s);
            reason = "не удалось создать процесс клиента";
            unlock();
            break;
        }
        g->c[s].pid = pid;
        unlock();
    }
    lock();
    close_arrivals(reason);
    unlock();

    //ждем всех детей; первое Ctrl+C уже закрыло прием, второе останавливает всех сразу
    int status;
    for (;;)
    {
        pid_t w = waitpid(-1, &status, 0);
        if (w == g->admin_pid && !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
        {
            stop_all("Администратор завершился с ошибкой, моделирование остановлено", 3);
        }
        if (w > 0)
        {
            continue;
        }
        if (errno != EINTR)
        {
            break;
        }
        if (interrupts >= 2)
        {
            stop_all("Повторное прерывание: аварийное завершение", 130);
        }
        lock();
        close_arrivals("по прерыванию");
        unlock();
    }

    lock();
    if (g->occupied || g->wait_n || g->req_n || g->alive)
    {
        violation("моделирование закончилось, а в гостинице остались клиенты");
    }
    event("Все процессы завершились, гостиница пуста, моделирование завершено");
    print_stats();
    unlock();
    return 0;
}
