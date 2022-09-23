// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/vms/client/core/event_search/event_search_globals.h>

namespace nx::vms::client::core {

template<typename Container>
struct FetchedData
{
    using Iterator = typename Container::iterator;
    struct ItemsRange
    {
        int offset = 0;
        int length = 0;
    };

    Container data;
    ItemsRange tail;
    ItemsRange body;

    template<typename Iterator>
    static FetchedData<Container> make(
        const Iterator& begin,
        const Iterator& end,
        const Iterator& tailEnd,
        EventSearch::FetchDirection direction)
    {
        const auto length = std::distance(begin, end);

        FetchedData<Container> result;
        result.tail.length = std::distance(begin, tailEnd);
        result.body.length = length - result.tail.length;

        if (direction == EventSearch::FetchDirection::later)
        {
            result.body.offset = result.tail.length;
            result.data = Container(begin, end);
        }
        else
        {
            result.tail.offset = length - result.tail.length;
            result.data.resize(length);
            int offset = length;
            for (auto it = begin; it != end; ++it)
                result.data[--offset] = *it;
        }
        return result;
    }

    template<typename Iterator>
    static FetchedData<Container> makeTailData(
        const Iterator& begin,
        const Iterator& end,
        EventSearch::FetchDirection direction)
    {
        return make(begin, end, end, direction);
    }
};

} // namespace nx::vms::client::core
