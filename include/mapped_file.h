#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

// RAII-обертка над файлом, отображенным в память (open + ftruncate + mmap(MAP_SHARED)).
// Деструктор делает munmap и close, но не ждет записи на диск: для этого нужен Sync().
// Ошибки системных вызовов -> std::system_error с errno.
//
// Внимание: файл создается разреженным, место на диске выделяется при первой записи.
// Если при записи в Data() кончится место или будет ошибка ввода-вывода,
// процесс получит SIGBUS, а не исключение.
class MappedFile {
public:
    MappedFile() = default;

    // Создает файл размера size или обрезает существующий (O_TRUNC): старое содержимое теряется.
    // Новые байты читаются как нули. Нельзя открывать файл, который уже отображен другим
    // MappedFile: после обрезки обращение к его памяти может дать SIGBUS.
    // Для size == 0 файл создается, но mmap не вызывается (mmap длины 0 запрещен).
    MappedFile(const std::filesystem::path& path, std::size_t size);
    ~MappedFile();

    // Копировать нельзя: два владельца сделали бы munmap и close дважды.
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // После перемещения исходный объект пуст: Size() == 0, Data() пустой,
    // деструктор ничего не делает.
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    // Отображенная память. Действительна, пока объект жив и не перемещен.
    // Для size == 0 пустой span с data() == nullptr.
    std::span<std::uint8_t> Data() { return {data_, size_}; }
    std::span<const std::uint8_t> Data() const { return {data_, size_}; }
    std::size_t Size() const { return size_; }

    // Синхронно записывает измененные страницы на диск (msync MS_SYNC).
    // Бросает std::system_error. Для пустого файла ничего не делает.
    void Sync();

private:
    void Reset() noexcept;  // munmap + close, если есть что закрывать

    int fd_ = -1;
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};
