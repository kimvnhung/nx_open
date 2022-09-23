// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>
#include <utility>

#include <nx/utils/scoped_model_operations.h>
#include <nx/vms/client/core/event_search/event_search_globals.h>
#include <nx/vms/client/core/event_search/utils/fetched_data.h>
#include <recording/time_period.h>

namespace nx::vms::client::core {

using namespace std::chrono;
using FetchDirection = EventSearch::FetchDirection;

template<typename Accessor, typename Iterator>
QnTimePeriod actualPeriod(const Iterator& begin, const Iterator& end)
{
    if (begin == end)
        return {};

    const auto frontTime = Accessor::timestamp(*begin);
    const auto backTime = Accessor::timestamp(*(end - 1));
    return QnTimePeriod::fromInterval(
        std::min(frontTime, backTime),
        std::max(frontTime, backTime));
}

template<typename Accessor, typename Container>
QnTimePeriod actualPeriod(const Container& container)//<< /?? rename? does not include duration
{
    if (container.empty())
        return {};

    const auto frontTime = Accessor::timestamp(container.front());
    const auto backTime = Accessor::timestamp(container.back());
    return QnTimePeriod::fromInterval(
        std::min(frontTime, backTime),
        std::max(frontTime, backTime));
}

template<typename Accessor>
struct Predicate
{
    using Type = typename Accessor::ItemType;
    using LowerBoundPredicate = std::function<bool (const Type& left, milliseconds right)>;
    using UpperBoundPredicate = std::function<bool (milliseconds left, const Type& right)>;

    static LowerBoundPredicate lowerBound(FetchDirection direction = FetchDirection::later)
    {
        return direction == FetchDirection::later
            ? [](const Type& lhs, milliseconds rhs) { return Accessor::timestamp(lhs) > rhs; }
            : [](const Type& lhs, milliseconds rhs) { return Accessor::timestamp(lhs) < rhs; };
    }

