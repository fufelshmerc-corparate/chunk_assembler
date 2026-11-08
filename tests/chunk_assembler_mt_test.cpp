#include "chunk_assembler.h"
#include "test_utils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using Ranges = std::vector<std::pair<std::size_t, std::size_t>>;

constexpr std::size_t fileSize = 1024 * 1024;
constexpr std::size_t chunkSize = 4 * 1024;
static_assert(fileSize % chunkSize == 0, "fileSize must be a multiple of chunkSize");

Bytes MakeRandomFile(std::size_t size = fileSize) {
    Bytes reference(size);
    std::mt19937 random(69);
    for (auto& byte : reference) {
        byte = static_cast<uint8_t>(random() >> 24);
    }
    return reference;
}

std::vector<std::size_t> ShuffledChunks(std::size_t size = fileSize, std::size_t chunk = chunkSize) {
    std::vector<std::size_t> chunks(size / chunk);
    std::iota(chunks.begin(), chunks.end(), 0);
    std::mt19937 random(67);
    std::shuffle(chunks.begin(), chunks.end(), random);
    return chunks;
}

// Каждый тест получает свою временную папку
class ChunkAssemblerMtTest : public testing::Test {
protected:
    TempDir dir;
};

// AddFile
//   регистрация разных файлов из двух потоков
TEST_F(ChunkAssemblerMtTest, AddFileDifferentIds) {
    ChunkAssembler collector;
    std::latch start(1);

    std::thread first([&] {
        start.wait();
        EXPECT_NO_THROW(collector.AddFile(1, dir.File(1), 4));
    });
    std::thread second([&] {
        start.wait();
        EXPECT_NO_THROW(collector.AddFile(2, dir.File(2), 6));
    });

    start.count_down();
    first.join();
    second.join();

    Bytes firstOut(4);
    Bytes secondOut(6);
    EXPECT_NO_THROW(collector.Read(1, 0, firstOut));
    EXPECT_NO_THROW(collector.Read(2, 0, secondOut));
}

