// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <gtest/gtest.h>

#include <chrono>
#include <nx/utils/uuid.h>
#include <nx/vms/client/core/event_search/event_search_globals.h>
#include <nx/vms/client/core/event_search/utils/event_search_item_helper.h>

namespace {

using namespace std::chrono;

struct Item
{
    QnUuid id;
    QString name;
    milliseconds timestamp;

    QString toString() const
    {
        return name.isEmpty()
            ? QString::number(timestamp.count())
            : QString::number(timestamp.count()) + " " + name;
    }

    bool operator==(const Item& other) const
    {
        return id == other.id
            && timestamp == other.timestamp
            && name == other.name;
    }

    static Item create(milliseconds timestamp, const QString& name = {})
    {
        return { QnUuid::createUuid(), name, timestamp};
    }

    static Item createCopy(const Item& other, const QString& name = {})
    {
        return { other.id, name, other.timestamp};
    }

    static Item createCopy(const Item& other, milliseconds timestamp)
    {
        return { other.id, other.name, timestamp};
    }
};

struct Accessor
{
    using ItemType = Item;
    static milliseconds timestamp(const ItemType& item)
    {
        return item.timestamp;
    }

    static QnUuid guid(const ItemType& item)
    {
        return item.id;
    }

    static QString toString(const ItemType& item)
    {
        return item.toString();
    }
};

using TestItems = std::vector<Item>;

static const TestItems kCurrentItems = {
    Item::create(100ms), // 0.
    Item::create(90ms),  // 1.
    Item::create(80ms),  // 2.
    Item::create(70ms),  // 3.
    Item::create(60ms),  // 4.
    Item::create(50ms)   // 5.
};

} // namespace

using namespace nx::vms::client::core;
using FetchDirection = EventSearch::FetchDirection;

// No actual tail tests.

TEST(EventSearchReorderAroundReference, noTailForEarlierFetch)
{
    auto fetched = kCurrentItems;  //< Same source items.
    const auto result = reorderAroundReferencePoint<Accessor>(
        kCurrentItems, fetched, FetchDirection::earlier);

    ASSERT_EQ(result.tail.length, 0); //< No actual tail
}

TEST(EventSearchReorderAroundReference, noTailForLaterFetch)
{
    auto fetched = TestItems(kCurrentItems.rbegin(), kCurrentItems.rend()); //< Same source items.
    const auto result = reorderAroundReferencePoint<Accessor>(
        kCurrentItems, fetched, FetchDirection::later);

    ASSERT_EQ(result.tail.length, 0); //< No actual tail
}

TEST(EventSearchReorderAroundReference, noTailForEarlierWithOnlyRemovedItems)
{
    TestItems fetched = {kCurrentItems[1]}; //< Removed all items from the source set except one.
    const auto result = reorderAroundReferencePoint<Accessor>(
        kCurrentItems, fetched, FetchDirection::earlier);

    ASSERT_EQ(result.tail.length, 0); //< No actual tail
}

TEST(EventSearchReorderAroundReference, noTailForLaterFetchWithOnlyRemovedItems)
{
    TestItems fetched = {kCurrentItems[1]}; //< Removed all items from the source set except one.
    const auto result = reorderAroundReferencePoint<Accessor>(
        kCurrentItems, fetched, FetchDirection::later);

    ASSERT_EQ(result.tail.length, 0); //< No actual tail
}

// Other tests.

