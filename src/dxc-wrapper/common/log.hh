#pragma once

#include <rich-log/domain.hh>
#include <rich-log/log.hh>

namespace dxcw
{
	RICH_LOG_DECLARE_DEFAULT_DOMAIN();
}

#define DXCW_LOG(...) RICH_LOGD(Default, Info, __VA_ARGS__)
#define DXCW_LOG_WARN(...) RICH_LOGD(Default, Warning, __VA_ARGS__)
#define DXCW_LOG_TRACE(...) RICH_LOGD(Default, Trace, __VA_ARGS__)
#define DXCW_LOG_ERROR(...) RICH_LOGD(Default, Error, __VA_ARGS__)
#define DXCW_LOG_ASSERT(...) RICH_LOGD(Default, Fatal, __VA_ARGS__)
