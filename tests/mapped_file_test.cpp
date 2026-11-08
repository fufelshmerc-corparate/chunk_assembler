#include "mapped_file.h"
#include "test_utils.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

// файл создается нужного размера и заполнен нулями
TEST(MappedFileTest, CreatesFileOfGivenSize) {
    TempDir dir;
    MappedFile file(dir.File(1), 4096);
    EXPECT_EQ(file.Size(), 4096u);
    EXPECT_EQ(std::filesystem::file_size(dir.File(1)), 4096u);
    for (auto byte : file.Data()) EXPECT_EQ(byte, 0);
}
// запись в отображение попадает в файл на диске
TEST(MappedFileTest, WritesReachDisk) {
    TempDir dir;
    {
        MappedFile file(dir.File(1), 3);
        file.Data()[0] = 'a';
        file.Data()[1] = 'b';
        file.Data()[2] = 'c';
        file.Sync();
    }
    std::ifstream in(dir.File(1), std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "abc");
}
// размер 0: файл есть, отображения нет
TEST(MappedFileTest, ZeroSize) {
    TempDir dir;
    MappedFile file(dir.File(1), 0);
    EXPECT_TRUE(file.Data().empty());
    EXPECT_NO_THROW(file.Sync());
    EXPECT_TRUE(std::filesystem::exists(dir.File(1)));
}
// ошибка open -> system_error с errno
TEST(MappedFileTest, OpenErrorHasErrno) {
    TempDir dir;
    try {
        MappedFile file(dir.Path() / "missing" / "f.bin", 1);
        FAIL() << "expected system_error";
    } catch (const std::system_error& e) {
        EXPECT_EQ(e.code().value(), ENOENT);
    }
}
// перемещение передает владение
TEST(MappedFileTest, MoveTransfersOwnership) {
    TempDir dir;
    MappedFile a(dir.File(1), 16);
    a.Data()[0] = 42;
    MappedFile b = std::move(a);
    EXPECT_TRUE(a.Data().empty());
    ASSERT_EQ(b.Size(), 16u);
    EXPECT_EQ(b.Data()[0], 42);
    MappedFile c;
    c = std::move(b);
    EXPECT_EQ(c.Data()[0], 42);
}
