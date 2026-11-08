#include "chunk_assembler.h"
#include "test_utils.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

using Bytes = std::vector<uint8_t>;
using Ranges = std::vector<std::pair<std::size_t, std::size_t>>;

class ChunkAssemblerTest : public testing::Test {
protected:
    static constexpr uint32_t id = 42;
    static constexpr uint32_t unknownId = 99;
    static constexpr std::size_t kSize = 10;

    TempDir dir;
    ChunkAssembler collector;

    void SetUp() override { collector.AddFile(id, dir.File(id), kSize); }

    Bytes ReadBytes(uint32_t fileId, std::size_t pos, std::size_t n, Ranges* ranges = nullptr) {
        Bytes out(n);
        auto r = collector.Read(fileId, pos, out);
        if (ranges) *ranges = std::move(r);
        return out;
    }
};

// AddFile
//  размер kSize, ничего не готово
TEST_F(ChunkAssemblerTest, AddFileRegistersEmptyState) {
    collector.AddFile(1, dir.File(1), 5);
    EXPECT_FALSE(collector.IsComplete(1));
    EXPECT_TRUE(collector.GetReadyRanges(1).empty());
    EXPECT_NO_THROW(ReadBytes(1, 0, 5));
    EXPECT_THROW(ReadBytes(1, 0, 6), std::out_of_range);
}
//  регистрация пустого файла
TEST_F(ChunkAssemblerTest, AddFileEmptyFile) {
    collector.AddFile(1, dir.File(1), 0);
    EXPECT_TRUE(collector.IsComplete(1));
    EXPECT_THROW(ReadBytes(1, 0, 1), std::out_of_range);
}
//  повторная регистрация файла не ломает существующий
TEST_F(ChunkAssemblerTest, AddFileDuplicateId) {
    collector.AddChunk(id, 0, Bytes{7});
    EXPECT_THROW(collector.AddFile(id, dir.File(id), 3), std::invalid_argument);
    EXPECT_EQ(collector.GetReadyRanges(id), (Ranges{{0, 1}}));
    EXPECT_NO_THROW(ReadBytes(id, 0, kSize));
}

// Повторный ID отклоняется до выделения буфера даже при недопустимо большом размере.
TEST_F(ChunkAssemblerTest, AddFileDuplicateIdWithMaximumSize) {
    EXPECT_THROW(collector.AddFile(id, dir.File(id), std::numeric_limits<std::size_t>::max()),
                 std::invalid_argument);
}

// AddChunk
//  запись чанка
TEST_F(ChunkAssemblerTest, AddChunkWritesBytes) {
    collector.AddChunk(id, 2, Bytes{2, 3, 4});
    Ranges ranges;
    EXPECT_EQ(ReadBytes(id, 2, 3, &ranges), (Bytes{2, 3, 4}));
    EXPECT_EQ(ranges, (Ranges{{2, 5}}));
}
//  частичное перекрытие, дописываются только недостающие байты
TEST_F(ChunkAssemblerTest, AddChunkOverlapWritesOnlyMissingBytes) {
    collector.AddChunk(id, 2, Bytes{2, 3});
    collector.AddChunk(id, 1, Bytes{1, 9, 9, 4});
    EXPECT_EQ(ReadBytes(id, 1, 4), (Bytes{1, 2, 3, 4}));
    EXPECT_EQ(collector.GetReadyRanges(id), (Ranges{{1, 5}}));
}
//  повторная запись того же места не портит данные
TEST_F(ChunkAssemblerTest, AddChunkDuplicateDoesNotOverwrite) {
    collector.AddChunk(id, 0, Bytes{1, 2});
    collector.AddChunk(id, 0, Bytes{9, 9});
    EXPECT_EQ(ReadBytes(id, 0, 2), (Bytes{1, 2}));
}
//  чанк ровно до конца файла
TEST_F(ChunkAssemblerTest, AddChunkEndsAtFileBoundary) {
    collector.AddChunk(id, kSize - 2, Bytes{8, 9});
    EXPECT_EQ(ReadBytes(id, kSize - 2, 2), (Bytes{8, 9}));
}
//  пустой чанк внутри файла
TEST_F(ChunkAssemblerTest, AddChunkEmptyChunkIsNoop) {
    collector.AddChunk(id, 3, Bytes{});
    EXPECT_TRUE(collector.GetReadyRanges(id).empty());
    EXPECT_FALSE(collector.IsComplete(id));
}
//  пустой чанк на границе файла разрешён
TEST_F(ChunkAssemblerTest, AddChunkEmptyChunkAtEnd) {
    EXPECT_NO_THROW(collector.AddChunk(id, kSize, Bytes{}));
    EXPECT_TRUE(collector.GetReadyRanges(id).empty());
}
//  чанк выходит за конец файла
TEST_F(ChunkAssemblerTest, AddChunkExceedsFileSize) {
    EXPECT_THROW(collector.AddChunk(id, kSize - 1, Bytes{1, 2}), std::out_of_range);
    EXPECT_TRUE(collector.GetReadyRanges(id).empty());
}
//  позиция за концом файла
TEST_F(ChunkAssemblerTest, AddChunkPositionPastEnd) {
    EXPECT_THROW(collector.AddChunk(id, kSize + 1, Bytes{}), std::out_of_range);
}
//  переполнение pos + size
TEST_F(ChunkAssemblerTest, AddChunkMaximumPosition) {
    EXPECT_THROW(collector.AddChunk(id, std::numeric_limits<std::size_t>::max(), Bytes{1, 2}),
                 std::out_of_range);
}
//  неизвестный файл
TEST_F(ChunkAssemblerTest, AddChunkUnknownFile) {
    EXPECT_THROW(collector.AddChunk(unknownId, 0, Bytes{1}), std::invalid_argument);
}

