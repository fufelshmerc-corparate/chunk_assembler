#include "chunk_assembler.h"

#include <algorithm>
#include <stdexcept>

namespace {
// Чанки от этого размера копируются без блокировки.
constexpr std::size_t kUnlockedCopyThreshold = 64 * 1024;
}  

std::shared_ptr<ChunkAssembler::FileState> ChunkAssembler::FindFile(uint32_t id) const {
    std::shared_lock lock(files_mtx_);
    auto it = files_.find(id);
    if (it == files_.end()) throw std::invalid_argument("unknown file id");
    return it->second;
}

void ChunkAssembler::AddFile(uint32_t id, const std::filesystem::path& path, std::size_t size) {
    std::unique_lock lock(files_mtx_);
    if (files_.contains(id)) throw std::invalid_argument("file id already registered");

    auto state = std::make_shared<FileState>();
    state->size = size;
    state->file = MappedFile(path, size);
    files_.emplace(id, std::move(state));
}

void ChunkAssembler::AddChunk(uint32_t id, std::size_t pos, std::span<const uint8_t> chunk) {
    auto file = FindFile(id);
    const std::size_t size = file->size;
    if (pos > size || chunk.size() > size - pos) throw std::out_of_range("chunk out of file bounds");
    const std::size_t end = pos + chunk.size();

    uint8_t* dst = file->file.Data().data();
    auto copyHoles = [&](const Ranges& holes) {
        for (auto [b, e] : holes) {
            std::copy(chunk.begin() + (b - pos), chunk.begin() + (e - pos), dst + b);
        }
    };

    // Маленький чанк: все под одной блокировкой
    if (chunk.size() < kUnlockedCopyThreshold) {
        std::lock_guard lock(file->mtx);
        auto holes = file->claimed.Insert(pos, end);
        copyHoles(holes);
        for (auto [b, e] : holes) file->written.Insert(b, e);
        return;
    }

    // Крупный чанк: reserve, copy без блокировки, commit
    Ranges holes;
    {
        std::lock_guard lock(file->mtx);
        holes = file->claimed.Insert(pos, end);
    }
    copyHoles(holes);
    {
        std::lock_guard lock(file->mtx);
        for (auto [b, e] : holes) file->written.Insert(b, e);
    }
}

ChunkAssembler::Ranges ChunkAssembler::Read(uint32_t id, std::size_t pos, std::span<uint8_t> out) const{
    auto file = FindFile(id);
    if (pos > file->size || out.size() > file->size - pos) throw std::out_of_range("read out of file bounds");
    const std::size_t end = pos + out.size();
    const uint8_t* src = file->file.Data().data();

    std::lock_guard lock(file->mtx);
    const auto& ranges = file->written.Ranges();
    auto it = ranges.upper_bound(pos);
    if (it != ranges.begin() && std::prev(it)->second > pos) it = std::prev(it);

    Ranges result;
    for (; it != ranges.end() && it->first < end; ++it) {
        const std::size_t b = std::max(it->first, pos);
        const std::size_t e = std::min(it->second, end);
        if (b < e) {
            std::copy(src + b, src + e, out.begin() + (b - pos));
            result.emplace_back(b, e);
        }
    }
    return result;
}

ChunkAssembler::Ranges ChunkAssembler::GetReadyRanges(uint32_t id) const {
    auto file = FindFile(id);
    std::lock_guard lock(file->mtx);
    const auto& ranges = file->written.Ranges();
    return {ranges.begin(), ranges.end()};
}

bool ChunkAssembler::IsComplete(uint32_t id) const {
    auto file = FindFile(id);
    std::lock_guard lock(file->mtx);
    return file->written.Covered() == file->size;
}

std::optional<std::span<const uint8_t>> ChunkAssembler::View(uint32_t id) const{
    auto file = FindFile(id);
    std::lock_guard lock(file->mtx);
    if (file->written.Covered() != file->size) return std::nullopt;
    return std::span<const uint8_t>(file->file.Data());
}

bool ChunkAssembler::Flush(uint32_t id) {
    auto file = FindFile(id);
    std::lock_guard lock(file->mtx);
    if (file->written.Covered() != file->size) return false;
    file->file.Sync();
    return true;
}
