# chunk_assembler

Многопоточная сборка файлов на диске из чанков, которые приходят в любом порядке, с повторами и перекрытиями. Пишется на C++20 под Linux.

Чанки пишутся прямо в файл: под каждый файл делается open, ftruncate и mmap(MAP_SHARED), и копирование идет в отображенную память. Потоки, которые пишут в разные файлы, друг другу не мешают. Крупные чанки одного файла копируются параллельно, вне блокировки.

## Сборка и тесты

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

С санитайзерами:

```bash
# ThreadSanitizer
cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure

# AddressSanitizer + UBSan
cmake -S . -B build-asan -DENABLE_ASAN=ON -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

GTest берется из системы, а если его нет, скачивается через FetchContent. Для TSan системный GTest лучше не брать (он собран без санитайзера), поэтому там стоит -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON.

Демо: файл 64 МБ из перемешанных чанков по 256 КБ в 4 потока.

```bash
./build/demo /tmp/out.bin 64 4 256
```

## Структура проекта

```
.
├── CMakeLists.txt
├── README.md
├── THEORY.md                     # теория для подготовки
├── .github/workflows/ci.yml      # CI: обычная сборка, TSan, ASan
├── examples
│   └── demo.cpp                  # сборка файла в N потоков
├── include
│   ├── chunk_assembler.h         # интерфейс ChunkAssembler
│   ├── interval_set.h            # множество полуинтервалов [begin, end)
│   └── mapped_file.h             # RAII-обертка над mmap
├── src
│   ├── chunk_assembler.cpp
│   └── mapped_file.cpp
└── tests
    ├── chunk_assembler_mt_test.cpp   # многопоточные тесты
    ├── chunk_assembler_test.cpp      # однопоточные тесты
    ├── interval_set_test.cpp
    ├── mapped_file_test.cpp
    └── test_utils.h                  # временная папка для тестов
```

## Интерфейс

```cpp
void AddFile(uint32_t id, const std::filesystem::path& path, std::size_t size);
void AddChunk(uint32_t id, std::size_t pos, std::span<const uint8_t> chunk);
Ranges Read(uint32_t id, std::size_t pos, std::span<uint8_t> out);
Ranges GetReadyRanges(uint32_t id) const;
bool IsComplete(uint32_t id) const;
std::optional<std::span<const uint8_t>> View(uint32_t id);
bool Flush(uint32_t id);
```

- AddFile создает файл нужного размера и отображает его в память.
- AddChunk пишет чанк по позиции pos. Уже записанные байты не перезаписываются.
- Read копирует готовые байты из диапазона в out и возвращает, какие отрезки готовы. Остальные байты out не определены.
- GetReadyRanges возвращает отсортированный список готовых диапазонов.
- View отдает вид на собранный файл без копирования или nullopt, если файл не собран.
- Flush для собранного файла делает msync и возвращает true, для несобранного возвращает false.

## Архитектура

### Два уровня блокировок

- std::shared_mutex на таблицу файлов. Поиск файла берет shared lock, поэтому поиски идут параллельно. Exclusive lock берет только AddFile.
- std::mutex внутри каждого файла. Потоки, которые пишут в разные файлы, не ждут друг друга.
- Эти два мьютекса никогда не держатся одновременно: сначала под shared lock достается shared_ptr на файл, lock отпускается, и только потом берется мьютекс файла. Поэтому deadlock невозможен.
- shared_ptr нужен, чтобы состояние файла жило, пока с ним работает поток, даже без блокировки таблицы.

### Учет диапазонов

У каждого файла два IntervalSet:

- claimed: байты уже записаны или прямо сейчас копируются;
- written: байты точно лежат в файле.

Читатели (Read, GetReadyRanges, IsComplete, View) смотрят только на written.

IntervalSet::Insert добавляет диапазон, склеивает соседние и пересекающиеся отрезки и возвращает дырки, в которые писать данные. Копируются только дырки, поэтому повторный или перекрывающийся чанк не перезаписывает данные.

### Reserve, copy, commit

Для чанков от 64 КБ:

- reserve: под мьютексом файла holes = claimed.Insert(pos, end);
- copy: дырки копируются в mmap без блокировки. Эти байты уже наши, другой поток их не возьмет;
- commit: под мьютексом файла каждая дырка добавляется в written.

Чанки меньше 64 КБ делают все то же самое под одной блокировкой, две лишние блокировки для маленькой копии дороже самой копии.

### mmap

- MappedFile делает open, ftruncate до нужного размера и mmap(MAP_SHARED).
- ftruncate нужен до записи: обращение к странице за концом файла дает SIGBUS.
- Деструктор делает munmap и close. Копировать MappedFile нельзя, перемещать можно.
- Ошибки системных вызовов бросаются как std::system_error с errno.
- Flush вызывает msync(MS_SYNC), чтобы данные гарантированно легли на диск.

## Тестирование

Всего 76 тестов на GTest, все проходят в обычной сборке, под TSan и под ASan + UBSan.

- interval_set_test (15): вставка, склейка, дырки, подсчет покрытия.
- mapped_file_test (5): размер файла, запись доходит до диска, размер 0, errno при ошибке, перемещение.
- chunk_assembler_test (46): все методы по отдельности, перекрытия, повторы, границы, переполнение, файл на диске после Flush.
- chunk_assembler_mt_test (10): гонка регистрации одного id, запись разных и одинаковых диапазонов, много файлов одновременно, читатели во время записи крупных чанков, смесь крупных и мелких чанков на файле 10 МБ.

Тесты пишут файлы во временную папку, которая удаляется после теста.

## Планы

- Асинхронная запись через io_uring вместо mmap: без page faults и с контролем, когда данные уходят на диск.
- Докачка после перезапуска: сохранять готовые диапазоны в отдельный файл и восстанавливать их при старте.
- Подобрать порог 64 КБ бенчмарком (Google Benchmark) по числу потоков и размеру чанка.
- Удаление файла из таблицы (RemoveFile).
- Строгая гарантия исключений: при std::bad_alloc внутри IntervalSet::Insert состояние файла может стать несогласованным.