//  гонка восьми потоков за один ID: должна быть одна успешная регистрация
TEST_F(ChunkAssemblerMtTest, AddFileSameIdFromEightThreads) {
    constexpr int NumberOfThreads = 8;
    ChunkAssembler collector;
    std::barrier start(NumberOfThreads);
    std::atomic<int> successes{0};
    std::atomic<int> duplicates{0};
    std::atomic<int> unexpected{0};
    std::vector<std::jthread> threads;

    for (int i = 0; i < NumberOfThreads; ++i) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            try {
                collector.AddFile(1, dir.File(1), fileSize);
                ++successes;
            } catch (const std::invalid_argument&) {
                ++duplicates;
            } catch (...) {
                ++unexpected;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(successes.load(), 1);
    EXPECT_EQ(duplicates.load(), NumberOfThreads - 1);
    EXPECT_EQ(unexpected.load(), 0);
}

// AddChunk
//   запись двух непересекающихся чанков одного файла из разных потоков
TEST_F(ChunkAssemblerMtTest, AddChunkDifferentRanges) {
    constexpr std::size_t kHalf = 64 * 1024;
    constexpr int kIterations = 200;
    for (int iter = 0; iter < kIterations; ++iter) {
        ChunkAssembler collector;
        collector.AddFile(1, dir.File(1), 2 * kHalf);
        std::latch start(1);
        {
            std::jthread first([&] {
                start.wait();
                EXPECT_NO_THROW(collector.AddChunk(1, 0, Bytes(kHalf, 1)));
            });
            std::jthread second([&] {
                start.wait();
                EXPECT_NO_THROW(collector.AddChunk(1, kHalf, Bytes(kHalf, 2)));
            });
            start.count_down();
        }
        Bytes expected(2 * kHalf, 2);
        std::fill(expected.begin(), expected.begin() + kHalf, 1);
        const auto file = collector.View(1);
        ASSERT_TRUE(file);
        EXPECT_EQ(ToBytes(*file), expected);
    }
}

//  запись в один диапазон: сохраняется целиком чанк одного из потоков(нельзя 1, 2, 1, 1)
TEST_F(ChunkAssemblerMtTest, AddChunkSameRange) {
    constexpr std::size_t kChunk = 64 * 1024;
    for (int iter = 0; iter < 1000; ++iter) {
        ChunkAssembler collector;
        collector.AddFile(1, dir.File(1), kChunk);
        std::latch start(1);
        {
            std::jthread first([&] {
                start.wait();
                EXPECT_NO_THROW(collector.AddChunk(1, 0, Bytes(kChunk, 1)));
            });
            std::jthread second([&] {
                start.wait();
                EXPECT_NO_THROW(collector.AddChunk(1, 0, Bytes(kChunk, 2)));
            });
            start.count_down();
        }
        const auto file = collector.View(1);
        ASSERT_TRUE(file);
        EXPECT_TRUE(ToBytes(*file) == Bytes(kChunk, 1) || ToBytes(*file) == Bytes(kChunk, 2));
    }
}

//  сборка файла 1 МБ из перемешанных чанков по 4 КБ в 8 потоков
TEST_F(ChunkAssemblerMtTest, AssembleOneMegabyteFromEightThreads) {
    constexpr std::size_t NumberOfThreads = 8;
    const auto reference = MakeRandomFile();
    const auto chunks = ShuffledChunks();
    ChunkAssembler collector;
    collector.AddFile(1, dir.File(1), reference.size());
    std::barrier start(static_cast<std::ptrdiff_t>(NumberOfThreads));
    std::vector<std::jthread> threads;

    for (std::size_t worker = 0; worker < NumberOfThreads; ++worker) {
        threads.emplace_back([&, worker] {
            start.arrive_and_wait();
            for (auto index = worker; index < chunks.size(); index += NumberOfThreads) {
                const auto pos = chunks[index] * chunkSize;
                EXPECT_NO_THROW(collector.AddChunk(
                    1, pos, Bytes(reference.begin() + pos, reference.begin() + pos + chunkSize)));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(collector.IsComplete(1));
    const auto file = collector.View(1);
    ASSERT_TRUE(file);
    EXPECT_EQ(ToBytes(*file), reference);
}

// 8 потоков шлют все чанки в рандомном порядке и с перекрытием
TEST_F(ChunkAssemblerMtTest, OverlappingAndDuplicateChunksFromEightThreads) {
    constexpr std::size_t NumberOfThreads = 8;
    const auto reference = MakeRandomFile();
    ChunkAssembler collector;
    collector.AddFile(1, dir.File(1), reference.size());
    std::barrier start(static_cast<std::ptrdiff_t>(NumberOfThreads));
    std::vector<std::jthread> threads;

    for (std::size_t worker = 0; worker < NumberOfThreads; ++worker) {
        threads.emplace_back([&, worker] {
            auto chunks = ShuffledChunks();
            std::mt19937 random(static_cast<uint32_t>(100 + worker));
            std::shuffle(chunks.begin(), chunks.end(), random);
            start.arrive_and_wait();

            for (const auto chunk : chunks) {
                const auto begin = chunk * chunkSize;
                const auto end = std::min(begin + 2 * chunkSize,
                                          reference.size());  // по 2 чанка запись, чтобы было перекрытие
                for (int repeat = 0; repeat < 2; ++repeat) {
                    EXPECT_NO_THROW(collector.AddChunk(
                        1, begin, Bytes(reference.begin() + begin, reference.begin() + end)));
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(collector.IsComplete(1));
    EXPECT_EQ(collector.GetReadyRanges(1), (Ranges{{0, reference.size()}}));
    const auto file = collector.View(1);
    ASSERT_TRUE(file);
    EXPECT_EQ(ToBytes(*file), reference);
}

// Много файлов
//  Тестирование регистрации 256 новых файлов одновременно с записью в четыре существующих файла
TEST_F(ChunkAssemblerMtTest, RegisterNewFilesWhileWritingExistingFiles) {
    constexpr std::size_t registrarCount = 4;
    constexpr std::size_t writerCount = 4;
    constexpr std::size_t filesPerRegistrar = 64;
    constexpr std::size_t newFileSize = 4 * chunkSize;
    constexpr uint32_t firstNewId = 1000;
    ChunkAssembler collector;

    // У каждого ID свой эталонный файл
    auto makeFile = [](uint32_t id, std::size_t size) {
        std::mt19937 random(id);
        Bytes bytes(size);
        for (auto& byte : bytes) {
            byte = static_cast<uint8_t>(random() >> 24);
        }
        return bytes;
    };

    std::vector<Bytes> existingFiles;
    for (uint32_t id = 0; id < writerCount; ++id) {
        collector.AddFile(id, dir.File(id), fileSize);
        existingFiles.push_back(makeFile(id, fileSize));
    }

    // раунд содержит и регистрацию и поиск
    std::barrier round(static_cast<std::ptrdiff_t>(registrarCount + writerCount));
    std::vector<std::jthread> threads;
    for (std::size_t worker = 0; worker < registrarCount; ++worker) {
        threads.emplace_back([&, worker] {
            for (std::size_t index = 0; index < filesPerRegistrar; ++index) {
                const auto id = firstNewId + static_cast<uint32_t>(worker * filesPerRegistrar + index);
                const auto reference = makeFile(id, newFileSize);
                round.arrive_and_wait();
                EXPECT_NO_THROW(collector.AddFile(id, dir.File(id), reference.size()));
                for (std::size_t pos = 0; pos < reference.size(); pos += chunkSize) {
                    EXPECT_NO_THROW(collector.AddChunk(
                        id, pos, Bytes(reference.begin() + pos, reference.begin() + pos + chunkSize)));
                }
            }
        });
    }

    for (uint32_t id = 0; id < writerCount; ++id) {
        threads.emplace_back([&, id] {
            const auto& reference = existingFiles[id];
            constexpr auto chunksPerRound = fileSize / chunkSize / filesPerRegistrar;
            for (std::size_t index = 0; index < filesPerRegistrar; ++index) {
                round.arrive_and_wait();
                for (std::size_t chunk = 0; chunk < chunksPerRound; ++chunk) {
                    const auto pos = (index * chunksPerRound + chunk) * chunkSize;
                    EXPECT_NO_THROW(collector.AddChunk(
                        id, pos, Bytes(reference.begin() + pos, reference.begin() + pos + chunkSize)));
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    for (uint32_t id = 0; id < writerCount; ++id) {
        SCOPED_TRACE(testing::Message() << "existing file id=" << id);
        EXPECT_TRUE(collector.IsComplete(id));
        const auto file = collector.View(id);
        ASSERT_TRUE(file);
        EXPECT_EQ(ToBytes(*file), existingFiles[id]);
    }
    for (std::size_t index = 0; index < registrarCount * filesPerRegistrar; ++index) {
        const auto id = firstNewId + static_cast<uint32_t>(index);
        SCOPED_TRACE(testing::Message() << "new file id=" << id);
        EXPECT_TRUE(collector.IsComplete(id));
        const auto file = collector.View(id);
        ASSERT_TRUE(file);
        EXPECT_EQ(ToBytes(*file), makeFile(id, newFileSize));
    }
}

// Read
//  чтение готового файла несколькими потоками в отдельные буферы
TEST_F(ChunkAssemblerMtTest, ReadFromManyThreads) {
    constexpr std::size_t kReaders = 8;
    constexpr int kReadsPerThread = 50;
    const auto reference = MakeRandomFile();
    ChunkAssembler collector;
    collector.AddFile(1, dir.File(1), reference.size());
    collector.AddChunk(1, 0, Bytes(reference));
    std::barrier start(static_cast<std::ptrdiff_t>(kReaders));
    {
        std::vector<std::jthread> threads;
        for (std::size_t r = 0; r < kReaders; ++r) {
            threads.emplace_back([&] {
                Bytes out(reference.size());
                start.arrive_and_wait();
                for (int i = 0; i < kReadsPerThread; ++i) {
                    Ranges ranges;
                    EXPECT_NO_THROW(ranges = collector.Read(1, 0, out));
                    EXPECT_EQ(ranges, (Ranges{{0, reference.size()}}));
                    EXPECT_EQ(out, reference);
                }
            });
        }
    }
}

// читатели видят согласованное состояние при записи чанков 128 КиБ через reserve, copy, commit
TEST_F(ChunkAssemblerMtTest, WritersAndReadersPreserveInvariants) {
    constexpr std::size_t kWriters = 4;
    constexpr std::size_t kReaders = 4;
    constexpr std::size_t kFileSize = 8 * 1024 * 1024;
    constexpr std::size_t kChunkSize = 128 * 1024;
    static_assert(kFileSize % kChunkSize == 0);
    const auto reference = MakeRandomFile(kFileSize);
    const auto chunks = ShuffledChunks(kFileSize, kChunkSize);

    ChunkAssembler collector;
    collector.AddFile(1, dir.File(1), reference.size());

    std::atomic<std::size_t> writersLeft{kWriters};
    std::barrier sync(static_cast<std::ptrdiff_t>(kWriters + kReaders));
    std::vector<std::jthread> threads;

    for (std::size_t w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            sync.arrive_and_wait();
            bool first = true;
            for (auto i = w; i < chunks.size(); i += kWriters) {
                const auto pos = chunks[i] * kChunkSize;
                collector.AddChunk(1, pos,
                                     Bytes(reference.begin() + pos, reference.begin() + pos + kChunkSize));
                if (first) {
                    sync.arrive_and_wait();
                    first = false;
                }
            }
            --writersLeft;
        });
    }

    for (std::size_t r = 0; r < kReaders; ++r) {
        threads.emplace_back([&, r] {
            Bytes out(2 * kChunkSize);
            bool seenComplete = false;
            std::size_t step = r;

            auto check = [&] {
                // 3079 - простое число, чтобы читать вразброс
                const auto pos = (step++ * 3079) % (reference.size() - out.size() + 1);
                for (const auto& [b, e] : collector.Read(1, pos, out)) {
                    EXPECT_TRUE(
                        std::equal(out.begin() + (b - pos), out.begin() + (e - pos), reference.begin() + b));
                }
                const bool complete = collector.IsComplete(1);
                if (const auto file = collector.View(1)) {
                    EXPECT_EQ(ToBytes(*file), reference);
                } else {
                    EXPECT_FALSE(seenComplete);
                }
                if (seenComplete) {
                    EXPECT_TRUE(complete);
                }
                seenComplete = seenComplete || complete;
                if (const auto file = collector.View(1)) {
                    EXPECT_EQ(ToBytes(*file), reference);
                } else {
                    EXPECT_FALSE(seenComplete);
                }
            };

            sync.arrive_and_wait();
            EXPECT_FALSE(collector.IsComplete(1));
            check();
            sync.arrive_and_wait();

            while (writersLeft.load() != 0) check();
            check();
            EXPECT_TRUE(seenComplete);
        });
    }
}

//  большие и маленькие чанки пишут один файл 10 МБ пересекаясь
TEST_F(ChunkAssemblerMtTest, BigAndSmallChunksOverlap) {
    constexpr std::size_t kFileSize = 10 * 1024 * 1024;
    constexpr std::size_t kBigChunk = 256 * 1024;
    constexpr std::size_t kSmallChunk = 4 * 1024;
    constexpr std::size_t kBigWriters = 2;
    constexpr std::size_t kSmallWriters = 4;

    Bytes reference(kFileSize);
    std::mt19937 random(69);
    for (auto& byte : reference) byte = static_cast<uint8_t>(random() >> 24);

    auto shuffledPositions = [&](std::size_t chunk, uint32_t seed) {
        std::vector<std::size_t> positions;
        for (std::size_t pos = 0; pos < kFileSize; pos += chunk) positions.push_back(pos);
        std::shuffle(positions.begin(), positions.end(), std::mt19937(seed));
        return positions;
    };

    auto writeAll = [&](ChunkAssembler& collector, std::size_t chunk, uint32_t seed) {
        for (const auto pos : shuffledPositions(chunk, seed)) {
            EXPECT_NO_THROW(collector.AddChunk(
                1, pos, Bytes(reference.begin() + pos, reference.begin() + pos + chunk)));
        }
    };

    ChunkAssembler collector;
    collector.AddFile(1, dir.File(1), kFileSize);
    std::barrier start(static_cast<std::ptrdiff_t>(kBigWriters + kSmallWriters));
    {
        std::vector<std::jthread> threads;
        for (std::size_t w = 0; w < kBigWriters; ++w) {
            threads.emplace_back([&, w] {
                start.arrive_and_wait();
                writeAll(collector, kBigChunk, static_cast<uint32_t>(100 + w));
            });
        }
        for (std::size_t w = 0; w < kSmallWriters; ++w) {
            threads.emplace_back([&, w] {
                start.arrive_and_wait();
                writeAll(collector, kSmallChunk, static_cast<uint32_t>(200 + w));
            });
        }
    }

    EXPECT_TRUE(collector.IsComplete(1));
    const auto file = collector.View(1);
    ASSERT_TRUE(file);
    EXPECT_EQ(ToBytes(*file), reference);
}

}  // namespace
