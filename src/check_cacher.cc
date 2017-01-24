/* Copyright 2017 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/check_cacher.h"
#include "src/signature.h"

#include "google/protobuf/stubs/logging.h"

using std::string;
using ::istio::mixer::v1::CheckRequest;
using ::istio::mixer::v1::CheckResponse;
using ::google::protobuf::util::Status;
using ::google::protobuf::util::error::Code;

namespace istio {
namespace mixer_client {

CheckCacher::CheckCacher(const CheckOptions& options) : options_(options) {
  // Converts flush_interval_ms to Cycle used by SimpleCycleTimer.
  flush_interval_in_cycle_ =
      options_.flush_interval_ms * SimpleCycleTimer::Frequency() / 1000;

  if (options.num_entries >= 0) {
    cache_.reset(new CheckCache(
        options.num_entries, std::bind(&CheckCacher::OnCacheEntryDelete, this,
                                       std::placeholders::_1)));
    cache_->SetMaxIdleSeconds(options.expiration_ms / 1000.0);
  }
}

CheckCacher::~CheckCacher() {
  // FlushAll() will remove all cache items.
  FlushAll();
}

bool CheckCacher::ShouldFlush(const CacheElem& elem) {
  int64_t age = SimpleCycleTimer::Now() - elem.last_check_time();
  return age >= flush_interval_in_cycle_;
}

Status CheckCacher::Check(const Attributes& attributes,
                          CheckResponse* response) {
  string request_signature = GenerateSignature(attributes);
  std::unique_lock<std::mutex> lock(cache_mutex_);
  CheckCache::ScopedLookup lookup(cache_.get(), request_signature);

  if (!lookup.Found()) {
    // By returning NO_FOUND, caller will send request to server.
    return Status(Code::NOT_FOUND, "");
  }

  CacheElem* elem = lookup.value();

  if (ShouldFlush(*elem)) {
    // By returning NO_FOUND, caller will send request to server.
    return Status(Code::NOT_FOUND, "");
  }

  // Setting last check to now to block more check requests to Chemist.
  elem->set_last_check_time(SimpleCycleTimer::Now());
  *response = elem->check_response();

  return Status::OK;
}

Status CheckCacher::CacheResponse(const Attributes& attributes,
                                  const CheckResponse& response) {
  std::unique_lock<std::mutex> lock(cache_mutex_);

  if (cache_) {
    string request_signature = GenerateSignature(attributes);
    CheckCache::ScopedLookup lookup(cache_.get(), request_signature);

    int64_t now = SimpleCycleTimer::Now();

    if (lookup.Found()) {
      lookup.value()->set_last_check_time(now);
      lookup.value()->set_check_response(response);
    } else {
      CacheElem* cache_elem = new CacheElem(response, now);
      cache_->Insert(request_signature, cache_elem, 1);
    }
  }

  return Status::OK;
}

// When the next Flush() should be called.
// Flush() call remove expired response.
int CheckCacher::GetNextFlushInterval() {
  if (!cache_) return -1;
  return options_.expiration_ms;
}

// Flush aggregated requests whom are longer than flush_interval.
// Called at time specified by GetNextFlushInterval().
Status CheckCacher::Flush() {
  std::unique_lock<std::mutex> lock(cache_mutex_);

  if (cache_) {
    cache_->RemoveExpiredEntries();
  }

  return Status::OK;
}

void CheckCacher::OnCacheEntryDelete(CacheElem* elem) { delete elem; }

// Flush out aggregated check requests, clear all cache items.
// Usually called at destructor.
Status CheckCacher::FlushAll() {
  GOOGLE_LOG(INFO) << "Remove all entries of check cache.";
  std::unique_lock<std::mutex> lock(cache_mutex_);

  if (cache_) {
    cache_->RemoveAll();
  }

  return Status::OK;
}

std::unique_ptr<CheckCacher> CreateCheckCacher(const CheckOptions& options) {
  return std::unique_ptr<CheckCacher>(new CheckCacher(options));
}

}  // namespace mixer_client
}  // namespace istio
