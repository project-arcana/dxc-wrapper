#pragma once

#include <rich-log/log.hh>

namespace dxcw::detail
{
static constexpr rlog::domain domain = rlog::domain("DXCW");
static constexpr rlog::severity assert_severity = rlog::severity("ASSERT", "\u001b[38;5;196m\u001b[1m");

constexpr void info_log(rlog::MessageBuilder& builder) { builder.set_domain(domain); }
constexpr void warn_log(rlog::MessageBuilder& builder)
{
    builder.set_domain(domain);
    builder.set_severity(rlog::severity::warning());
    builder.set_use_error_stream(true);
}
constexpr void err_log(rlog::MessageBuilder& builder)
{
    builder.set_domain(domain);
    builder.set_severity(rlog::severity::error());
    builder.set_use_error_stream(true);
}
constexpr void assert_log(rlog::MessageBuilder& builder)
{
    builder.set_domain(domain);
    builder.set_severity(assert_severity);
    builder.set_use_error_stream(true);
}
}

#define DXCW_LOG RICH_LOG_IMPL(dxcw::detail::info_log)
#define DXCW_LOG_WARN RICH_LOG_IMPL(dxcw::detail::warn_log)
#define DXCW_LOG_ERROR RICH_LOG_IMPL(dxcw::detail::err_log)
#define DXCW_LOG_ASSERT RICH_LOG_IMPL(dxcw::detail::assert_log)
