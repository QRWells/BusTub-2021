//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_replacer.cpp
//
// Identification: src/buffer/lru_replacer.cpp
//
// Copyright (c) 2015-2019, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/lru_replacer.h"
#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace bustub {

LRUReplacer::LRUReplacer(size_t num_pages) : capacity_(num_pages) {}

LRUReplacer::~LRUReplacer() = default;

bool LRUReplacer::Victim(frame_id_t *frame_id) {
  std::shared_lock lock(mutex_);
  if (list_.empty() || map_.empty()) {
    return false;
  }
  *frame_id = list_.back();
  map_.erase(list_.back());
  list_.pop_back();
  return true;
}

void LRUReplacer::Pin(frame_id_t frame_id) {
  std::unique_lock lock(mutex_);
  auto ex = map_.find(frame_id);
  if (ex != map_.end()) {
    map_.erase(frame_id);
    list_.erase(ex->second);
  }
}

void LRUReplacer::Unpin(frame_id_t frame_id) {
  std::unique_lock lock(mutex_);
  auto ex = map_.find(frame_id);
  if (ex == map_.end()) {
    if (list_.size() >= capacity_) {
      map_.erase(list_.back());
      list_.pop_back();
    }
    list_.emplace_front(frame_id);
    map_.emplace(frame_id, list_.begin());
  }
}

size_t LRUReplacer::Size() {
  std::shared_lock lock(mutex_);
  return list_.size();
}

}  // namespace bustub
