# SPO 3

Краткая карта проекта.

## Главная цепочка

```text
simpleLang source
-> remote-parser.ps1
-> asm + dgml + sym
-> remotetasks-assemble.ps1
-> ptptb + embedded inspector metadata
-> remotetasks-inspect.ps1
-> remotetasks-run.ps1
```

## Главные файлы

- грамматика: `src/SimpleLang.g`
- генератор: `src/cfg_builder.c`
- VM: `src/TacVm13.target.pdsl`
- runtime I/O:
  - `src/runtime/in.asm`
  - `src/runtime/out.asm`
- inspector:
  - `tools/SpoInspector/Program.cs`
  - `tools/remotetasks-inspect.ps1`

## Основные документы

- `TESTING.md` - команды для проверки
- `INSPECTOR.md` - запуск и команды инспектора lab 4
- `LAB5.md` - памятка по лабораторной 5
- `VIRTUAL_MACHINE.md` - описание `TacVm13`
- `REMOTE_TASKS_MANAGER.md` - как используется `Portable.RemoteTasks.Manager.exe`
- `programm.md` - текущее состояние языка и генератора

## Быстрый старт

### Smoke-test

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\remote-parser.ps1 -InputFile .\src\test_echo.txt -AsmOutput .\build\test_echo.asm -ParseTreeOutput .\build\test_echo.dgml
powershell -ExecutionPolicy Bypass -File .\tools\remotetasks-assemble.ps1 -AsmListing .\build\test_echo.asm -DefinitionFile .\src\TacVm13.target.pdsl -ArchName TacVm13 -BinaryOutput .\build\test_echo.ptptb
powershell -ExecutionPolicy Bypass -File .\tools\remotetasks-run.ps1 -BinaryFile .\build\test_echo.ptptb -DefinitionFile .\src\TacVm13.target.pdsl -RunMode InputFile -InputFile .\build\test_echo.stdin.txt -StdinRegStorage INPUT -StdoutRegStorage OUTPUT -ArchName TacVm13
```

### JVM backend smoke-test

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\remote-parser.ps1 -Target jvm -InputFile .\src\test_echo.txt -AsmOutput .\build\jvm\test_echo.jasm -ParseTreeOutput .\build\jvm\test_echo.dgml
cmd /c "echo Z| java -cp build\jvm SimpleLangProgram"
javap -classpath build\jvm -c SimpleLangProgram
```

Дополнительные JVM-проверки:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\remote-parser.ps1 -Target jvm -InputFile .\src\jvm_ops_test.txt -AsmOutput .\build\jvm\ops.jasm -ParseTreeOutput .\build\jvm\ops.dgml
java -cp build\jvm SimpleLangProgram

powershell -ExecutionPolicy Bypass -File .\tools\remote-parser.ps1 -Target jvm -InputFile .\src\jvm_array_test.txt -AsmOutput .\build\jvm\array.jasm -ParseTreeOutput .\build\jvm\array.dgml
java -cp build\jvm SimpleLangProgram
```

### Лабораторная 4: inspector

`remote-parser.ps1` теперь скачивает `*.asm.sym`, а `remotetasks-assemble.ps1` встраивает в `.ptptb` debug-секцию `simplelang.debug.json`.

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\remote-parser.ps1 -InputFile .\src\test.txt -AsmOutput .\build\inspect_test.asm -ParseTreeOutput .\build\inspect_test.dgml -RemoteRunTimeoutSeconds 180
powershell -ExecutionPolicy Bypass -File .\tools\remotetasks-assemble.ps1 -AsmListing .\build\inspect_test.asm -DefinitionFile .\src\TacVm13.target.pdsl -ArchName TacVm13 -BinaryOutput .\build\inspect_test.ptptb
powershell -ExecutionPolicy Bypass -File .\tools\remotetasks-inspect.ps1 -BinaryFile .\build\inspect_test.ptptb -DefinitionFile .\src\TacVm13.target.pdsl -RunMode plain
```

В REPL доступны `regs`, `disas`, `step`, `nextsrc`, `line`, `locals`, `bt`, `bline`, `dline`, `breaks`, `cont`, `quit`.

### Лабораторная 5

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\remote-parser.ps1 -InputFile .\src\classTest.txt -AsmOutput .\build\classTest.asm -ParseTreeOutput .\build\classTest.dgml -RemoteRunTimeoutSeconds 180
powershell -ExecutionPolicy Bypass -File .\tools\remotetasks-assemble.ps1 -AsmListing .\build\classTest.asm -DefinitionFile .\src\TacVm13.target.pdsl -ArchName TacVm13 -BinaryOutput .\build\classTest.ptptb
powershell -ExecutionPolicy Bypass -File .\tools\remotetasks-run.ps1 -BinaryFile .\build\classTest.ptptb -DefinitionFile .\src\TacVm13.target.pdsl -RunMode InteractiveInput -StdinRegStorage INPUT -StdoutRegStorage OUTPUT -ArchName TacVm13
```
