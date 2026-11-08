#include "mapped_file.h"

#include <cerrno>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
[[noreturn]] void ThrowErrno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}
}

MappedFile::MappedFile(const std::filesystem::path& path, std::size_t size) : size_(size) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ == -1) ThrowErrno("open");

    if (::ftruncate(fd_, static_cast<off_t>(size)) == -1) {
        const int err = errno;
        ::close(fd_);
        throw std::system_error(err, std::generic_category(), "ftruncate");
    }
    if (size == 0) return;

    void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (addr == MAP_FAILED) {
        const int err = errno;
        ::close(fd_);
        throw std::system_error(err, std::generic_category(), "mmap");
    }
    data_ = static_cast<uint8_t*>(addr);
}

MappedFile::~MappedFile() { Reset(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        Reset();
        fd_ = std::exchange(other.fd_, -1);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void MappedFile::Sync() {
    if (data_ != nullptr && ::msync(data_, size_, MS_SYNC) == -1) ThrowErrno("msync");
}

void MappedFile::Reset() noexcept {
    if (data_ != nullptr) ::munmap(data_, size_);
    if (fd_ != -1) ::close(fd_);
    fd_ = -1;
    data_ = nullptr;
    size_ = 0;
}
