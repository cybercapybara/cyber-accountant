#!/usr/bin/env bash
#
# Собрать каждый образец блоков (templates/blocks-samples/*.json) режимом
# --compile-blocks, отрендерить настоящим движком и прогнать получившийся PDF
# через scripts/check-render.py — те же четыре слоя, что и для встроенных
# шаблонов.
#
# ЗАЧЕМ ЭТО ОТДЕЛЬНО ОТ render-templates.sh: у образца блоков нет каталога
# версии на диске. Шаблон, фикстура и expected.txt порождаются на месте самим
# сборщиком, и проверять надо ИМЕННО их — модульные тесты сборщика сверяют
# подстроки и не могут судить, компилируется ли собранный текст. Ровно так
# прошла незамеченной лишняя закрывающая скоба.
set -uo pipefail

WORKER=${WORKER:-/app/cyber_accountant_worker}
SAMPLES=${SAMPLES:-/app/templates/blocks-samples}
GATE=${GATE:-/scripts/check-render.py}
OUT_ROOT=${KEEP_RENDERS:-$(mktemp -d)}

pass=0
fail=0

shopt -s nullglob
for sample in "$SAMPLES"/*.json; do
    name=$(basename "$sample" .json)
    out="$OUT_ROOT/blocks_$name"
    mkdir -p "$out"

    if ! "$WORKER" --compile-blocks "$sample" "$out"; then
        fail=$((fail + 1))
        continue
    fi

    # Гейт ожидает каталог шаблона (expected.txt рядом с template.typ) и
    # фикстуру. Собранный шаблон лежит как main.typ — кладём рядом под тем
    # именем, которое гейт читает для сверки поля страницы.
    cp "$out/main.typ" "$out/template.typ"
    if python3 "$GATE" "$out" "$out/fixture.json" "$out/main.pdf"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
done

echo "blocks: PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
