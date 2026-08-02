#include "history_manager.hpp"

void HistoryManager::Add(OCRResult result) {
    items_.insert(items_.begin(), std::move(result));
    if (items_.size() > 20) items_.resize(20);
}
void HistoryManager::Replace(std::vector<OCRResult> items) {
    items_ = std::move(items);
    if (items_.size() > 20) items_.resize(20);
}
