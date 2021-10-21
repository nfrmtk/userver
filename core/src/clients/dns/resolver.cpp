#include <userver/clients/dns/resolver.hpp>

#include <chrono>

#include <clients/dns/file_resolver.hpp>
#include <clients/dns/helpers.hpp>
#include <clients/dns/net_resolver.hpp>
#include <userver/cache/nway_lru_cache.hpp>
#include <userver/clients/dns/exception.hpp>
#include <userver/concurrent/mutex_set.hpp>
#include <userver/engine/async.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/assert.hpp>
#include <userver/utils/impl/wait_token_storage.hpp>
#include <userver/utils/mock_now.hpp>

namespace clients::dns {

enum class FailureMode { kIgnore, kCache };

class Resolver::Impl {
 public:
  struct NetCacheResult {
    enum class Status {
      kMiss,
      kHitReply,
      kHitReplyWithUpdate,
      kHitFailure,
    };

    Status status{Status::kMiss};
    AddrVector addrs;
  };

  Impl(engine::TaskProcessor& fs_task_processor, const ResolverConfig& config);
  ~Impl();

  const LookupSourceCounters& GetLookupSourceCounters() const;

  void ReloadHosts();
  void FlushNetworkCache();
  void FlushNetworkCache(const std::string& name);

  AddrVector QueryFileCache(const std::string& name);
  NetCacheResult QueryNetCache(const std::string& name);

  auto GetUpdateMutex(const std::string& name);
  void AccountNetUpdateFailure();

  template <typename Mutex>
  AddrVector DoForegroundQuery(std::unique_lock<Mutex>& lock, Mutex&& mutex,
                               const std::string& name,
                               engine::Deadline deadline);

  template <typename Mutex>
  void StartBackgroundQuery(std::unique_lock<Mutex>& lock, Mutex&& mutex,
                            const std::string& name);

 private:
  struct NetCacheEntry {
    AddrVector addrs;
    std::chrono::steady_clock::time_point expiration;
    bool is_failure{false};
  };

  template <typename Mutex>
  void MoveQueryToBackground(std::unique_lock<Mutex>& lock, Mutex&& mutex,
                             engine::Future<NetResolver::Response>&& future,
                             const std::string& name, FailureMode failure_mode);

  template <typename Mutex>
  void FinishNetUpdate(std::unique_lock<Mutex>& lock,
                       engine::Future<NetResolver::Response>&& future,
                       const std::string& name, AddrVector* addrs,
                       FailureMode failure_mode);

