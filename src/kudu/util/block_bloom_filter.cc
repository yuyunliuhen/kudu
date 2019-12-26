// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/util/block_bloom_filter.h"

#include <emmintrin.h>
#include <mm_malloc.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ostream>

#include <gflags/gflags.h>

#include "kudu/gutil/singleton.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/flag_tags.h"

DEFINE_bool(disable_blockbloomfilter_avx2, false,
            "Disable AVX2 operations in BlockBloomFilter. This flag has no effect if the target "
            "CPU doesn't support AVX2 at run-time or BlockBloomFilter was built with a compiler "
            "that doesn't support AVX2.");
TAG_FLAG(disable_blockbloomfilter_avx2, hidden);

namespace kudu {

constexpr uint32_t BlockBloomFilter::kRehash[8] __attribute__((aligned(32)));
const base::CPU BlockBloomFilter::kCpu = base::CPU();

BlockBloomFilter::BlockBloomFilter(BlockBloomFilterBufferAllocatorIf* buffer_allocator) :
  always_false_(true),
  buffer_allocator_(buffer_allocator),
  log_num_buckets_(0),
  directory_mask_(0),
  directory_(nullptr) {
#ifdef USE_AVX2
  if (has_avx2()) {
    bucket_insert_func_ptr_ = &BlockBloomFilter::BucketInsertAVX2;
    bucket_find_func_ptr_ = &BlockBloomFilter::BucketFindAVX2;
  } else {
    bucket_insert_func_ptr_ = &BlockBloomFilter::BucketInsert;
    bucket_find_func_ptr_ = &BlockBloomFilter::BucketFind;
  }
#else
  bucket_insert_func_ptr_ = &BlockBloomFilter::BucketInsert;
  bucket_find_func_ptr_ = &BlockBloomFilter::BucketFind;
#endif
}

BlockBloomFilter::~BlockBloomFilter() {
  DCHECK(directory_ == nullptr) <<
    "Close() should have been called before the object is destroyed.";
}

Status BlockBloomFilter::Init(const int log_space_bytes) {
  // Since log_space_bytes is in bytes, we need to convert it to the number of tiny
  // Bloom filters we will use.
  log_num_buckets_ = std::max(1, log_space_bytes - kLogBucketByteSize);
  // Since we use 32 bits in the arguments of Insert() and Find(), log_num_buckets_
  // must be limited.
  if (log_num_buckets_ > 32) {
    return Status::InvalidArgument(
        strings::Substitute("Bloom filter too large. log_space_bytes: $0", log_space_bytes));
  }
  // Don't use log_num_buckets_ if it will lead to undefined behavior by a shift
  // that is too large.
  directory_mask_ = (1ULL << log_num_buckets_) - 1;

  const size_t alloc_size = directory_size();
  Close(); // Ensure that any previously allocated memory for directory_ is released.
  RETURN_NOT_OK(buffer_allocator_->AllocateBuffer(alloc_size,
                                                  reinterpret_cast<void**>(&directory_)));
  memset(directory_, 0, alloc_size);
  return Status::OK();
}

void BlockBloomFilter::Close() {
  if (directory_ != nullptr) {
    buffer_allocator_->FreeBuffer(directory_);
    directory_ = nullptr;
  }
}

ATTRIBUTE_NO_SANITIZE_INTEGER
void BlockBloomFilter::BucketInsert(const uint32_t bucket_idx, const uint32_t hash) noexcept {
  // new_bucket will be all zeros except for eight 1-bits, one in each 32-bit word. It is
  // 16-byte aligned so it can be read as a __m128i using aligned SIMD loads in the second
  // part of this method.
  uint32_t new_bucket[kBucketWords] __attribute__((aligned(16)));
  for (int i = 0; i < kBucketWords; ++i) {
    // Rehash 'hash' and use the top kLogBucketWordBits bits, following Dietzfelbinger.
    new_bucket[i] = (kRehash[i] * hash) >> ((1 << kLogBucketWordBits) - kLogBucketWordBits);
    new_bucket[i] = 1U << new_bucket[i];
  }
  for (int i = 0; i < 2; ++i) {
    __m128i new_bucket_sse = _mm_load_si128(reinterpret_cast<__m128i*>(new_bucket + 4 * i));
    __m128i* existing_bucket = reinterpret_cast<__m128i*>(&directory_[bucket_idx][4 * i]);
    *existing_bucket = _mm_or_si128(*existing_bucket, new_bucket_sse);
  }
}

ATTRIBUTE_NO_SANITIZE_INTEGER
bool BlockBloomFilter::BucketFind(
    const uint32_t bucket_idx, const uint32_t hash) const noexcept {
  for (int i = 0; i < kBucketWords; ++i) {
    BucketWord hval = (kRehash[i] * hash) >> ((1 << kLogBucketWordBits) - kLogBucketWordBits);
    hval = 1U << hval;
    if (!(directory_[bucket_idx][i] & hval)) {
      return false;
    }
  }
  return true;
}

// The following three methods are derived from
//
// fpp = (1 - exp(-kBucketWords * ndv/space))^kBucketWords
//
// where space is in bits.
size_t BlockBloomFilter::MaxNdv(const int log_space_bytes, const double fpp) {
  DCHECK(log_space_bytes > 0 && log_space_bytes < 61);
  DCHECK(0 < fpp && fpp < 1);
  static const double ik = 1.0 / kBucketWords;
  return -1 * ik * static_cast<double>(1ULL << (log_space_bytes + 3)) * log(1 - pow(fpp, ik));
}

int BlockBloomFilter::MinLogSpace(const size_t ndv, const double fpp) {
  static const double k = kBucketWords;
  if (0 == ndv) return 0;
  // m is the number of bits we would need to get the fpp specified
  const double m = -k * ndv / log(1 - pow(fpp, 1.0 / k));

  // Handle case where ndv == 1 => ceil(log2(m/8)) < 0.
  return std::max(0, static_cast<int>(ceil(log2(m / 8))));
}

double BlockBloomFilter::FalsePositiveProb(const size_t ndv, const int log_space_bytes) {
  return pow(1 - exp((-1.0 * static_cast<double>(kBucketWords) * static_cast<double>(ndv))
                     / static_cast<double>(1ULL << (log_space_bytes + 3))),
             kBucketWords);
}

void BlockBloomFilter::InsertNoAvx2(const uint32_t hash) noexcept {
  always_false_ = false;
  const uint32_t bucket_idx = Rehash32to32(hash) & directory_mask_;
  BucketInsert(bucket_idx, hash);
}

// To set 8 bits in an 32-byte Bloom filter, we set one bit in each 32-bit uint32_t. This
// is a "split Bloom filter", and it has approximately the same false positive probability
// as standard a Bloom filter; See Mitzenmacher's "Bloom Filters and Such". It also has
// the advantage of requiring fewer random bits: log2(32) * 8 = 5 * 8 = 40 random bits for
// a split Bloom filter, but log2(256) * 8 = 64 random bits for a standard Bloom filter.
void BlockBloomFilter::Insert(const uint32_t hash) noexcept {
  DCHECK_NOTNULL(directory_);
  always_false_ = false;
  const uint32_t bucket_idx = Rehash32to32(hash) & directory_mask_;
  (this->*bucket_insert_func_ptr_)(bucket_idx, hash);
}

bool BlockBloomFilter::Find(const uint32_t hash) const noexcept {
  if (always_false_) {
    return false;
  }
  DCHECK_NOTNULL(directory_);
  const uint32_t bucket_idx = Rehash32to32(hash) & directory_mask_;
  return (this->*bucket_find_func_ptr_)(bucket_idx, hash);
}

Status DefaultBlockBloomFilterBufferAllocator::AllocateBuffer(size_t bytes, void** ptr) {
  int ret_code = posix_memalign(ptr, CACHELINE_SIZE, bytes);
  return ret_code == 0 ? Status::OK() :
                         Status::RuntimeError(strings::Substitute("bad_alloc. bytes: $0", bytes));
}

void DefaultBlockBloomFilterBufferAllocator::FreeBuffer(void* ptr) {
  DCHECK_NOTNULL(ptr);
  free(ptr);
}

DefaultBlockBloomFilterBufferAllocator* DefaultBlockBloomFilterBufferAllocator::GetSingleton() {
  return Singleton<DefaultBlockBloomFilterBufferAllocator>::get();
}

} // namespace kudu
