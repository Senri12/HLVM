# Task 2 (variant 1) — async-await on the JVM target

## Цель

Расширить компилятор SimpleLang поддержкой высокоуровневой синтаксической
конструкции `async`/`await` (вариант 1) и обеспечить корректное выполнение
скомпилированных программ на эталонной JVM.

## Семантика

В терминах языка SimpleLang добавлены две новые синтаксические единицы:

* модификатор `async` перед сигнатурой функции — функция возвращает
  объект **Task** (целочисленный «handle» в управляемой куче);
* унарный оператор `await EXPR` — берёт `Task` и возвращает его сохранённый
  результат.

Семантика реализуется через паттерн «синхронной материализации Task»:

* при входе в async-функцию аллоцируется объект `Task` (две ячейки в `HEAP`:
  `[done, result]`);
* каждый `return E` async-функции компилируется как
  `__task_complete(task, E); ireturn` (возвращается handle);
* `await t` компилируется как `__await(t)` — чтение `HEAP[t+1]`.

Так как тело async-функции в этой реализации выполняется синхронно, на момент
выхода Task всегда переведён в состояние `done = 1`, поэтому `await` всегда
возвращает корректный результат.  Данный паттерн соответствует семантике C#
`Task<T>` для функций без точек подвески (await’ов на «реально» асинхронные
операции) и достаточен, чтобы продемонстрировать «компиляцию async-await в
дополнительные подпрограммы и пользовательские типы».

## Что добавлено

### 1. Грамматика SimpleLang (`src/SimpleLang.g`)

* Новые лексические токены `ASYNC_KW = 'async'` и `AWAIT_KW = 'await'`.
* Воображаемые AST-токены `ASYNC` и `AWAIT`.
* Альтернативы для async-сигнатур в `typedMethodDef` и `implicitMethodDef`,
  при наличии `async` в AST добавляется маркер `^(ASYNC)` внутри `FUNC_SIG`.
* В `unaryExpr` добавлена альтернатива `'await' unaryExpr -> ^(AWAIT unaryExpr)`.
* Синтаксический предикат в `member` обновлён, чтобы async-методы в классах
  тоже распознавались.

### 2. CFG-builder (`src/cfg_builder.h`, `src/cfg_builder.c`)

* В `FunctionCFG` добавлено поле `int is_async`.
* Добавлен helper `signature_is_async(sig)`, читающий маркер `ASYNC` из
  `FUNC_SIG`. `parse_signature_info` пропускает этот маркер, чтобы не
  принимать его за тип возврата или имя функции.
* Узел `^(AWAIT expr)` в `node_to_code` преобразуется в текст
  `__await(<expr>)`, благодаря чему JVM-бэкенд видит обычный вызов рантайма.

### 3. JVM-бэкенд (`src/jvm_backend.c`)

* Дескриптор возврата для async-функций жёстко задан как `I` (Task handle):
  `jvm_ret_desc()` теперь учитывает `is_async`.
* В прологе скомпилированной async-функции аллоцируется Task (`alloc(2)`),
  handle сохраняется в скрытом локале `__async_task`.
* Любой `return EXPR` внутри async компилируется как:
  `iload __async_task; eval EXPR; invokestatic __task_complete(II)I; ireturn`.
  Та же последовательность ставится в путях `FINISH` и в неполном «дотечении»
  до конца функции.
* В `eval_call` распознан служебный builtin `__await(handle)`, эмитирующий
  `invokestatic __await(I)I`.
* В сгенерированный класс добавлены два дополнительных метода:
  * `public static int __await(int handle)` — `return HEAP[handle+1];`
  * `public static int __task_complete(int task, int result)` — записывает
    `HEAP[task+1] = result; HEAP[task+0] = 1;` и возвращает handle.
* Новые маркеры `JVM_MARKER_AWAIT_MTH` / `JVM_MARKER_TASK_COMPLETE_MTH`
  патчатся ссылками на эти CP-MethodRef’ы при финализации class-файла.
* Listing (`.jasm`) и сериализованный class-файл содержат оба новых метода;
  счётчик методов в class-файле увеличен на 2.

### 4. Тестовые программы

* `src/async_smoke.txt` — минимальный smoke-тест: одна async-функция и одно
  `await`.  Ожидаемый вывод: `A\n`.
* `src/async_test.txt` — расширенный пример: цепочка async-функций (одна
  ждёт другую внутри своего тела), несколько `await` в `main`, демонстрация
  «дополнительных подпрограмм и пользовательских типов» (Task) на работе.
  Ожидаемый вывод: `346\n`.

## Проверка на эталонной JVM

```
make INPUT_FILE=src/async_smoke.txt ASM_FILE=build/jvm/async_smoke.jasm \
     DGML_FILE=build/jvm/async_smoke.dgml asm \
     -- (через remote-parser.ps1 -Target jvm)

java -cp build/jvm SimpleLangProgram
```

В листинге `async_smoke.jasm` рядом с `alloc(I)I`, `__await(I)I` и
`__task_complete(II)I` видны три «дополнительные подпрограммы», описывающие
паттерн Task; в теле каждой пользовательской async-функции виден пролог с
`alloc` и эпилоги с `__task_complete; ireturn`.
