#!/usr/bin/env bash
#тесты ИДЗ-3: hotel_named, hotel_unnamed и пара hotel_admin + hotel_client (только Linux)
#запуск: make test или ./tests/run_tests.sh из папки hw3

cd "$(dirname "$0")/.." || exit 1
OUT=tests/out
mkdir -p "$OUT"
pass=0
fail=0

#печать результата одной проверки: check "название" команда
check() {
    local name="$1"
    shift
    if "$@"; then
        echo "  ok    $name"
        pass=$((pass + 1))
    else
        echo "  FAIL  $name"
        fail=$((fail + 1))
    fi
}

#запуск программы, вывод в tests/out/<имя>.txt, код возврата в $code
run() {
    local name="$1"
    shift
    "./$BIN" "$@" > "$OUT/$name.txt" 2>&1
    code=$?
}

#число из итоговой строки, например stat basic "обслужено"
stat() {
    grep -o "$2: [0-9]*" "$OUT/$1.txt" | head -1 | grep -o '[0-9]*$'
}

#порядок, в котором клиенты заселялись из очереди
order() {
    grep -o 'передается клиенту [0-9]*' "$OUT/$1.txt" | grep -o '[0-9]*$' | tr '\n' ',' | sed 's/,$//'
}

#нет нарушений инвариантов
no_violation() {
    ! grep -q "НАРУШЕНИЕ" "$OUT/$1.txt"
}

#обслужено + отказов + прервано = прибыло
balanced() {
    [ $(($(stat "$1" 'обслужено') + $(stat "$1" 'отказов') + $(stat "$1" 'прервано'))) -eq "$(stat "$1" 'прибыло клиентов')" ]
}

#после программы не осталось разделяемой памяти, семафоров и процессов гостиницы
clean() {
    sleep 0.3
    [ -z "$(ls /dev/shm | grep hotel12)" ] && ! pgrep -x hotel_named > /dev/null && ! pgrep -x hotel_unnamed > /dev/null &&
        ! pgrep -x hotel_admin > /dev/null && ! pgrep -x hotel_client > /dev/null
}

#прогон с прерываниями: interrupt имя нажатий задержка параметры...
interrupt() {
    local name="$1" times="$2" delay="$3"
    shift 3
    "./$BIN" "$@" > "$OUT/$name.txt" 2>&1 &
    local pid=$!
    sleep "$delay"
    for ((k = 0; k < times; ++k)); do
        kill -INT "$pid"
        sleep 0.3
    done
    wait "$pid"
    code=$?
}

for BIN in hotel_named hotel_unnamed; do
    echo "######## $BIN"
    p="$BIN"

    echo "== обычный прогон"
    run "$p-basic" -c tests/basic.conf -l "$OUT/$p-basic.log"
    check "код 0" [ "$code" -eq 0 ]
    check "все прибывшие обслужены" [ "$(stat "$p-basic" 'прибыло клиентов')" = "$(stat "$p-basic" 'обслужено')" ]
    check "инварианты не нарушены" no_violation "$p-basic"
    check "каждые сутки отмечены" [ "$(grep -c 'прожиты сутки' "$OUT/$p-basic.txt")" = "$(stat "$p-basic" 'прожито номеро-суток')" ]
    check "журнал со статистикой" grep -q "ИТОГИ" "$OUT/$p-basic.log"
    check "общие объекты удалены" clean

    echo "== ограниченная зона ожидания"
    run "$p-queue" -c tests/queue_limit.conf
    check "есть отказы" [ "$(stat "$p-queue" 'отказов')" -gt 0 ]
    check "баланс клиентов" balanced "$p-queue"
    check "очередь не длиннее 1" [ "$(stat "$p-queue" 'максимальная длина очереди')" -le 1 ]

    echo "== стратегии заселения"
    run "$p-fifo" -c tests/fifo.conf
    run "$p-short" -c tests/short.conf
    run "$p-long" -c tests/long.conf
    check "fifo: 2,3,4,5,6" [ "$(order "$p-fifo")" = "2,3,4,5,6" ]
    check "short: 3,5,6,4,2" [ "$(order "$p-short")" = "3,5,6,4,2" ]
    check "long: 2,4,6,3,5" [ "$(order "$p-long")" = "2,4,6,3,5" ]

    echo "== ограничение по времени"
    run "$p-dur" -c tests/duration.conf
    check "прием закрыт по времени" grep -q "истекла продолжительность" "$OUT/$p-dur.txt"
    check "после закрытия никто не прибыл" \
        [ -z "$(sed -n '/Прием новых клиентов прекращен/,$p' "$OUT/$p-dur.txt" | grep 'прибыл в гостиницу')" ]
    check "все дообслужены" [ "$(stat "$p-dur" 'прибыло клиентов')" = "$(stat "$p-dur" 'обслужено')" ]

    echo "== нагрузка: 300 процессов-клиентов"
    run "$p-stress" -c tests/stress.conf
    check "код 0" [ "$code" -eq 0 ]
    check "инварианты не нарушены" no_violation "$p-stress"
    check "баланс клиентов" balanced "$p-stress"
    check "общие объекты удалены" clean

    echo "== прерывание"
    interrupt "$p-int1" 1 1 -r 3 -n 0 -a 6-12 -s 1-2 -t 60 -x 5
    check "одно Ctrl+C: код 0" [ "$code" -eq 0 ]
    check "одно Ctrl+C: все дообслужены" [ "$(stat "$p-int1" 'прибыло клиентов')" = "$(stat "$p-int1" 'обслужено')" ]
    check "одно Ctrl+C: общие объекты удалены" clean
    interrupt "$p-int2" 2 1 -r 2 -n 0 -a 1-3 -s 3-5 -t 200 -x 5
    check "два Ctrl+C: код 130" [ "$code" -eq 130 ]
    check "два Ctrl+C: выведена статистика" grep -q "ИТОГИ" "$OUT/$p-int2.txt"
    check "два Ctrl+C: процессы и общие объекты удалены" clean

    echo "== kill -9 процесса клиента"
    "./$BIN" -r 1 -n 3 -a 1 -S 4,4,4 -t 150 > "$OUT/$p-kill.txt" 2>&1 &
    parent=$!
    sleep 0.6
    victim=$(pgrep -P "$parent" | tail -1)
    kill -9 "$victim"
    wait "$parent"
    code=$?
    check "администратор заметил и освободил место" grep -q "процесс клиента .* прерван" "$OUT/$p-kill.txt"
    check "код 0" [ "$code" -eq 0 ]
    check "баланс клиентов" balanced "$p-kill"
    check "общие объекты удалены" clean

    echo "== неверные данные"
    for args in "-r 0" "-r 65" "-s 5-2" "-p lifo" "-c tests/bad.conf" "-a 0 -n 0"; do
        run "$p-bad" $args
        check "$args: код 2" [ "$code" -eq 2 ]
    done
