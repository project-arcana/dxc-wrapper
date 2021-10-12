#pragma once

#include <rich-log/detail/log_impl.hh>

namespace dxcw::detail
{
static constexpr rlog::domain domain = rlog::domain("DXCW");

inline void info_log(rlog::MessageBuilder& builder) { builder.set_domain(domain); }
inline void warn_log(rlog::MessageBuilder& builder)
{
    builder.set_domain(domain);
    builder.set_severity(rlog::severity::warning);
}
inline void err_log(rlog::MessageBuilder& builder)
{
    builder.set_domain(domain);
    builder.set_severity(rlog::severity::error);
}
inline void assert_log(rlog::MessageBuilder& builder)
{
    builder.set_domain(domain);
    builder.set_severity(rlog::severity::critical);
}
}

#define DXCW_LOG RICH_LOG_IMPL(dxcw::detail::info_log)
#define DXCW_LOG_WARN RICH_LOG_IMPL(dxcw::detail::warn_log)
#define DXCW_LOG_ERROR RICH_LOG_IMPL(dxcw::detail::err_log)
#define DXCW_LOG_ASSERT RICH_LOG_IMPL(dxcw::detail::assert_log)