// IsComplete
TEST_F(ChunkAssemblerTest, IsCompleteNewFile) { EXPECT_FALSE(collector.IsComplete(id)); }
TEST_F(ChunkAssemblerTest, IsCompletePartialFile) {
    collector.AddChunk(id, 2, Bytes{2, 3});
    EXPECT_FALSE(collector.IsComplete(id));
}
TEST_F(ChunkAssemblerTest, IsCompleteFullFile) {
    collector.AddChunk(id, 0, Bytes(kSize, 1));
    EXPECT_TRUE(collector.IsComplete(id));
}
//  сборка из чанков в произвольном порядке: готов только после последнего
TEST_F(ChunkAssemblerTest, IsCompleteAfterOutOfOrderChunks) {
    collector.AddChunk(id, 6, Bytes{6, 7, 8, 9});
    EXPECT_FALSE(collector.IsComplete(id));
    collector.AddChunk(id, 0, Bytes{0, 1, 2});
    EXPECT_FALSE(collector.IsComplete(id));
    collector.AddChunk(id, 3, Bytes{3, 4, 5});
    EXPECT_TRUE(collector.IsComplete(id));
}
TEST_F(ChunkAssemblerTest, IsCompleteEmptyFile) {
    collector.AddFile(1, dir.File(1), 0);
    EXPECT_TRUE(collector.IsComplete(1));
}
TEST_F(ChunkAssemblerTest, IsCompleteUnknownFile) {
    EXPECT_THROW(collector.IsComplete(unknownId), std::invalid_argument);
}

// GetReadyRanges
TEST_F(ChunkAssemblerTest, GetReadyRangesNewFile) { EXPECT_TRUE(collector.GetReadyRanges(id).empty()); }
TEST_F(ChunkAssemblerTest, GetReadyRangesSortedRanges) {
    collector.AddChunk(id, 7, Bytes{7, 8});
    collector.AddChunk(id, 2, Bytes{2, 3});
    EXPECT_EQ(collector.GetReadyRanges(id), (Ranges{{2, 4}, {7, 9}}));
}
TEST_F(ChunkAssemblerTest, GetReadyRangesAdjacentChunks) {
    collector.AddChunk(id, 2, Bytes{2, 3});
    collector.AddChunk(id, 4, Bytes{4, 5});
    EXPECT_EQ(collector.GetReadyRanges(id), (Ranges{{2, 6}}));
}
TEST_F(ChunkAssemblerTest, GetReadyRangesUnknownFile) {
    EXPECT_THROW(collector.GetReadyRanges(unknownId), std::invalid_argument);
}