done

echo "######## hotel_admin + hotel_client"
BIN=hotel_client

echo "== клиент без администратора"
run ind-noadmin -s 1
check "код 2 и понятная ошибка" grep -q "сначала запустите" "$OUT/ind-noadmin.txt"

echo "== администратор и 5 независимых клиентов"
./hotel_admin -r 2 -d 15 -t 100 -l "$OUT/ind.log" > "$OUT/ind-admin.txt" 2>&1 &
admin=$!
sleep 0.3
for st in 2 1 3 1 2; do
    ./hotel_client -s "$st" > /dev/null 2>&1 &
    sleep 0.05
done
./hotel_admin -r 1 > "$OUT/ind-second.txt" 2>&1
check "второй администратор не запускается" grep -q "уже работает" "$OUT/ind-second.txt"
wait "$admin"
code=$?
check "код 0" [ "$code" -eq 0 ]
check "все 5 обслужены" [ "$(stat ind-admin 'обслужено')" = 5 ]
check "события клиентов попали в общий журнал" grep -q "прожиты сутки" "$OUT/ind.log"
check "общие объекты удалены" clean

echo "== kill -9 клиента"
./hotel_admin -r 1 -d 10 -t 100 > "$OUT/ind-kill.txt" 2>&1 &
admin=$!
sleep 0.2
./hotel_client -s 5 > /dev/null 2>&1 &
victim=$!
sleep 0.5
kill -9 "$victim"
wait "$victim" 2>/dev/null
wait "$admin"
check "номер освобожден, прервано 1" [ "$(stat ind-kill 'прервано')" = 1 ]
check "общие объекты удалены" clean

echo "== Ctrl+C администратора"
./hotel_admin -r 1 -t 100 > "$OUT/ind-int1.txt" 2>&1 &
admin=$!
sleep 0.2
./hotel_client -s 3 > /dev/null 2>&1 &
sleep 0.3
kill -INT "$admin"
wait "$admin"
code=$?
check "одно Ctrl+C: код 0" [ "$code" -eq 0 ]
check "одно Ctrl+C: клиент дожил срок" [ "$(stat ind-int1 'обслужено')" = 1 ]
./hotel_admin -r 1 -t 100 > "$OUT/ind-int2.txt" 2>&1 &
admin=$!
sleep 0.2
./hotel_client -s 9 > /dev/null 2>&1 &
sleep 0.1
./hotel_client -s 1 > "$OUT/ind-int2-client.txt" 2>&1 &
sleep 0.3
kill -INT "$admin"
sleep 0.2
kill -INT "$admin"
wait "$admin"
code=$?
wait
check "два Ctrl+C: код 130" [ "$code" -eq 130 ]
check "клиент в очереди понял, что администратора нет" grep -q "администратор не отвечает" "$OUT/ind-int2-client.txt"
check "общие объекты удалены" clean

echo "== остатки после kill -9 администратора"
./hotel_admin -r 1 -t 100 > /dev/null 2>&1 &
admin=$!
sleep 0.3
kill -9 "$admin"
wait "$admin" 2>/dev/null
check "остатки есть" [ -n "$(ls /dev/shm | grep hotel12)" ]
./hotel_admin -r 1 -d 1 -t 50 > "$OUT/ind-stale.txt" 2>&1
check "новый запуск их убрал и отработал" [ $? -eq 0 ]
check "общие объекты удалены" clean

echo
echo "пройдено: $pass, провалено: $fail"
[ "$fail" -eq 0 ]