    static UpperBoundPredicate upperBound(FetchDirection direction = FetchDirection::later)
    {
        return direction == FetchDirection::later
            ? [](milliseconds lhs, const Type& rhs) { return lhs > Accessor::timestamp(rhs); }
            : [](milliseconds lhs, const Type& rhs) { return lhs < Accessor::timestamp(rhs); };
    }
};

std::chrono::milliseconds getTailPointTime(
    const QnTimePeriod& period,
    EventSearch::FetchDirection direction);

template<typename DataContainer, typename FetchedIterator, typename Model>
void insertTail(
    Model* model,
    DataContainer& old,
    FetchedIterator fetchedBegin,
    FetchedIterator fetchedEnd,
    EventSearch::FetchDirection direction)
{
    const int count = std::distance(fetchedBegin, fetchedEnd);
    const int start = direction == EventSearch::FetchDirection::later ? 0 : old.size();
    const typename Model::ScopedInsertRows insertRows(model, start, start + count - 1);
    old.insert(old.begin() + start, fetchedBegin, fetchedEnd);
}

// TODO: add getters for timestamp/uuid
// describe that we suppose that there are no a lot of update/insert/delete items inside request
template<typename OldDataContainer, typename FetchedIterator, typename Model>
void updateSearchEvents(
    Model* model,
    OldDataContainer& old,
    FetchedIterator fetchedBegin,
    FetchedIterator fetchedEnd)
{
    const auto removeItems =
        [model, &old](auto begin, auto end)
        {
            const int start = std::distance(old.begin(), begin);
            const int count = std::distance(begin, end);
            const typename Model::ScopedRemoveRows removeRows(model, start, start + count - 1);
            return old.erase(begin, end);
        };

    const auto insertItems =
        [model, &old](auto position, auto begin, auto end)
        {
            const int start = std::distance(old.begin(), position);
            const int count = std::distance(begin, end);
            const typename Model::ScopedInsertRows insertRows(model, start, start + count - 1);
            return old.insert(position, begin, end) + count;
        };

    const auto setData =
        [model, &old](auto position, auto value)
        {
            *position = value;
            const int row = std::distance(old.begin(), position);
            const auto index = model->index(row, 0);
            emit model->dataChanged(index, index);
        };

    auto itOld = old.begin();
    while (itOld != old.end() || fetchedBegin != fetchedEnd)
    {
        if (itOld == old.end())
        {
            // Insert all other fetched item to the end.
            insertItems(old.end(), fetchedBegin, fetchedEnd);
            return;
        }

        if (fetchedBegin == fetchedEnd)
        {
            // Remove all remaining current items.
            removeItems(itOld, old.end());
            return;
        }

        // We suppose there are not a lot items with the same timestamp. So to avoid redundant
        // operations it was decided to develop it with step-by-step (with while loop)
        // implementation rather than using lower/uppper_bound/find versions.

        // Remove all items "above" current fetched items.
        auto itRemoveEnd = itOld;
        while (itRemoveEnd->startTimeMs > fetchedBegin->startTimeMs && itRemoveEnd != old.end())
            ++itRemoveEnd;
        if (itOld != itRemoveEnd)
        {
            itOld = removeItems(itOld, itRemoveEnd);
            if (itOld == old.end())
                continue;
        }

        // Insert all items "above" current old items.
        auto itInsertEnd = fetchedBegin;
        while (itOld->startTimeMs < itInsertEnd->startTimeMs)
            ++itInsertEnd;
        if (fetchedBegin != itInsertEnd)
        {
            itOld = insertItems(itOld, fetchedBegin, itInsertEnd);
            fetchedBegin = itInsertEnd;
            if (itOld == old.end() || fetchedBegin == fetchedEnd)
                continue;
        }

        // Process "same timestamp" bookmarks
        if (!NX_ASSERT(itOld->startTimeMs == fetchedBegin->startTimeMs))
            continue;

        const auto currentTimestamp = itOld->startTimeMs;
        while (itOld != old.end() && fetchedBegin != fetchedEnd
            && itOld->startTimeMs == currentTimestamp)
        {
            auto fetchedIt = fetchedBegin;
            while (fetchedIt != fetchedEnd && fetchedIt->startTimeMs == currentTimestamp
                && itOld->guid != fetchedIt->guid)
            {
                ++fetchedIt;
            }

            if (fetchedIt == fetchedEnd || fetchedIt->startTimeMs != currentTimestamp)
            {
                // Didn't find old item in the fetched items with the same timestamp.
                itOld = removeItems(itOld, itOld + 1);
            }
            else
            {
                NX_ASSERT(fetchedIt->guid == itOld->guid);
                if (*fetchedIt != *itOld)
                    setData(itOld, *fetchedIt);

                std::swap(*fetchedIt, *fetchedBegin); //< Move up processed fetched item.
                ++fetchedBegin;
                ++itOld;
            }
        }
    }
}

/**
 * Return pair of iterators which represents request tail [begin, end) interval. TODO: rename to
 * getTailAndRearrance ?
 */
template<typename FetchedContainer, typename Accessor, typename CurrentIterator,
    typename FetchedIterator>
FetchedData<FetchedContainer> reorderAroundReferencePointInternal(
    const CurrentIterator& currentBeginIt,
    const CurrentIterator& currentEndIt,
    const FetchedIterator& fetchedBeginIt,
    const FetchedIterator& fetchedEndIt,
    EventSearch::FetchDirection direction)
{
    using ResultType = FetchedData<FetchedContainer>;

    if (currentBeginIt == currentEndIt || fetchedBeginIt == fetchedEndIt)
        return ResultType::makeTailData(fetchedBeginIt, fetchedEndIt, direction);

    const auto lowerBound = Predicate<Accessor>::lowerBound(direction);
    const auto upperBound = Predicate<Accessor>::upperBound(direction);
    auto fetchedReferenceTimeBeginIt = fetchedBeginIt;
    auto currentReferenceTimeIt = currentBeginIt;
    auto tailPointTime = getTailPointTime(
        actualPeriod<Accessor>(currentBeginIt, currentEndIt), direction);

    // Looking for the real reference point - for exmaple in case if initial reference point
    // item(s) were deleted.

    bool referenceTimePointExists = false;
    do
    {
        fetchedReferenceTimeBeginIt = std::lower_bound(fetchedReferenceTimeBeginIt,
            fetchedEndIt, tailPointTime, lowerBound);
        if (fetchedReferenceTimeBeginIt == fetchedEndIt)
            break;

        tailPointTime = Accessor::timestamp(*fetchedReferenceTimeBeginIt);

        currentReferenceTimeIt = std::lower_bound(currentReferenceTimeIt, currentEndIt,
            tailPointTime, lowerBound);
        if (currentReferenceTimeIt == currentEndIt)
            break; //< No any "old" item in the fetched data.

        if (Accessor::timestamp(*currentReferenceTimeIt) == tailPointTime)
        {
            // Check if we have at least on "old" item in the fetched data.
            const auto currentRefEndIt = std::upper_bound(currentReferenceTimeIt, currentEndIt,
                tailPointTime, upperBound);
            const auto fetchedReferenceTimeEndIt = std::upper_bound(fetchedReferenceTimeBeginIt,
                fetchedEndIt, tailPointTime, upperBound);
            const bool hasSameItem = std::any_of(currentReferenceTimeIt, currentRefEndIt,
                [fetchedReferenceTimeBeginIt, fetchedReferenceTimeEndIt](const auto& item)
                {
                    const auto it = std::find_if(fetchedReferenceTimeBeginIt,
                        fetchedReferenceTimeEndIt,
                        [item](const auto& other)
                        {
                            return Accessor::guid(item) == Accessor::guid(other);
                        });
                    return it != fetchedReferenceTimeEndIt;
                });
            if (hasSameItem)
            {
                referenceTimePointExists = true;
                break; // Reference point timestamp was found.
            }

            // Looking for the next potential reference time point.
            currentReferenceTimeIt = std::upper_bound(currentReferenceTimeIt, currentEndIt,
                tailPointTime, upperBound);

            if (currentReferenceTimeIt == currentEndIt)
                break; //< Still no any "old" item in the fetched data.

            tailPointTime = Accessor::timestamp(*currentReferenceTimeIt);
        }
        else
        {
            tailPointTime = Accessor::timestamp(*currentReferenceTimeIt);
        }
    } while (true);

    if (!referenceTimePointExists)
        return ResultType::makeTailData(fetchedBeginIt, fetchedEndIt, direction);

    // Move all "new" items with the reference timestamp to the correct side of the tail.
    const auto fetchedReferenceTimeEndIt = std::upper_bound(fetchedReferenceTimeBeginIt,
        fetchedEndIt, tailPointTime, upperBound);

    currentReferenceTimeIt = std::lower_bound(currentReferenceTimeIt, currentEndIt,
        tailPointTime, lowerBound);
    const auto currentReferenceTimeEndIt = std::upper_bound(currentReferenceTimeIt, currentEndIt,
        tailPointTime, upperBound);

    const auto moveToBackIf =
        [](auto begin, auto end, auto predicate)
        {
            while (begin != end)
            {
                if (predicate(*begin))
                    std::swap(*begin, *--end);
                else
                    ++begin;
            }
            return end;
        };

    // Move existing items "lower" for the "later" fetch dirction and "upper" for the "earlier" one.
    const auto it = moveToBackIf(fetchedReferenceTimeBeginIt, fetchedReferenceTimeEndIt,
        [currentReferenceTimeIt, currentReferenceTimeEndIt](const auto& item)
        {
            // We suppose that there are not a lot of items at the same millisecond.
            const auto itSame = std::find_if(currentReferenceTimeIt, currentReferenceTimeEndIt,
                [item](const auto& other)
                {
                    return Accessor::guid(item) == Accessor::guid(other);
                });

            return itSame != currentReferenceTimeEndIt;
        });

    return ResultType::make(fetchedBeginIt, fetchedEndIt, it, direction);
}

template<typename Accessor, typename CurrentContainer, typename FetchedContainer>
FetchedData<FetchedContainer> reorderAroundReferencePoint(
    const CurrentContainer& current,
    FetchedContainer& fetched,
    EventSearch::FetchDirection direction)
{
    if (direction == EventSearch::FetchDirection::later)
    {
        return reorderAroundReferencePointInternal<FetchedContainer, Accessor>(
            current.begin(), current.end(), fetched.rbegin(), fetched.rend(), direction);
    }

    // Since we have different order for the "earlier" request we use reverse iterators here.
    return reorderAroundReferencePointInternal<FetchedContainer, Accessor>(
        current.rbegin(), current.rend(), fetched.rbegin(), fetched.rend(), direction);
}

template<typename Data>
void truncateFetchedData(
    Data& fetched,
    FetchDirection direction,
    int targetSize,
    int maxTailSize)
{
    if (int(fetched.data.size()) <= targetSize)
        return;

    NX_ASSERT(maxTailSize <= targetSize);
    maxTailSize = std::min(targetSize, maxTailSize);

    using Range = typename Data::ItemsRange;
    const auto cutBack =
        [&fetched, direction](int toRemove)
        {
            const int newCount = int(fetched.data.size()) - toRemove;
            fetched.data.erase(fetched.data.begin() + newCount, fetched.data.end());
            const auto cutRanges =
                [newCount, toRemove](Range& topRange, Range& bottomRange)
                {
                    topRange.offset = 0; // Top ranfe offset is always 0.
                    topRange.length = std::min(topRange.length, newCount);

                    bottomRange.offset = std::min(bottomRange.offset, newCount);
                    bottomRange.length = std::max(0, bottomRange.length - toRemove);
                };

            if (direction == FetchDirection::later)
                cutRanges(fetched.tail, fetched.body);
            else
                cutRanges(fetched.body, fetched.tail);
        };

    const auto cutFront =
        [&fetched, direction](int toRemove)
        {
            fetched.data.erase(fetched.data.begin(), fetched.data.begin() + toRemove);

            const auto shiftLeft =
                [toRemove](Range& topRange, Range& bottomRange)
                {
                    topRange.offset = 0; // Top range is always 0
                    topRange.length = std::max(0, topRange.length - toRemove);
                    bottomRange.length -= std::max(toRemove - bottomRange.offset, 0);
                    bottomRange.offset = std::max(0, bottomRange.offset - toRemove);
                };

            if (direction == FetchDirection::later)
                shiftLeft(fetched.tail, fetched.body);
            else
                shiftLeft(fetched.body, fetched.tail);
        };

    // Check tail length.
    const int extraTailLength = fetched.tail.length - maxTailSize;
    if (extraTailLength > 0)
    {
        if (direction == FetchDirection::earlier)
            cutBack(extraTailLength);
        else
            cutFront(extraTailLength);
    }

    const auto removeCount = int(fetched.data.size()) - targetSize;
    if (removeCount <= 0)
        return;

    if (direction == FetchDirection::earlier)
        cutFront(removeCount);
    else
        cutBack(removeCount);
}

struct FetchedDataCheckResult
{
    bool success = false;
    std::chrono::milliseconds nextStartPointMs;
};

template<typename FetchedData>
FetchedDataCheckResult checkFetchedData(
    const FetchedData& fetched,
    int maxBodyLength,
    int maxTailLength,
    FetchDirection direction)
{
    if (fetched.data.empty())
        return FetchedDataCheckResult{true, {}};

    const bool fetchedAllNeeded =
        fetched.tail.length + fetched.body.length < maxBodyLength + maxTailLength ;

    if (fetchedAllNeeded || fetched.tail.length > maxTailLength / 2)
        return FetchedDataCheckResult{true, {}};

    if (fetched.data.begin()->startTimeMs >= fetched.data.rbegin()->startTimeMs)
        return FetchedDataCheckResult{false, {}};

    // Looking for the new period to be fetched to get an appropriate tail data.
    const int shift = std::min(maxTailLength / 2, fetched.body.length / 2);

    if (direction == FetchDirection::earlier)
    {
        auto newBodyStartTime = fetched.data.at(shift).startTimeMs;
        const auto bodyStartTime = fetched.data.begin()->startTimeMs;
        if (newBodyStartTime >= bodyStartTime && bodyStartTime.count())
            newBodyStartTime = bodyStartTime - 1ms;

        return FetchedDataCheckResult{false, newBodyStartTime};
    }

    auto newBodyStartTime = fetched.data.at(int(fetched.data.size()) - shift).startTimeMs;
    const auto bodyStartTime = fetched.data.rbegin()->startTimeMs;
    if (newBodyStartTime <= bodyStartTime && bodyStartTime != QnTimePeriod::kMaxTimeValue)
        newBodyStartTime = bodyStartTime + 1ms;

    return FetchedDataCheckResult{false, newBodyStartTime};
}

} // namespace nx::vms::client::core
