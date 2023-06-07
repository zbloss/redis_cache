// Copyright 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <sstream>

#include "rapidjson/document.h"
#include "redis_cache.h"
#include "triton/common/logging.h"
#include "triton/core/tritoncache.h"

namespace triton::cache::redis {

std::unique_ptr<sw::redis::Redis>
init_client(
    const sw::redis::ConnectionOptions& connectionOptions,
    sw::redis::ConnectionPoolOptions poolOptions)
{
  std::unique_ptr<sw::redis::Redis> redis =
      std::make_unique<sw::redis::Redis>(connectionOptions, poolOptions);
  auto res = redis->ping("pong");
  LOG_VERBOSE("Successfully connected to Redis");
  return redis;
}


template<typename T>
void setOption(const char* key, T& option, rapidjson::Document& document){
  if(document.HasMember(key)){
    option = document[key].GetString();
  }
}

template<>
void setOption(const char* key, int& option, rapidjson::Document& document){
  if(document.HasMember(key)){
    option = std::atoi(document[key].GetString());
  }
}

template<>
void setOption(const char* key, size_t& option, rapidjson::Document& document){
  if(document.HasMember(key)){
    option = std::strtol(document[key].GetString(), nullptr, 10);
  }
}

template<>
void setOption(const char* key, std::chrono::milliseconds& option, rapidjson::Document& document){
  if(document.HasMember(key)){
    auto ms = std::atoi(document[key].GetString());
    option = std::chrono::milliseconds(ms);
  }
}


TRITONSERVER_Error*
RedisCache::Create(
    const std::string& cache_config, std::unique_ptr<RedisCache>* cache)
{
  rapidjson::Document document;

  document.Parse(cache_config.c_str());
  if (!document.HasMember("host") || !document.HasMember("port")) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        "Failed to initialize RedisCache, didn't specify address. Must at a "
        "minimum specify 'host' and 'port' in the configuration - e.g. "
        "tritonserver --cache-config redis,host=redis --cache-config "
        "redis,port=6379 --model-repository=/models ...");
  }

  sw::redis::ConnectionOptions options;
  sw::redis::ConnectionPoolOptions poolOptions;

  setOption("host", options.host, document);
  setOption("port", options.port, document);
  setOption("user", options.user, document);
  setOption("password", options.password, document);
  setOption("db", options.db, document);
  setOption("connect_timeout", options.connect_timeout, document);
  setOption("socket_timeout", options.socket_timeout, document);
  setOption("pool_size", poolOptions.size, document);
  setOption("wait_timeout", poolOptions.wait_timeout, document);
  if (!document.HasMember("wait_timeout")){
    poolOptions.wait_timeout = std::chrono::milliseconds(100);
  }

  try {
    cache->reset(new RedisCache(options, poolOptions));
  }
  catch (const std::exception& ex) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Failed to initialize Response Cache: " + std::string(ex.what()))
            .c_str());
  }
  return nullptr;  // success
}

RedisCache::RedisCache(
    const sw::redis::ConnectionOptions& connectionOptions,
    const sw::redis::ConnectionPoolOptions& poolOptions)
{
  try {
    this->_client = init_client(connectionOptions, poolOptions);
  }
  catch (const std::exception& ex) {
    throw std::runtime_error(
        ("Failed to initialize Response Cache: " + std::string(ex.what()))
            .c_str());
  }
}

RedisCache::~RedisCache()
{
  this->_client.reset();
}

std::pair<TRITONSERVER_Error*, CacheEntry>
RedisCache::Lookup(const std::string& key)
{
  auto handleError = [&key](const std::string& message, const char* cause = nullptr){
    std::ostringstream msg;
    msg << message << key << " from cache";
    if(cause){
      msg << ' ' << cause;
    }
    return TRITONSERVER_ErrorNew(TRITONSERVER_ERROR_INTERNAL, msg.str().c_str());
  };

  CacheEntry entry;

  try {
    this->_client->hgetall(
        key, std::inserter(entry.items, entry.items.begin()));

    // determine the number of buffers by dividing the size by the number of
    // fields per buffer
    entry.numBuffers = entry.items.size() / FIELDS_PER_BUFFER;
    return {nullptr, entry};
  }
  catch (sw::redis::TimeoutError& e) {
    return {handleError("Timeout retrieving key: ", e.what()), {}};
  }
  catch (sw::redis::IoError& e) {
    return {handleError("Failed to retrieve key:", e.what()), {}};
  }
  catch (...) {
    return {handleError("Failed to retrieve key:"), {}};
  }
}

TRITONSERVER_Error*
RedisCache::Insert(const std::string& key, CacheEntry& entry)
{
  auto handleError = [&key](const std::string& message, const char* cause = nullptr){
    std::ostringstream msg;
    msg << message << key << " into cache";
    if(cause){
      msg << ' ' << cause;
    }
    return TRITONSERVER_ErrorNew(TRITONSERVER_ERROR_INTERNAL, msg.str().c_str());
  };

  try {
    _client->hmset(key, entry.items.begin(), entry.items.end());
  }
  catch (sw::redis::TimeoutError& e) {
    return handleError("Timeout inserting key: ", e.what());
  }
  catch (sw::redis::IoError& e) {
    return handleError("Failed to insert key:", e.what());
  }
  catch (...) {
    return handleError("Failed to insert key:");
  }

  return nullptr;  // success
}
}  // namespace triton::cache::redis
