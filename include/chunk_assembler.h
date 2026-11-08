#pragma once

#include "interval_set.h"
#include "mapped_file.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// Потокобезопасная сборка файлов на диске из чанков, приходящих в любом порядке,
// с повторами и перекрытиями. Все методы можно вызывать одновременно из любых потоков.
// Все диапазоны это полуинтервалы [begin, end) в координатах файла.
// Ошибки: неизвестный id или повторный AddFile -> std::invalid_argument,
// выход за конец файла -> std::out_of_range, ошибки ОС -> std::system_error.
class ChunkAssembler {
public:
    using Ranges = std::vector<std::pair<std::size_t, std::size_t>>;

    // Создает файл path размера size (существующий файл обрезается) и отображает его в память.
    // Один путь нельзя регистрировать под двумя id. При повторном id файл на диске не трогается.
    // Файл размера 0 сразу считается собранным.
    void AddFile(std::uint32_t id, const std::filesystem::path& path, std::size_t size);

    // Пишет чанк по позиции pos. Уже записанные байты не перезаписываются: побеждает первый чанк.
    // Если часть диапазона сейчас пишет другой поток, вызов может вернуться раньше,
    // чем эти байты появятся в GetReadyRanges. Пустой чанк ничего не делает.
    void AddChunk(std::uint32_t id, std::size_t pos, std::span<const std::uint8_t> chunk);

    // Копирует готовые байты из [pos, pos + out.size()) в out и возвращает готовые отрезки
    // внутри этого диапазона, по возрастанию, в координатах файла (не out).
    // Остальные байты out не определены.
    Ranges Read(std::uint32_t id, std::size_t pos, std::span<std::uint8_t> out) const;

    // Снимок готовых диапазонов: отсортированы, соседние склеены.
    Ranges GetReadyRanges(std::uint32_t id) const;
    bool IsComplete(std::uint32_t id) const;

    // Вид на собранный файл без копирования или nullopt, если файл не собран.
    // Собранный файл больше не меняется, поэтому вид можно читать из любых потоков
    // без блокировок. Действителен, пока жив ChunkAssembler.
    std::optional<std::span<const std::uint8_t>> View(std::uint32_t id) const;

    // Если файл собран: записывает его на диск (msync) и возвращает true. Иначе false.
    // Бросает std::system_error при ошибке msync.
    bool Flush(std::uint32_t id);

private:
    // size и file не меняются после AddFile и читаются без блокировки.
    // mtx защищает written и claimed. Байты отображения защищены протоколом
    // reserve/copy/commit: поток пишет только диапазоны, которые сам взял в claimed,
    // а читатели смотрят только на written. Инвариант: written всегда внутри claimed.
    struct FileState {
        mutable std::mutex mtx;
        std::size_t size = 0;
        MappedFile file;
        IntervalSet written;  // байты записаны в отображение (на диске только после Flush)
        IntervalSet claimed;  // байты записаны или прямо сейчас копируются
    };

    // Поиск под shared lock таблицы. Бросает std::invalid_argument, если id нет.
    std::shared_ptr<FileState> FindFile(uint32_t id) const;

    // Таблица: shared lock на поиск, exclusive на вставку.
    // Никогда не держится вместе с FileState::mtx, поэтому deadlock невозможен.
    mutable std::shared_mutex files_mtx_;
    std::unordered_map<uint32_t, std::shared_ptr<FileState>> files_;
};
