// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/reflect/enum_instrument.h>
#include <nx/utils/log/log_main.h>

#include <QDateTime>
#include <QDebug>
#define DBG(...) qDebug()<<QDateTime::currentDateTime().toString("yyyy/MM/dd hh:mm:ss.zzz")<<__FILE__<<__FUNCTION__<<__LINE__<<__VA_ARGS__

namespace nx::log {

NX_REFLECTION_ENUM_CLASS(LogName,
    main,
    http,
    system
);

extern const NX_UTILS_API nx::utils::log::Tag kMainTag;
extern const NX_UTILS_API nx::utils::log::Tag kHttpTag;
extern const NX_UTILS_API nx::utils::log::Tag kSystemTag;

constexpr size_t kLogNamesCount =
    nx::reflect::enumeration::visitAllItems<LogName>(
        [](auto&&... items)
        {
            return sizeof...(items);
        });

NX_UTILS_API std::array<QString, kLogNamesCount> getCompatLoggerNames();
NX_UTILS_API std::shared_ptr<nx::utils::log::AbstractLogger> getLogger(const LogName id);

} // namespace nx::log
