//ИДЗ-3, программа hotel_client (7-8 баллов): один клиент, запускается отдельно от администратора
#include <unistd.h>
#include "hotel.h"

//подключается к работающей гостинице и проживает свой срок; Ctrl+C завершает клиента,
//а администратор, заметив это, освобождает его номер или место в очереди
int main(int argc, char **argv)
{
    parse_args(argc, argv, "s:x:h", "Гостиница (задача 12): клиент, сначала запустите hotel_admin");
    int stay = rng_range(P.stay_min, P.stay_max); //до подключения: потом P заменят параметры администратора
    if (shm_attach() < 0)
    {
        die("гостиница не работает: сначала запустите ./hotel_admin");
    }
    lock();
    int s = g->arrivals_closed ? -1 : slot_alloc(stay);
    if (s < 0)
    {
        ++g->arrived;
        ++g->rejected;
        event("Клиент не принят: %s", g->arrivals_closed ? "прием клиентов закрыт" : "в гостинице нет места");
        unlock();
        return 1;
    }
    g->c[s].pid = getpid();
    unlock();
    client_run(s);
    return 0;
}
