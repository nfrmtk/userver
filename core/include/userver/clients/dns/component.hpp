#pragma once

/// @file userver/clients/dns/component.hpp
/// @brief @copybrief clients::dns::Component

#include <userver/clients/dns/resolver.hpp>
#include <userver/components/loggable_component_base.hpp>
#include <userver/components/statistics_storage.hpp>

namespace clients::dns {

// clang-format off

/// @ingroup userver_components
///
/// @brief Caching DNS resolver component.
///
/// Returned references to clients::dns::Resolver live for a lifetime
/// of the component and are safe for concurrent use.
///
/// ## Static options:
/// Name | Description | Default value
/// ---- | ----------- | -------------
/// fs-task-processor | task processor for disk I/O operations | -
/// hosts-file-path | path to the `hosts` file | /etc/hosts
/// hosts-file-update-interval | `hosts` file cache reload interval | 5m
/// network-timeout | timeout for network requests | 1s
/// network-attempts | number of attempts for network requests | 1
/// network-custom-servers | list of name servers to use | from `/etc/resolv.conf`
/// cache-ways | number of ways for network cache | 16
/// cache-size-per-way | size of each way of network cache | 256
/// cache-max-reply-ttl | TTL limit for network replies caching | 5m
/// cache-failure-ttl | TTL for network failures caching | 5s
///
/// ## Static configuration example:
///
/// @snippet components/common_components_list_test.cpp  Sample dns client component config

// clang-format on
class Component final : public components::LoggableComponentBase {
 public:
  static constexpr auto kName = "dns-client";

  Component(const components::ComponentConfig&,
            const components::ComponentContext&);

  Resolver& GetResolver();

 private:
  formats::json::Value ExtendStatistics();

 private:
  Resolver resolver_;
  utils::statistics::Entry statistics_holder_;
};

}  // namespace clients::dns
