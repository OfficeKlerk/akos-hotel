#!/usr/bin/env bash
#прогон тестов для обеих программ ИДЗ-2: hotel_sem и hotel_cond
#запуск: make test или ./tests/run_tests.sh из папки hw2

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
clean() {
    ! grep -q "НАРУШЕНИЕ" "$OUT/$1.txt"
}

#все прибывшие либо обслужены, либо получили отказ
balanced() {
    [ $(($(stat "$1" 'обслужено') + $(stat "$1" 'отказов'))) -eq "$(stat "$1" 'прибыло клиентов')" ]
}

#последовательность прибытий (номер и срок) - она зависит только от seed
arrivals() {
    grep -o 'Клиент [0-9]*: прибыл в гостиницу (срок [0-9]*' "$OUT/$1.txt"
}

#прогон с прерываниями: interrupt имя число_нажатий параметры...
interrupt() {
    local name="$1" times="$2"
    shift 2
    "./$BIN" "$@" > "$OUT/$name.txt" 2>&1 &
    local pid=$!
    sleep 1
    for ((k = 0; k < times; ++k)); do
        kill -INT "$pid"
        sleep 0.3
    done
    wait "$pid"
    code=$?
}

for BIN in hotel_sem hotel_cond; do
    echo "######## $BIN"
    p="$BIN"

    echo "== обычный прогон"
    run "$p-basic" -c tests/basic.conf -l "$OUT/$p-basic.log"
    check "код 0" [ "$code" -eq 0 ]
    check "все прибывшие обслужены" [ "$(stat "$p-basic" 'прибыло клиентов')" = "$(stat "$p-basic" 'обслужено')" ]
    check "инварианты не нарушены" clean "$p-basic"
    check "каждые сутки отмечены" [ "$(grep -c 'прожиты сутки' "$OUT/$p-basic.txt")" = "$(stat "$p-basic" 'прожито номеро-суток')" ]
    check "журнал сохранен со статистикой" grep -q "ИТОГИ" "$OUT/$p-basic.log"

    echo "== воспроизводимость по seed"
    run "$p-seed" -c tests/basic.conf
    check "те же прибытия и сроки при том же seed" diff -q <(arrivals "$p-basic") <(arrivals "$p-seed")

    echo "== ограниченная зона ожидания"
    run "$p-queue" -c tests/queue_limit.conf
    check "код 0" [ "$code" -eq 0 ]
    check "есть отказы" [ "$(stat "$p-queue" 'отказов')" -gt 0 ]
    check "обслужено + отказов = прибыло" balanced "$p-queue"
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
    check "код 0" [ "$code" -eq 0 ]
    check "прием закрыт по времени" grep -q "истекла продолжительность" "$OUT/$p-dur.txt"
    check "после закрытия никто не прибыл" \
        [ -z "$(sed -n '/Прием новых клиентов прекращен/,$p' "$OUT/$p-dur.txt" | grep 'прибыл в гостиницу')" ]
    check "все дообслужены" [ "$(stat "$p-dur" 'прибыло клиентов')" = "$(stat "$p-dur" 'обслужено')" ]

    echo "== нагрузка 500 клиентов"
    run "$p-stress" -c tests/stress.conf
    check "код 0" [ "$code" -eq 0 ]
    check "инварианты не нарушены" clean "$p-stress"
    check "обслужено + отказов = прибыло" balanced "$p-stress"

    echo "== прерывание"
    interrupt "$p-int1" 1 -r 3 -n 0 -a 6-12 -s 1-2 -t 60 -x 5
    check "одно Ctrl+C: код 0" [ "$code" -eq 0 ]
    check "одно Ctrl+C: все дообслужены" [ "$(stat "$p-int1" 'прибыло клиентов')" = "$(stat "$p-int1" 'обслужено')" ]
    interrupt "$p-int2" 2 -r 2 -n 0 -a 1-3 -s 3-5 -t 200 -x 5
    check "два Ctrl+C: код 130" [ "$code" -eq 130 ]
    check "два Ctrl+C: выведена статистика" grep -q "ИТОГИ" "$OUT/$p-int2.txt"

    echo "== неверные данные"
    for args in "-r 0" "-s 5-2" "-p lifo" "-n 5 -S 1,2" "-c tests/bad.conf" "-c tests/nofile.conf" "-a 0 -n 0" "-t 5"; do
        run "$p-bad" $args
        check "$args: код 2" [ "$code" -eq 2 ]
    done
done

echo
echo "пройдено: $pass, провалено: $fail"
[ "$fail" -eq 0 ]