// Read
TEST_F(ChunkAssemblerTest, ReadNewFile) {
    Ranges ranges;
    ReadBytes(id, 0, kSize, &ranges);
    EXPECT_TRUE(ranges.empty());
}
TEST_F(ChunkAssemblerTest, ReadReadyRange) {
    collector.AddChunk(id, 2, Bytes{2, 3, 4});
    Ranges ranges;
    EXPECT_EQ(ReadBytes(id, 2, 3, &ranges), (Bytes{2, 3, 4}));
    EXPECT_EQ(ranges, (Ranges{{2, 5}}));
}
//  готовый диапазон обрезается границами чтения
TEST_F(ChunkAssemblerTest, ReadClipsReadyRange) {
    collector.AddChunk(id, 2, Bytes{2, 3, 4, 5});
    Ranges ranges;
    EXPECT_EQ(ReadBytes(id, 3, 2, &ranges), (Bytes{3, 4}));
    EXPECT_EQ(ranges, (Ranges{{3, 5}}));
}
//  байты в out со смещением относительно pos
TEST_F(ChunkAssemblerTest, ReadCopiesAtRelativeOffsets) {
    collector.AddChunk(id, 4, Bytes{4, 5});
    Ranges ranges;
    const Bytes out = ReadBytes(id, 2, 5, &ranges);
    EXPECT_EQ(ranges, (Ranges{{4, 6}}));
    EXPECT_EQ(out[2], 4);
    EXPECT_EQ(out[3], 5);
}
//  чтение с дырками, несколько диапазонов
TEST_F(ChunkAssemblerTest, ReadMultipleRangesWithHoles) {
    collector.AddChunk(id, 2, Bytes{2, 3});
    collector.AddChunk(id, 7, Bytes{7, 8});
    Ranges ranges;
    const Bytes out = ReadBytes(id, 0, kSize, &ranges);
    EXPECT_EQ(ranges, (Ranges{{2, 4}, {7, 9}}));
    EXPECT_EQ(out[2], 2);
    EXPECT_EQ(out[3], 3);
    EXPECT_EQ(out[7], 7);
    EXPECT_EQ(out[8], 8);
}
//  чтение промежутка между готовыми диапазонами
TEST_F(ChunkAssemblerTest, ReadGap) {
    collector.AddChunk(id, 2, Bytes{2, 3});
    collector.AddChunk(id, 7, Bytes{7, 8});
    Ranges ranges;
    ReadBytes(id, 4, 3, &ranges);
    EXPECT_TRUE(ranges.empty());
}
//  пустой буфер на границе файла разрешён
TEST_F(ChunkAssemblerTest, ReadEmptyBufferAtEnd) {
    EXPECT_TRUE(collector.Read(id, kSize, std::span<uint8_t>{}).empty());
}
TEST_F(ChunkAssemblerTest, ReadExceedsFileSize) {
    EXPECT_THROW(ReadBytes(id, kSize - 1, 2), std::out_of_range);
}
TEST_F(ChunkAssemblerTest, ReadPositionPastEnd) {
    EXPECT_THROW(collector.Read(id, kSize + 1, std::span<uint8_t>{}), std::out_of_range);
}
TEST_F(ChunkAssemblerTest, ReadMaximumPosition) {
    EXPECT_THROW(ReadBytes(id, std::numeric_limits<std::size_t>::max(), 2), std::out_of_range);
}
TEST_F(ChunkAssemblerTest, ReadUnknownFile) {
    EXPECT_THROW(ReadBytes(unknownId, 0, 1), std::invalid_argument);
}

// View
TEST_F(ChunkAssemblerTest, ViewNewFile) { EXPECT_FALSE(collector.View(id)); }
TEST_F(ChunkAssemblerTest, ViewPartialFile) {
    collector.AddChunk(id, 2, Bytes{2, 3});
    EXPECT_FALSE(collector.View(id));
}
TEST_F(ChunkAssemblerTest, ViewFullFile) {
    const Bytes expected{0, 1, 2, 3, 4, 5, 6, 7, 8, 255};
    collector.AddChunk(id, 0, Bytes(expected));
    const auto file = collector.View(id);
    ASSERT_TRUE(file);
    EXPECT_EQ(ToBytes(*file), expected);
}
//  View, оба вызова отдают один и тот же буфер
TEST_F(ChunkAssemblerTest, ViewReturnsSameBufferWithoutCopy) {
    collector.AddChunk(id, 0, Bytes(kSize, 1));
    const auto first = collector.View(id);
    const auto second = collector.View(id);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->data(), second->data());
}
TEST_F(ChunkAssemblerTest, ViewEmptyFile) {
    collector.AddFile(1, dir.File(1), 0);
    const auto file = collector.View(1);
    ASSERT_TRUE(file);
    EXPECT_TRUE(file->empty());
}
TEST_F(ChunkAssemblerTest, ViewUnknownFile) {
    EXPECT_THROW(collector.View(unknownId), std::invalid_argument);
}

// Flush и файл на диске
TEST_F(ChunkAssemblerTest, FlushIncompleteFileReturnsFalse) {
    collector.AddChunk(id, 0, Bytes{1, 2});
    EXPECT_FALSE(collector.Flush(id));
}
TEST_F(ChunkAssemblerTest, FileOnDiskHasRightBytesAfterFlush) {
    const Bytes expected{9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    collector.AddChunk(id, 5, Bytes(expected.begin() + 5, expected.end()));
    collector.AddChunk(id, 0, Bytes(expected.begin(), expected.begin() + 5));
    ASSERT_TRUE(collector.Flush(id));
    std::ifstream in(dir.File(id), std::ios::binary);
    const Bytes onDisk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(onDisk, expected);
}
TEST_F(ChunkAssemblerTest, EmptyFileIsCreatedOnDisk) {
    collector.AddFile(1, dir.File(1), 0);
    EXPECT_TRUE(collector.Flush(1));
    EXPECT_TRUE(std::filesystem::exists(dir.File(1)));
    EXPECT_EQ(std::filesystem::file_size(dir.File(1)), 0u);
}
TEST_F(ChunkAssemblerTest, AddFileBadPathThrowsSystemError) {
    EXPECT_THROW(collector.AddFile(1, dir.Path() / "no_such_dir" / "f.bin", 4), std::system_error);
    EXPECT_THROW(collector.IsComplete(1), std::invalid_argument);  // не зарегистрирован
}
TEST_F(ChunkAssemblerTest, FlushUnknownFile) {
    EXPECT_THROW(collector.Flush(unknownId), std::invalid_argument);
}
