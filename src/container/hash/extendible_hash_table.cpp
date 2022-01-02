//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// extendible_hash_table.cpp
//
// Identification: src/container/hash/extendible_hash_table.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "container/hash/extendible_hash_table.h"

namespace bustub {

template <typename KeyType, typename ValueType, typename KeyComparator>
HASH_TABLE_TYPE::ExtendibleHashTable(const std::string &name, BufferPoolManager *buffer_pool_manager,
                                     const KeyComparator &comparator, HashFunction<KeyType> hash_fn)
    : buffer_pool_manager_(buffer_pool_manager), comparator_(comparator), hash_fn_(std::move(hash_fn)) {
  auto dir_page = buffer_pool_manager_->NewPage(&directory_page_id_);
  auto dir_page_data = reinterpret_cast<HashTableDirectoryPage *>(dir_page->GetData());

  // update directory page
  dir_page_data->IncrGlobalDepth();
  dir_page_data->SetPageId(directory_page_id_);

  // init two buckets
  page_id_t bucket_0_page_id;
  buffer_pool_manager_->NewPage(&bucket_0_page_id);
  dir_page_data->SetBucketPageId(0, bucket_0_page_id);
  dir_page_data->SetLocalDepth(0, 1);

  page_id_t bucket_1_page_id;
  buffer_pool_manager_->NewPage(&bucket_1_page_id);
  dir_page_data->SetBucketPageId(1, bucket_1_page_id);
  dir_page_data->SetLocalDepth(1, 1);

  // unpin pages
  buffer_pool_manager_->UnpinPage(bucket_0_page_id, false);
  buffer_pool_manager_->UnpinPage(bucket_1_page_id, false);
  buffer_pool_manager_->UnpinPage(directory_page_id_, true);
}

/*****************************************************************************
 * HELPERS
 *****************************************************************************/
/**
 * Hash - simple helper to downcast MurmurHash's 64-bit hash to 32-bit
 * for extendible hashing.
 *
 * @param key the key to hash
 * @return the downcasted 32-bit hash
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
uint32_t HASH_TABLE_TYPE::Hash(KeyType key) {
  return static_cast<uint32_t>(hash_fn_.GetHash(key));
}

template <typename KeyType, typename ValueType, typename KeyComparator>
inline uint32_t HASH_TABLE_TYPE::KeyToDirectoryIndex(KeyType key, HashTableDirectoryPage *dir_page) {
  return Hash(key) & dir_page->GetGlobalDepthMask();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
inline uint32_t HASH_TABLE_TYPE::KeyToPageId(KeyType key, HashTableDirectoryPage *dir_page) {
  return dir_page->GetBucketPageId(KeyToDirectoryIndex(key, dir_page));
}

template <typename KeyType, typename ValueType, typename KeyComparator>
HashTableDirectoryPage *HASH_TABLE_TYPE::FetchDirectoryPage() {
  return reinterpret_cast<HashTableDirectoryPage *>(buffer_pool_manager_->FetchPage(directory_page_id_)->GetData());
}

template <typename KeyType, typename ValueType, typename KeyComparator>
std::pair<Page *, HASH_TABLE_BUCKET_TYPE *> HASH_TABLE_TYPE::FetchBucketPage(page_id_t bucket_page_id) {
  auto bucket_page = buffer_pool_manager_->FetchPage(bucket_page_id);
  auto bucket_page_data = reinterpret_cast<HASH_TABLE_BUCKET_TYPE *>(bucket_page->GetData());
  return std::make_pair(bucket_page, bucket_page_data);
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::GetValue(Transaction *transaction, const KeyType &key, std::vector<ValueType> *result) {
  table_latch_.RLock();

  auto dir_page_data = FetchDirectoryPage();
  auto bucket_page_id = KeyToPageId(key, dir_page_data);
  auto [bucket_page, bucket_page_data] = FetchBucketPage(bucket_page_id);

  bucket_page->RLatch();

  auto success = bucket_page_data->GetValue(key, comparator_, result);
  buffer_pool_manager_->UnpinPage(bucket_page_id, false);
  buffer_pool_manager_->UnpinPage(directory_page_id_, false);

  bucket_page->RUnlatch();
  table_latch_.RUnlock();

  return success;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::Insert(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.RLock();

  auto dir_page_data = FetchDirectoryPage();
  auto bucket_page_id = KeyToPageId(key, dir_page_data);
  auto [bucket_page, bucket_page_data] = FetchBucketPage(bucket_page_id);

  bucket_page->WLatch();

  // if the bucket is full, the insertion is handed over to SplitInsert() to complete.
  if (bucket_page_data->IsFull()) {
    buffer_pool_manager_->UnpinPage(bucket_page_id, false);
    buffer_pool_manager_->UnpinPage(directory_page_id_, false);
    bucket_page->WUnlatch();
    table_latch_.RUnlock();
    return SplitInsert(transaction, key, value);
  }

  auto success = bucket_page_data->Insert(key, value, comparator_);
  buffer_pool_manager_->UnpinPage(bucket_page_id, success);
  buffer_pool_manager_->UnpinPage(directory_page_id_, false);
  bucket_page->WUnlatch();
  table_latch_.RUnlock();

  return success;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::SplitInsert(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.WLock();

  auto success = false;
  auto is_growing = false;
  auto dir_page_data = FetchDirectoryPage();

  auto old_global_depth = dir_page_data->GetGlobalDepth();
  auto bucket_idx = KeyToDirectoryIndex(key, dir_page_data);
  auto bucket_page_id = KeyToPageId(key, dir_page_data);
  auto [bucket_page, bucket_page_data] = FetchBucketPage(bucket_page_id);

  // If the bucket is full, split until it is successfully inserted into the bucket.

  bucket_page->WLatch();

  // first check whether we need to grow the directory
  if (dir_page_data->GetLocalDepth(bucket_idx) == dir_page_data->GetGlobalDepth()) {
    dir_page_data->IncrGlobalDepth();
    is_growing = true;
  }

  // second find the bucket pair, and update them
  dir_page_data->IncrLocalDepth(bucket_idx);
  auto bucket_depth = dir_page_data->GetLocalDepth(bucket_idx);
  auto split_bucket_idx = dir_page_data->GetSplitImageIndex(bucket_idx);
  page_id_t split_page_id;
  auto split_page_data =
      reinterpret_cast<HASH_TABLE_BUCKET_TYPE *>(buffer_pool_manager_->NewPage(&split_page_id)->GetData());
  dir_page_data->SetBucketPageId(split_bucket_idx, split_page_id);
  dir_page_data->SetLocalDepth(split_bucket_idx, bucket_depth);

  // rehash all key-value pairs in the bucket pair
  uint32_t num_read = 0;
  uint32_t num_readable = bucket_page_data->NumReadable();
  while (num_read < num_readable) {
    if (bucket_page_data->IsReadable(num_read)) {
      auto key = bucket_page_data->KeyAt(num_read);
      uint32_t which_bucket = Hash(key) & ((1 << bucket_depth) - 1);
      if (which_bucket == split_bucket_idx) {
        // remove from the original bucket and insert the new bucket
        auto value = bucket_page_data->ValueAt(num_read);
        split_page_data->Insert(key, value, comparator_);
        bucket_page_data->RemoveAt(num_read);
      }
      ++num_read;
    }
  }
  buffer_pool_manager_->UnpinPage(split_page_id, true);

  // redirect buckets.
  for (uint32_t i = 1 << old_global_depth; i < dir_page_data->Size(); ++i) {
    if (i == split_bucket_idx) {
      continue;
    }
    uint32_t redirect_bucket_idx = i & ((1 << old_global_depth) - 1);
    dir_page_data->SetBucketPageId(i, dir_page_data->GetBucketPageId(redirect_bucket_idx));
    dir_page_data->SetLocalDepth(i, dir_page_data->GetLocalDepth(redirect_bucket_idx));
  }

  success = bucket_page_data->Insert(key, value, comparator_);

  buffer_pool_manager_->UnpinPage(bucket_page_id, true);
  bucket_page->WUnlatch();

  buffer_pool_manager_->UnpinPage(directory_page_id_, is_growing);

  table_latch_.WUnlock();
  return success;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
bool HASH_TABLE_TYPE::Remove(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.RLock();

  auto dir_page_data = FetchDirectoryPage();
  auto bucket_page_id = KeyToPageId(key, dir_page_data);
  auto [bucket_page, bucket_page_data] = FetchBucketPage(bucket_page_id);

  bucket_page->WLatch();
  auto success = bucket_page_data->Remove(key, value, comparator_);

  buffer_pool_manager_->UnpinPage(bucket_page_id, success);
  buffer_pool_manager_->UnpinPage(directory_page_id_, false);
  bucket_page->WUnlatch();
  table_latch_.RUnlock();

  if (success && bucket_page_data->IsEmpty()) {
    Merge(transaction, key, value);
  }

  return success;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
void HASH_TABLE_TYPE::Merge(Transaction *transaction, const KeyType &key, const ValueType &value) {
  table_latch_.WLock();

  auto dir_page_data = FetchDirectoryPage();

  // traverse the directory page and merge all empty buckets.
  for (uint32_t i = 0; i < dir_page_data->Size(); ++i) {
    auto old_local_depth = dir_page_data->GetLocalDepth(i);
    auto bucket_page_id = dir_page_data->GetBucketPageId(i);
    auto [bucket_page, bucket_page_data] = FetchBucketPage(bucket_page_id);

    bucket_page->RLatch();

    if (old_local_depth <= 1 || !bucket_page_data->IsEmpty()) {
      bucket_page->RUnlatch();
      buffer_pool_manager_->UnpinPage(bucket_page_id, false);
      continue;
    }

    auto split_bucket_idx = dir_page_data->GetSplitImageIndex(i);
    if (dir_page_data->GetLocalDepth(split_bucket_idx) == old_local_depth) {
      dir_page_data->DecrLocalDepth(i);
      dir_page_data->DecrLocalDepth(split_bucket_idx);
      dir_page_data->SetBucketPageId(i, dir_page_data->GetBucketPageId(split_bucket_idx));
      auto new_bucket_page_id = dir_page_data->GetBucketPageId(i);

      for (uint32_t j = 0; j < dir_page_data->Size(); ++j) {
        if (i == j || j == split_bucket_idx) {
          continue;
        }
        auto cur_bucket_page_id = dir_page_data->GetBucketPageId(j);
        if (cur_bucket_page_id == bucket_page_id || cur_bucket_page_id == new_bucket_page_id) {
          dir_page_data->SetLocalDepth(j, dir_page_data->GetLocalDepth(i));
          dir_page_data->SetBucketPageId(j, new_bucket_page_id);
        }
      }
    }

    if (dir_page_data->CanShrink()) {
      dir_page_data->DecrGlobalDepth();
    }

    bucket_page->RUnlatch();
    buffer_pool_manager_->UnpinPage(bucket_page_id, false);
  }
  buffer_pool_manager_->UnpinPage(directory_page_id_, true);

  table_latch_.WUnlock();
}

/*****************************************************************************
 * GETGLOBALDEPTH - DO NOT TOUCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
uint32_t HASH_TABLE_TYPE::GetGlobalDepth() {
  table_latch_.RLock();
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  uint32_t global_depth = dir_page->GetGlobalDepth();
  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false, nullptr));
  table_latch_.RUnlock();
  return global_depth;
}

/*****************************************************************************
 * VERIFY INTEGRITY - DO NOT TOUCH
 *****************************************************************************/
template <typename KeyType, typename ValueType, typename KeyComparator>
void HASH_TABLE_TYPE::VerifyIntegrity() {
  table_latch_.RLock();
  HashTableDirectoryPage *dir_page = FetchDirectoryPage();
  dir_page->VerifyIntegrity();
  assert(buffer_pool_manager_->UnpinPage(directory_page_id_, false, nullptr));
  table_latch_.RUnlock();
}

/*****************************************************************************
 * TEMPLATE DEFINITIONS - DO NOT TOUCH
 *****************************************************************************/
template class ExtendibleHashTable<int, int, IntComparator>;

template class ExtendibleHashTable<GenericKey<4>, RID, GenericComparator<4>>;
template class ExtendibleHashTable<GenericKey<8>, RID, GenericComparator<8>>;
template class ExtendibleHashTable<GenericKey<16>, RID, GenericComparator<16>>;
template class ExtendibleHashTable<GenericKey<32>, RID, GenericComparator<32>>;
template class ExtendibleHashTable<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
