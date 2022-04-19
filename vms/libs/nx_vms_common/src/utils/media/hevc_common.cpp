// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "hevc_common.h"

#include <QtGlobal>

#include <nx/utils/log/log.h>
#include "utils/media/nalUnits.h"
#include "hevc_decoder_configuration_record.h"

namespace nx::media::hevc {

namespace {

static const uint8_t kPayloadHeaderForbiddenZeroBitMask = 0x80;
static const uint8_t kPayloadHeaderNalUnitTypeMask = 0x7e;
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
    static const uint16_t kPayloadHeaderLayerIdMask = 0x1f8;
#else
    static const uint16_t kPayloadHeaderLayerIdMask = 0xf801;
#endif
static const uint8_t kPayloadHeaderTidMask = 0x07;
static const uint8_t kStartFuPacketMask = 0x80;
static const uint8_t kEndFuPacketMask = 0x40;
static const uint8_t kFuNalUnitTypeMask = 0x3f;

} // namespace

bool FuHeader::decode(const uint8_t* const payload, int payloadLength)
{
    if (payloadLength < 1)
        return false;

    startFlag = *payload & kStartFuPacketMask;
    endFlag = *payload & kEndFuPacketMask;
    unitType = (NalUnitType)(*payload & kFuNalUnitTypeMask);

    return true;
}

bool NalUnitHeader::decode(const uint8_t* const payload, int payloadLength, QString* outErrorString)
{
    if (outErrorString)
        outErrorString->clear();

    if (payloadLength < kTotalLength)
    {
        if (outErrorString)
            *outErrorString = NX_FMT("Payload length is too short: %1 byte(s)", payloadLength);
        return false;
    }

    if (payload[0] & kPayloadHeaderForbiddenZeroBitMask)
        NX_WARNING(this, "Incorrect hevc nal unit header, forbidden zero bit is set!");

    unitType = (NalUnitType)((payload[0] & kPayloadHeaderNalUnitTypeMask) >> 1);
    layerId = ((*((uint16_t*)payload) & kPayloadHeaderLayerIdMask) >> kTidFieldLengthBit);
    tid = payload[1] & kPayloadHeaderTidMask;

    return true;
}

PacketType fromNalUnitTypeToPacketType(NalUnitType unitType)
{
    switch (unitType)
    {
        case NalUnitType::unspec48:
            return PacketType::aggregationPacket;
        case NalUnitType::unspec49:
            return PacketType::fragmentationPacket;
        case NalUnitType::unspec50:
            return PacketType::paciPacket;
        default:
            return PacketType::singleNalUnitPacket;
    }
}

bool isValidUnitType(int unitType)
{
    return unitType >= (int)NalUnitType::trailN
        && unitType <= (int)NalUnitType::unspec63;
}

bool isRandomAccessPoint(NalUnitType unitType)
{
    return unitType >= NalUnitType::blaWLp
        && unitType <= NalUnitType::craNut;
}

bool isLeadingPicture(NalUnitType unitType)
{
    return unitType >= NalUnitType::radlN
        && unitType <= NalUnitType::raslR;
}

bool isTrailingPicture(NalUnitType unitType)
{
    return unitType == NalUnitType::trailN
        || unitType == NalUnitType::trailR;
}

bool isParameterSet(NalUnitType unitType)
{
    return unitType == NalUnitType::vpsNut
        || unitType == NalUnitType::spsNut
        || unitType == NalUnitType::ppsNut;
}

bool isSlice(NalUnitType unitType)
{
    return (unitType >= NalUnitType::trailN && unitType <= NalUnitType::raslR)
        || (unitType >= NalUnitType::blaWLp && unitType <= NalUnitType::rsvIrapVcl23);
}

bool isNewAccessUnit(NalUnitType unitType)
{
    return (unitType >= NalUnitType::vpsNut && unitType <= NalUnitType::eobNut)
        || (unitType == NalUnitType::prefixSeiNut)
        || (unitType >= NalUnitType::rsvNvcl41 && unitType <= NalUnitType::rsvNvcl44)
        || (unitType >= NalUnitType::unspec48 && unitType <= NalUnitType::unspec55);
}

std::vector<uint8_t> buildExtraDataAnnexB(const uint8_t* data, int32_t size)
{
    // TODO HEVCDecoderConfigurationRecord hvcc;
    // FFmpeg support both hvcc and nal unit formats, so we can write raw nal units.
    std::vector<uint8_t> extraData;
    std::vector<uint8_t> startcode = {0x0, 0x0, 0x0, 0x1};
    auto nalUnits = nx::media::nal::findNalUnitsAnnexB(data, size);
    for (const auto& nalu: nalUnits)
    {
        NalUnitType unitType = (NalUnitType)((*nalu.data & kPayloadHeaderNalUnitTypeMask) >> 1);
        if (unitType == NalUnitType::vpsNut
            || unitType == NalUnitType::ppsNut
            || unitType == NalUnitType::spsNut)
        {
            extraData.insert(extraData.end(), startcode.begin(), startcode.end());
            extraData.insert(extraData.end(), nalu.data, nalu.data + nalu.size);
        }
    }
    return extraData;
}

std::vector<uint8_t> buildExtraData(const uint8_t* data, int32_t size)
{
    std::vector<std::vector<uint8_t>> spsVector;
    std::vector<std::vector<uint8_t>> ppsVector;
    std::vector<std::vector<uint8_t>> vpsVector;
    auto nalUnits = nx::media::nal::findNalUnitsAnnexB(data, size);
    for (const auto& nalu: nalUnits)
    {
        NalUnitType unitType = (NalUnitType)((*nalu.data & kPayloadHeaderNalUnitTypeMask) >> 1);
        if (unitType == NalUnitType::vpsNut)
            vpsVector.push_back(std::vector<uint8_t>(nalu.data, nalu.data + nalu.size));
        if (unitType == NalUnitType::ppsNut)
            ppsVector.push_back(std::vector<uint8_t>(nalu.data, nalu.data + nalu.size));
        if (unitType == NalUnitType::spsNut)
            spsVector.push_back(std::vector<uint8_t>(nalu.data, nalu.data + nalu.size));
    }

    if (spsVector.empty() || ppsVector.empty() || vpsVector.empty())
    {
        NX_WARNING(NX_SCOPE_TAG, "Failed to write h264 extra data, no sps/pps/vps found");
        return std::vector<uint8_t>();
    }
    std::vector<uint8_t> extradata;
    // TODO;
    return extradata;
}

} // namespace nx::media::hevc
