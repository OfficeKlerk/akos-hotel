# ИДЗ-2. Гостиница, многопоточная версия

Две программы с общей моделью (`hotel.c`), отличаются только синхронизацией:
- `hotel_sem` - мьютекс + семафоры (`sync_sem.c`)
- `hotel_cond` - мьютекс + условные переменные (`sync_cond.c`)

Нужен Linux: безымянные семафоры на macOS не работают

```bash
make
./hotel_sem -r 3 -n 12 -q 5 -x 42
./hotel_cond -c tests/basic.conf -l run.log
make test
```

Отчет - в [report.md](report.md)
