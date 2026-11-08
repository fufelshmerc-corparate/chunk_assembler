#include "interval_set.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <utility>
#include <vector>

using HolesVec = std::vector<std::pair<std::size_t, std::size_t>>;
using RangesMap = std::map<std::size_t, std::size_t>;

TEST(IntervalSetTest, EmptySetHasZeroCoverage) {
    IntervalSet s;
    EXPECT_EQ(s.Covered(), 0u);
}

// невалидные или пустые интервалы
TEST(IntervalSetTest, InvalidOrEmptyIntervals) {
    IntervalSet tracker;

    EXPECT_TRUE(tracker.Insert(5, 5).empty());
    EXPECT_TRUE(tracker.Insert(10, 5).empty());
    EXPECT_TRUE(tracker.Insert(0, 0).empty());
    EXPECT_TRUE(tracker.Ranges().empty());
    EXPECT_EQ(tracker.Covered(), 0u);
}

// вставка первого интервала
TEST(IntervalSetTest, SingleInsert) {
    IntervalSet tracker;

    EXPECT_EQ(tracker.Insert(10, 20), (HolesVec{{10, 20}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{10, 20}}));
    EXPECT_EQ(tracker.Covered(), 10u);
}

// непересекающиеся интервалы
TEST(IntervalSetTest, DisjointIntervals) {
    IntervalSet tracker;

    EXPECT_EQ(tracker.Insert(20, 30), (HolesVec{{20, 30}}));
    EXPECT_EQ(tracker.Insert(0, 10), (HolesVec{{0, 10}}));
    EXPECT_EQ(tracker.Insert(40, 50), (HolesVec{{40, 50}}));
    EXPECT_EQ(tracker.Insert(12, 18), (HolesVec{{12, 18}}));

    RangesMap expected_ranges = {{0, 10}, {12, 18}, {20, 30}, {40, 50}};
    EXPECT_EQ(tracker.Ranges(), expected_ranges);
    EXPECT_EQ(tracker.Covered(), 36u);
}

// смежные интервалы (20, 30)(30, 40)
TEST(IntervalSetTest, AdjacentIntervalsMerge) {
    IntervalSet tracker;
    tracker.Insert(10, 20);

    EXPECT_EQ(tracker.Insert(20, 30), (HolesVec{{20, 30}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{10, 30}}));

    EXPECT_EQ(tracker.Insert(5, 10), (HolesVec{{5, 10}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{5, 30}}));

    tracker.Insert(40, 50);
    EXPECT_EQ(tracker.Insert(30, 40), (HolesVec{{30, 40}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{5, 50}}));
    EXPECT_EQ(tracker.Covered(), 45u);
}

// дубликаты и вложенные интервалы
TEST(IntervalSetTest, SubsetsAndDuplicatesReturnNoHoles) {
    IntervalSet tracker;
    tracker.Insert(10, 30);

    EXPECT_TRUE(tracker.Insert(10, 30).empty());
    EXPECT_TRUE(tracker.Insert(15, 25).empty());
    EXPECT_TRUE(tracker.Insert(10, 20).empty());
    EXPECT_TRUE(tracker.Insert(20, 30).empty());

    EXPECT_EQ(tracker.Ranges(), (RangesMap{{10, 30}}));
    EXPECT_EQ(tracker.Covered(), 20u);
}

// частичное пересечение
TEST(IntervalSetTest, PartialOverlaps) {
    IntervalSet tracker;
    tracker.Insert(10, 20);

    EXPECT_EQ(tracker.Insert(5, 15), (HolesVec{{5, 10}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{5, 20}}));

    EXPECT_EQ(tracker.Insert(15, 25), (HolesVec{{20, 25}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{5, 25}}));
}

// поглощение одного интервала большим
TEST(IntervalSetTest, SupersetSingleInterval) {
    IntervalSet tracker;
    tracker.Insert(10, 20);

    HolesVec expected_holes = {{5, 10}, {20, 25}};
    EXPECT_EQ(tracker.Insert(5, 25), expected_holes);
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{5, 25}}));
}

// поглощение нескольких интервалов сразу
TEST(IntervalSetTest, SpanningMultipleIntervals) {
    IntervalSet tracker;
    tracker.Insert(10, 20);
    tracker.Insert(30, 40);
    tracker.Insert(50, 60);

    HolesVec expected_holes = {{5, 10}, {20, 30}, {40, 50}, {60, 65}};
    EXPECT_EQ(tracker.Insert(5, 65), expected_holes);
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{5, 65}}));
}

// пересечение от середины одного интервала до середины другого
TEST(IntervalSetTest, SpanFromMiddleToMiddle) {
    IntervalSet tracker;
    tracker.Insert(10, 20);
    tracker.Insert(30, 40);
    tracker.Insert(50, 60);

    HolesVec expected_holes = {{20, 30}, {40, 50}};
    EXPECT_EQ(tracker.Insert(15, 55), expected_holes);
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{10, 60}}));
}

// граничные значения типа size_t (0 и SIZE_MAX)
TEST(IntervalSetTest, BoundaryValuesZeroAndMax) {
    IntervalSet tracker;
    std::size_t max_val = std::numeric_limits<std::size_t>::max();

    EXPECT_EQ(tracker.Insert(0, 10), (HolesVec{{0, 10}}));
    EXPECT_EQ(tracker.Insert(max_val - 10, max_val), (HolesVec{{max_val - 10, max_val}}));

    EXPECT_EQ(tracker.Insert(0, max_val), (HolesVec{{10, max_val - 10}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{0, max_val}}));
}

// интервал справа начинается ровно в end
TEST(IntervalSetTest, RightAdjacentIntervalDoesNotExtendHole) {
    IntervalSet tracker;
    tracker.Insert(20, 30);

    EXPECT_EQ(tracker.Insert(10, 20), (HolesVec{{10, 20}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{10, 30}}));
}

// вставка частично перекрывает левый интервал и примыкает к правому
TEST(IntervalSetTest, OverlapOnLeftAndAdjacentOnRight) {
    IntervalSet tracker;
    tracker.Insert(10, 20);
    tracker.Insert(30, 40);

    EXPECT_EQ(tracker.Insert(15, 30), (HolesVec{{20, 30}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{10, 40}}));
}

// вставка начинается внутри уже покрытого диапазона а заканчивается до следующего диапазона
TEST(IntervalSetTest, ExtendsExistingRangeWithoutReachingNext) {
    IntervalSet tracker;
    tracker.Insert(10, 20);
    tracker.Insert(30, 40);

    EXPECT_EQ(tracker.Insert(15, 25), (HolesVec{{20, 25}}));
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{10, 25}, {30, 40}}));
}

// после объединения диапазонов повторная вставка внутри результата не должна возвращать дыр
TEST(IntervalSetTest, InsertInsidePreviouslyMergedRange) {
    IntervalSet tracker;
    tracker.Insert(10, 20);
    tracker.Insert(30, 40);
    tracker.Insert(50, 60);

    EXPECT_EQ(tracker.Insert(15, 55), (HolesVec{{20, 30}, {40, 50}}));
    EXPECT_TRUE(tracker.Insert(18, 58).empty());
    EXPECT_EQ(tracker.Ranges(), (RangesMap{{10, 60}}));
}