  LookupSourceCounters source_counters_;
  FileResolver file_resolver_;
  NetResolver net_resolver_;
  const std::chrono::milliseconds net_cache_update_margin_;
  const std::chrono::milliseconds net_cache_max_reply_ttl_;
  const std::chrono::milliseconds net_cache_failure_ttl_;
  cache::NWayLRU<std::string, NetCacheEntry> net_cache_;
  concurrent::MutexSet<std::string> net_cache_update_mutexes_;
  utils::impl::WaitTokenStorage wait_token_storage_;
};

Resolver::Impl::Impl(engine::TaskProcessor& fs_task_processor,
                     const ResolverConfig& config)
    : file_resolver_{fs_task_processor, config.file_path,
                     config.file_update_interval},
      net_resolver_{fs_task_processor, config.network_timeout,
                    config.network_attempts, config.network_custom_servers},
      net_cache_update_margin_{config.network_timeout},
      net_cache_max_reply_ttl_{config.cache_max_reply_ttl},
      net_cache_failure_ttl_{config.cache_failure_ttl},
      net_cache_{config.cache_ways, config.cache_size_per_way},
      net_cache_update_mutexes_(config.cache_ways) {}

Resolver::Impl::~Impl() { wait_token_storage_.WaitForAllTokens(); }

const Resolver::LookupSourceCounters& Resolver::Impl::GetLookupSourceCounters()
    const {
  return source_counters_;
}

void Resolver::Impl::ReloadHosts() { file_resolver_.ReloadHosts(); }

void Resolver::Impl::FlushNetworkCache() { net_cache_.Invalidate(); }

void Resolver::Impl::FlushNetworkCache(const std::string& name) {
  net_cache_.InvalidateByKey(name);
}

AddrVector Resolver::Impl::QueryFileCache(const std::string& name) {
  auto addrs = file_resolver_.Resolve(name);
  if (!addrs.empty()) {
    ++source_counters_.file;
  }
  return addrs;
}

Resolver::Impl::NetCacheResult Resolver::Impl::QueryNetCache(
    const std::string& name) {
  NetCacheResult result;

  const auto now = utils::datetime::MockSteadyNow();
  const auto cached = net_cache_.Get(name);
  if (!cached) return result;

  if (cached->is_failure) {
    if (cached->expiration >= now) {
      ++source_counters_.cached_failure;
      result.status = NetCacheResult::Status::kHitFailure;
    }
    return result;
  }

  result.addrs = cached->addrs;
  if (cached->expiration >= now) {
    ++source_counters_.cached;
  } else {
    ++source_counters_.cached_stale;
  }

  if (cached->expiration - now >= net_cache_update_margin_) {
    result.status = NetCacheResult::Status::kHitReply;
  } else {
    result.status = NetCacheResult::Status::kHitReplyWithUpdate;
  }

  return result;
}

auto Resolver::Impl::GetUpdateMutex(const std::string& name) {
  return net_cache_update_mutexes_.GetMutexForKey(name);
}

void Resolver::Impl::AccountNetUpdateFailure() {
  ++source_counters_.network_failure;
}

template <typename Mutex>
AddrVector Resolver::Impl::DoForegroundQuery(std::unique_lock<Mutex>& lock,
                                             Mutex&& mutex,
                                             const std::string& name,
                                             engine::Deadline deadline) {
  UINVARIANT(lock, "Foreground query doesn't have a lock");
  UASSERT(lock.mutex() == &mutex);

  LOG_TRACE() << "Resolving '" << name << "' in foreground";
  auto future = net_resolver_.Resolve(name);
  auto future_status = future.wait_until(deadline);
  if (future_status != engine::FutureStatus::kReady) {
    LOG_TRACE() << "Sending query for '" << name << "' to background";
    MoveQueryToBackground(lock, std::forward<Mutex>(mutex), std::move(future),
                          name, FailureMode::kCache);
    // not updating counters here as the request lives on in the background
    if (future_status == engine::FutureStatus::kTimeout) {
      throw NotResolvedException{"Resolving '" + name + "' timed out"};
    }
    throw NotResolvedException{"Resolving '" + name + "' interrupted"};
  }
  AddrVector addrs;
  FinishNetUpdate(lock, std::move(future), name, &addrs, FailureMode::kCache);
  return addrs;
}

template <typename Mutex>
void Resolver::Impl::StartBackgroundQuery(std::unique_lock<Mutex>& lock,
                                          Mutex&& mutex,
                                          const std::string& name) {
  UASSERT(lock.mutex() == &mutex);
  if (!lock && !lock.try_lock()) {
    LOG_TRACE() << "Record for '" << name << "' is already updating, skipping";
    return;
  }
  LOG_TRACE() << "Updating record for '" << name << "' in background";
  auto future = net_resolver_.Resolve(name);
  MoveQueryToBackground(lock, std::forward<Mutex>(mutex), std::move(future),
                        name, FailureMode::kIgnore);
}

template <typename Mutex>
void Resolver::Impl::MoveQueryToBackground(
    std::unique_lock<Mutex>& lock, Mutex&& mutex,
    engine::Future<NetResolver::Response>&& future, const std::string& name,
    FailureMode failure_mode) {
  UASSERT(lock);
  UASSERT(lock.mutex() == &mutex);
  engine::impl::CriticalAsync(
      [token = wait_token_storage_.GetToken(), this, name, failure_mode](
          auto&& mutex, auto&& future) {
        std::unique_lock lock{mutex, std::adopt_lock};
        this->FinishNetUpdate(lock, std::forward<decltype(future)>(future),
                              name, nullptr, failure_mode);
      },
      std::forward<Mutex>(mutex), std::move(future))
      .Detach();
  lock.release();
}

// See RFC8767:
//  - TTL is considered to be an 32-bit unsigned integer
//  - TTL of zero should not be cached
//  - TTL should be capped (we use minutes instead of days though)
//  - Stale records may be used and recommended to have a TTL of 30 seconds
template <typename Mutex>
void Resolver::Impl::FinishNetUpdate(
    std::unique_lock<Mutex>& lock,
    engine::Future<NetResolver::Response>&& future, const std::string& name,
    AddrVector* addrs, FailureMode failure_mode) {
  UASSERT(lock);
  NetResolver::Response response;
  try {
    response = future.get();
  } catch (const ResolverException& ex) {
    LOG_LIMITED_ERROR() << "Resolving of '" << name << "' failed: " << ex;
    if (failure_mode == FailureMode::kCache) {
      LOG_TRACE() << "Caching failure for '" << name << '\'';
      net_cache_.Put(name, NetCacheEntry{{},
                                         utils::datetime::MockSteadyNow() +
                                             net_cache_failure_ttl_,
                                         true});
    }
    ++source_counters_.network_failure;
    throw;
  }

  const auto effective_ttl =
      std::min<std::chrono::milliseconds>(response.ttl,
                                          net_cache_max_reply_ttl_) +
      (response.received_at - utils::datetime::MockNow());
  if (addrs) *addrs = response.addrs;
  if (effective_ttl.count() > 0) {
    LOG_TRACE() << "Updating cache for '" << name << '\'';
    net_cache_.Put(
        name, NetCacheEntry{std::move(response.addrs),
                            utils::datetime::MockSteadyNow() + effective_ttl});
  } else {
    LOG_TRACE() << "Skipping cache update for '" << name << '\'';
  }
  ++source_counters_.network;
}

Resolver::Resolver(engine::TaskProcessor& fs_task_processor,
                   const ResolverConfig& config)
    : impl_(fs_task_processor, config) {}

Resolver::~Resolver() = default;

AddrVector Resolver::Resolve(const std::string& name,
                             engine::Deadline deadline) {
  {
    auto file_addrs = impl_->QueryFileCache(name);
    if (!file_addrs.empty()) return file_addrs;
  }

  auto net_result = impl_->QueryNetCache(name);

  if (net_result.status == Impl::NetCacheResult::Status::kHitReply) {
    return std::move(net_result.addrs);
  }

  auto mutex = impl_->GetUpdateMutex(name);
  std::unique_lock lock{mutex, std::defer_lock};
  if (net_result.status == Impl::NetCacheResult::Status::kMiss) {
    // synchronize with possible parallel updates
    if (deadline.IsReachable()) {
      lock.try_lock_for(deadline.TimeLeft());
    } else {
      lock.lock();
    }
    if (!lock) {
      impl_->AccountNetUpdateFailure();
      throw NotResolvedException{"Resolving '" + name + "' timed out (lock)"};
    }

    net_result = impl_->QueryNetCache(name);
  }

  switch (net_result.status) {
    case Impl::NetCacheResult::Status::kMiss:
      return impl_->DoForegroundQuery(lock, std::move(mutex), name, deadline);

    case Impl::NetCacheResult::Status::kHitReplyWithUpdate:
      impl_->StartBackgroundQuery(lock, std::move(mutex), name);
      [[fallthrough]];
    case Impl::NetCacheResult::Status::kHitReply:
      return std::move(net_result.addrs);

    case Impl::NetCacheResult::Status::kHitFailure:
      throw NotResolvedException{"Not resolving '" + name +
                                 "' because of prior failure"};
  }
}

const Resolver::LookupSourceCounters& Resolver::GetLookupSourceCounters()
    const {
  return impl_->GetLookupSourceCounters();
}

void Resolver::ReloadHosts() { impl_->ReloadHosts(); }

void Resolver::FlushNetworkCache() { impl_->FlushNetworkCache(); }

void Resolver::FlushNetworkCache(const std::string& name) {
  impl_->FlushNetworkCache(name);
}

}  // namespace clients::dns
