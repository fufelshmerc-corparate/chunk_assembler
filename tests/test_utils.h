#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <unistd.h>

// Временная папка под тест, удаляется в деструкторе.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("chunk_assembler_" + std::to_string(::getpid()) + "_" + std::to_string(rd()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path File(uint32_t id) const { return path_ / ("file_" + std::to_string(id) + ".bin"); }
    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline std::vector<uint8_t> ToBytes(std::span<const uint8_t> s) { return {s.begin(), s.end()}; }
