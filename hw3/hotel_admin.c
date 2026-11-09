//ИДЗ-3, программа hotel_admin (7-8 баллов): администратор, запускается отдельно от клиентов
#include "hotel.h"

//создает гостиницу, работает до конца приема и выезда всех клиентов, потом удаляет общие объекты
int main(int argc, char **argv)
{
    parse_args(argc, argv, "r:q:p:d:t:l:c:h", "Гостиница (задача 12): администратор, клиенты запускаются программой hotel_client");
    shm_create();
    install_signals();
    out("Синхронизация: %s\n", sync_name);
    out("Номеров %d, очередь %d (-1 - без ограничения), прием %d сут. (0 - до Ctrl+C), сутки = %d мс\n",
        P.rooms, P.queue_cap, P.duration, P.day_ms);
    out("Клиенты запускаются в других терминалах: ./hotel_client -s 2-4\n");
    admin_run();
    lock();
    print_stats();
    unlock();
    return 0;
}
