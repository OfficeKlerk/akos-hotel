//синхронизация на мьютексе и условных переменных (вторая программа)
#include <stdlib.h>
#include "hotel.h"

const char *sync_name = "мьютекс + условные переменные";

//условная переменная администратора "есть работа"
static pthread_cond_t admin_cv = PTHREAD_COND_INITIALIZER;

//условная переменная main "клиент покинул модель"
static pthread_cond_t main_cv = PTHREAD_COND_INITIALIZER;

//статические условные переменные уже инициализированы
void sync_init(void)
{
}

//у каждого клиента своя условная переменная
void sync_client_init(client_t *c)
{
    c->sync = malloc(sizeof(pthread_cond_t));
    pthread_cond_init(c->sync, NULL);
}

//удаляет условную переменную клиента
void sync_client_free(client_t *c)
{
    pthread_cond_destroy(c->sync);
    free(c->sync);
}

//клиент ждет решения администратора, вызывающий код проверяет условие в цикле
void wait_client(client_t *c)
{
    pthread_cond_wait(c->sync, &mtx);
}

//администратор сообщает клиенту о решении
void wake_client(client_t *c)
{
    pthread_cond_signal(c->sync);
}

//администратор ждет работы
void wait_admin(void)
{
    pthread_cond_wait(&admin_cv, &mtx);
}

//сообщает администратору о новой работе
void wake_admin(void)
{
    pthread_cond_signal(&admin_cv);
}

//main ждет ухода клиентов
void wait_main(void)
{
    pthread_cond_wait(&main_cv, &mtx);
}

//клиент сообщает main, что покинул модель
void wake_main(void)
{
    pthread_cond_signal(&main_cv);
}

//точка входа программы
int main(int argc, char **argv)
{
    return hotel_run(argc, argv);
}