TEST(EventSearchReorderAroundReference, upperTailWithChangedReferencePointForLaterFetch)
{
    const auto tailItem = Item::create(kCurrentItems.front().timestamp + 10ms, "expected_tail");
    const auto expectedReferenceItem = Item::createCopy(kCurrentItems[3], "reference_point");

    TestItems fetched = {
        // Some new item with the not existing timestamp which is below reference point.
        Item::create(kCurrentItems[5].timestamp - 10ms, "below_point3"),

        // Some new item with the existing timestamp.
        Item::create(kCurrentItems[4].timestamp, "below_point2"),

        // Old item below reference point.
        Item::createCopy(kCurrentItems[4], "below_point_1"),

        // Reference + same timestamp items.
        Item::create(expectedReferenceItem.timestamp, "expected_tail_item_4"),
        expectedReferenceItem, //< Expected reference point.
        Item::create(expectedReferenceItem.timestamp, "expected_tail_item_3"),

        // New items with the same timestamp as in existing items.
        Item::create(kCurrentItems[2].timestamp, "expected_tail_item_2"),

        tailItem
    };

    const auto result = reorderAroundReferencePoint<Accessor>(
        kCurrentItems, fetched, FetchDirection::later);

    ASSERT_EQ(result.tail.length, 4);
    ASSERT_EQ(result.body.length, 4);
    ASSERT_EQ((result.data.begin() + result.body.offset)->id, expectedReferenceItem.id);
}

TEST(EventSearchReorderAroundReference, lowerTailWithChangedReferencePointForEarlierFetch)
{
    const auto tailItem = Item::create(kCurrentItems.back().timestamp - 10ms, "expected_tail");
    const auto expectedReferenceItem = Item::createCopy(kCurrentItems[2], "reference_point");

    TestItems fetched = {
        // Some new item with the not existing timestamp which is above reference point.
        Item::create(kCurrentItems[0].timestamp + 10ms, "above_point3"),

        // Some new item with the existing timestamp.
        Item::create(kCurrentItems[1].timestamp, "above_point2"),

        Item::createCopy(kCurrentItems[1], "above_point_1"), //< Old item above reference point.

        // Reference + same timestamp items.
        Item::create(expectedReferenceItem.timestamp, "expected_tail_item_3"),
        expectedReferenceItem, //< Expected reference point.
        Item::create(expectedReferenceItem.timestamp, "expected_tail_item_2"),

        // New items with the same timestamp as in existing items.
        Item::create(kCurrentItems[4].timestamp, "expected_tail_item_1"),

        tailItem
    };

    const auto result = reorderAroundReferencePoint<Accessor>(
        kCurrentItems, fetched, FetchDirection::earlier);

    ASSERT_EQ(result.tail.length, 4);
    ASSERT_EQ(result.body.length, 4);
    ASSERT_EQ((result.data.begin() + result.body.length - 1)->id, expectedReferenceItem.id);
}

TEST(EventSearchReorderAroundReference, fetchLaterWithMovedSourceItem)
{
    const auto expectedReferenceItem = Item::createCopy(kCurrentItems[2], "reference_item");
    const auto belowItem = Item::createCopy(kCurrentItems[3], "below_item");
    const TestItems source = { expectedReferenceItem, belowItem };

    const auto tail = Item::create(kCurrentItems[0].timestamp, "expected_tail");
    TestItems fetched = {
        expectedReferenceItem,

        // Move some item above the reference point.
        Item::createCopy(belowItem, tail.timestamp + 10ms),

        tail
    };

    const auto result = reorderAroundReferencePoint<Accessor>(
        source, fetched, FetchDirection::later);

    ASSERT_EQ(result.tail.offset, 0);
    ASSERT_EQ(result.tail.length, 2);
    ASSERT_EQ(result.body.offset, result.tail.length);
    ASSERT_EQ(result.body.length, 1);

}

TEST(EventSearchReorderAroundReference, fetchEarlierWithMovedSourceItem)
{
    const auto aboveItem = Item::createCopy(kCurrentItems[2], "above_item");
    const auto expectedReferenceItem = Item::createCopy(kCurrentItems[3], "reference_item");
    const TestItems source = { aboveItem, expectedReferenceItem };

    const auto tail = Item::create(kCurrentItems[5].timestamp, "expected_tail");

    TestItems fetched = {
        expectedReferenceItem,

        // Move some item above the reference point.
        Item::createCopy(aboveItem, tail.timestamp + 10ms),

        tail
    };

    const auto result = reorderAroundReferencePoint<Accessor>(
        source, fetched, FetchDirection::earlier);

    ASSERT_EQ(result.body.offset, 0);
    ASSERT_EQ(result.body.length, 1);
    ASSERT_EQ(result.tail.offset, result.body.length);
    ASSERT_EQ(result.tail.length, 2);
}
