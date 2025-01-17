#pragma once

#ifdef USERVER_MYSQL_OLD_INCLUDE

#include <mysql/errmsg.h>
#include <mysql/mysql.h>
#include <mysql/mysqld_error.h>

#else

#include <mariadb/errmsg.h>
#include <mariadb/mysql.h>
#include <mariadb/mysqld_error.h>

#endif

USERVER_NAMESPACE_BEGIN

namespace storages::mysql::impl {

// We use std::size_t interchangeably with unsigned long/unsigned long long
// which libmariadb uses
// TODO : TAXICOMMON-6397, build for 32 bit.
static_assert(sizeof(std::size_t) == sizeof(unsigned long));
static_assert(sizeof(std::size_t) == sizeof(unsigned long long));

}  // namespace storages::mysql::impl

USERVER_NAMESPACE_END
