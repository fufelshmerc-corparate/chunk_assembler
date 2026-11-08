// Демо: собирает файл из перемешанных чанков в N потоков и печатает время.
// Запуск: ./demo <путь> [размер_МБ] [потоки] [чанк_КБ]
#include "chunk_assembler.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: demo <output> [size_mb=64] [threads=4] [chunk_kb=256]\n";
        return 1;
    }
    const std::size_t size = (argc > 2 ? std::stoul(argv[2]) : 64) * 1024 * 1024;
    const std::size_t threads = argc > 3 ? std::stoul(argv[3]) : 4;
    const std::size_t chunk = (argc > 4 ? std::stoul(argv[4]) : 256) * 1024;
    if (threads == 0 || chunk == 0) {
        std::cerr << "threads and chunk_kb must be > 0\n";
        return 1;
    }

    // Исходные данные и перемешанный порядок чанков
    std::vector<uint8_t> source(size);
    std::mt19937 rng(42);
    for (auto& b : source) b = static_cast<uint8_t>(rng());
    std::vector<std::size_t> positions;
    for (std::size_t pos = 0; pos < size; pos += chunk) positions.push_back(pos);
    std::shuffle(positions.begin(), positions.end(), rng);

    ChunkAssembler assembler;
    assembler.AddFile(1, argv[1], size);

    const auto start = std::chrono::steady_clock::now();
    {
        std::vector<std::jthread> workers;
        for (std::size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                for (std::size_t i = t; i < positions.size(); i += threads) {
                    const std::size_t pos = positions[i];
                    const std::size_t n = std::min(chunk, size - pos);
                    assembler.AddChunk(1, pos, std::span<const uint8_t>(source.data() + pos, n));
                }
            });
        }
    }
    const auto assembled = std::chrono::steady_clock::now();

    // msync меряется отдельно: это запись на диск, а не сборка
    const bool ok = assembler.Flush(1);
    const auto flushed = std::chrono::steady_clock::now();

    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    const auto assembleMs = duration_cast<milliseconds>(assembled - start).count();
    const auto flushMs = duration_cast<milliseconds>(flushed - assembled).count();

    const auto view = assembler.View(1);
    const bool same = view && std::equal(view->begin(), view->end(), source.begin());
    std::cout << "size=" << size / (1024 * 1024) << "MB threads=" << threads << " chunk=" << chunk / 1024
              << "KB assemble=" << assembleMs << "ms flush=" << flushMs << "ms complete=" << ok
              << " match=" << same << "\n";
    return ok && same ? 0 : 2;
}
