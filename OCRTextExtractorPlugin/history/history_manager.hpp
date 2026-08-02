#pragma once
#include "common/types.hpp"
#include <vector>

class HistoryManager {
public:
    void Add(OCRResult result);
    const std::vector<OCRResult>& Items() const { return items_; }
    void Replace(std::vector<OCRResult> items);
private:
    std::vector<OCRResult> items_;
};
