#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

// Множество полуинтервалов [begin, end).
// отрезки в ranges_ не пересекаются и не касаются друг друга
// (соседние склеиваются), covered_ равен сумме их длин.
class IntervalSet {
public:
    using Holes = std::vector<std::pair<std::size_t, std::size_t>>;

    // Отрезки по возрастанию: ключ это начало (включительно), значение это конец (не включительно).
    const std::map<std::size_t, std::size_t>& Ranges() const { return ranges_; }

    // Суммарная длина всех отрезков.
    std::size_t Covered() const { return covered_; }

    // Добавляет [begin, end) и возвращает дырки - части диапазона, которых раньше не было.
    // Пустой или неверный диапазон (begin >= end) ничего не делает.
    Holes Insert(std::size_t begin, std::size_t end) {
        if (begin >= end) return {};

        Holes holes;
        std::size_t curr = begin;       
        std::size_t new_begin = begin;
        std::size_t new_end = end;

        auto it = ranges_.upper_bound(begin);
        if (it != ranges_.begin() && std::prev(it)->second >= begin) it = std::prev(it);
        while (it != ranges_.end() && it->first <= end) {
            if (curr < it->first) {
                holes.emplace_back(curr, it->first);
                covered_ += it->first - curr;
            }
            curr = std::max(curr, it->second);
            new_begin = std::min(new_begin, it->first);
            new_end = std::max(new_end, it->second);
            it = ranges_.erase(it);
        }
        if (curr < end) {
            holes.emplace_back(curr, end);
            covered_ += end - curr;
        }
        ranges_[new_begin] = new_end;
        return holes;
    }

private:
    std::map<std::size_t, std::size_t> ranges_;
    std::size_t covered_ = 0;
};
