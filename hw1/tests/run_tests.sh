#!/usr/bin/env bash
#прогон всех тестовых наборов для задачи 12
#запуск: make test или ./tests/run_tests.sh из корня проекта

cd "$(dirname "$0")/.." || exit 1
BIN=./hotel
OUT=tests/out
mkdir -p "$OUT"

pass=0
fail=0

#печать результата одной проверки: check "название" условие
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

#запуск программы с конфигом, вывод сохраняется в tests/out/<имя>.txt, код возврата в $code
run() {
    local name="$1"
    shift
    "$BIN" "$@" > "$OUT/$name.txt" 2>&1
    code=$?
}

#значение из итоговой строки, например stat basic "обслужено"
stat() {
    grep -o "$2: [0-9]*" "$OUT/$1.txt" | head -1 | grep -o '[0-9]*$'
}

#порядок, в котором клиенты заселялись из очереди
handoff_order() {
    grep -o 'передается клиенту [0-9]*' "$OUT/$1.txt" | grep -o '[0-9]*$' | tr '\n' ',' | sed 's/,$//'
}

#файлы различаются
differ() {
    ! diff -q "$1" "$2" > /dev/null
}

no_violation() {
    ! grep -q "НАРУШЕНИЕ" "$OUT/$1.txt"
}

echo "== обычный прогон"
run basic -c tests/basic.conf -l "$OUT/basic.log"
check "код возврата 0" [ "$code" -eq 0 ]
check "все прибывшие обслужены" [ "$(stat basic 'прибыло клиентов')" = "$(stat basic 'обслужено')" ]
check "инварианты не нарушены" no_violation basic
check "каждые сутки отмечены: номеро-суток = число строк 'прожиты сутки'" \
    [ "$(grep -c 'прожиты сутки' "$OUT/basic.txt")" = "$(stat basic 'прожито номеро-суток')" ]
check "журнал сохранен и содержит итоги" grep -q "ИТОГИ" "$OUT/basic.log"
check "журнал совпадает с выводом на экран" \
    diff <(grep -v '^Журнал сохранен' "$OUT/basic.txt") "$OUT/basic.log"

echo "== воспроизводимость по seed"
run seed_a -c tests/basic.conf
run seed_b -c tests/basic.conf
run seed_c -c tests/basic.conf -x 43
check "одинаковый seed дает одинаковый прогон" diff -q "$OUT/seed_a.txt" "$OUT/seed_b.txt"
check "другой seed дает другой прогон" differ "$OUT/seed_a.txt" "$OUT/seed_c.txt"

echo "== ограниченная зона ожидания"
run queue_limit -c tests/queue_limit.conf
check "код возврата 0" [ "$code" -eq 0 ]
check "есть отказы" [ "$(stat queue_limit 'отказов')" -gt 0 ]
check "обслужено + отказов = прибыло" \
    [ $(( $(stat queue_limit 'обслужено') + $(stat queue_limit 'отказов') )) -eq "$(stat queue_limit 'прибыло клиентов')" ]
check "очередь не длиннее 1" [ "$(stat queue_limit 'максимальная длина очереди')" -le 1 ]

echo "== без зоны ожидания"
run no_queue -c tests/no_queue.conf
check "код возврата 0" [ "$code" -eq 0 ]
check "никто не стоял в очереди" [ "$(stat no_queue 'максимальная длина очереди')" -eq 0 ]
check "инварианты не нарушены" no_violation no_queue

echo "== стратегии заселения"
run fifo -c tests/fifo.conf
run short -c tests/short.conf
run long -c tests/long.conf
check "fifo: порядок 2,3,4,5,6" [ "$(handoff_order fifo)" = "2,3,4,5,6" ]
check "short: порядок 3,5,6,4,2" [ "$(handoff_order short)" = "3,5,6,4,2" ]
check "long: порядок 2,4,6,3,5" [ "$(handoff_order long)" = "2,4,6,3,5" ]

echo "== ограничение по времени"
run duration -c tests/duration.conf
check "код возврата 0" [ "$code" -eq 0 ]
check "прием закрыт по истечении 3 суток" grep -q "сутки   4 00:00\] Прием новых клиентов прекращен (истекла" "$OUT/duration.txt"
check "после закрытия никто не прибыл" \
    [ -z "$(sed -n '/Прием новых клиентов прекращен/,$p' "$OUT/duration.txt" | grep 'прибыл в гостиницу')" ]
check "все дообслужены" [ "$(stat duration 'прибыло клиентов')" = "$(stat duration 'обслужено')" ]

echo "== нагрузка 100000 клиентов"
run stress -c tests/stress.conf
check "код возврата 0" [ "$code" -eq 0 ]
check "инварианты не нарушены" no_violation stress
check "обслужено + отказов = прибыло" \
    [ $(( $(stat stress 'обслужено') + $(stat stress 'отказов') )) -eq "$(stat stress 'прибыло клиентов')" ]

echo "== прерывание Ctrl+C"
"$BIN" -r 3 -n 0 -a 6-12 -s 1-2 -t 100 -x 5 > "$OUT/sigint1.txt" 2>&1 &
pid=$!
sleep 1
kill -INT "$pid"
wait "$pid"
code=$?
check "одно прерывание: код 0" [ "$code" -eq 0 ]
check "одно прерывание: прием закрыт и все выехали" \
    grep -q "Все клиенты выехали" "$OUT/sigint1.txt"
check "одно прерывание: все дообслужены" [ "$(stat sigint1 'прибыло клиентов')" = "$(stat sigint1 'обслужено')" ]

"$BIN" -r 2 -n 0 -a 1-3 -s 3-5 -t 200 -x 5 > "$OUT/sigint2.txt" 2>&1 &
pid=$!
sleep 1
kill -INT "$pid"
sleep 0.3
kill -INT "$pid"
wait "$pid"
code=$?
check "два прерывания: код 130" [ "$code" -eq 130 ]
check "два прерывания: выведена статистика" grep -q "ИТОГИ" "$OUT/sigint2.txt"

echo "== неверные данные"
run bad1 -r 0
check "0 номеров: код 2" [ "$code" -eq 2 ]
run bad2 -s 5-2
check "мин > макс: код 2" [ "$code" -eq 2 ]
run bad3 -p lifo
check "неизвестная стратегия: код 2" [ "$code" -eq 2 ]
run bad4 -n 5 -S 1,2
check "сроков меньше, чем клиентов: код 2" [ "$code" -eq 2 ]
run bad5 -c tests/bad.conf
check "ошибка в конфиге: код 2" [ "$code" -eq 2 ]
run bad6 -c tests/nonexistent.conf
check "нет файла конфига: код 2" [ "$code" -eq 2 ]
run bad7 -a 0 -n 0
check "нулевой интервал и бесконечно клиентов: код 2" [ "$code" -eq 2 ]

echo "== интерактивный ввод"
printf '2\n5\n3-3\n1-1\n\n-1\nfifo\n0\n0\n' | "$BIN" -i -x 1 > "$OUT/interactive.txt" 2>&1
code=$?
check "код 0" [ "$code" -eq 0 ]
check "параметры приняты" grep -q "номеров 2, клиентов 5" "$OUT/interactive.txt"

echo
echo "пройдено: $pass, провалено: $fail"
[ "$fail" -eq 0 ]
