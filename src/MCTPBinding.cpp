#include "MCTPBinding.hpp"

#include <endian.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <xyz/openbmc_project/MCTP/Base/server.hpp>

#include "libmctp-msgtypes.h"
#include "libmctp-vdpci.h"
#include "libmctp.h"

using json = nlohmann::json;
using mctp_base = sdbusplus::xyz::openbmc_project::MCTP::server::Base;

std::string reqRespDataFile = "/usr/share/mctp-emulator/req_resp.json";

std::shared_ptr<sdbusplus::asio::dbus_interface> mctpInterface;
std::string mctpIntf = "xyz.openbmc_project.MCTP.Base";
bool timerExpired = true;

static std::unique_ptr<boost::asio::steady_timer> delayTimer;
static std::vector<std::pair<
    int, std::tuple<uint8_t, uint8_t, uint8_t, bool, std::vector<uint8_t>>>>
    respQueue;

constexpr int retryTimeMilliSec = 10;

static void sendMessageReceivedSignal(uint8_t msgType, uint8_t srcEid,
                                      uint8_t msgTag, bool tagOwner,
                                      std::vector<uint8_t> response)
{
    auto msgSignal = bus->new_signal("/xyz/openbmc_project/mctp",
                                     mctpIntf.c_str(), "MessageReceivedSignal");
    msgSignal.append(msgType, srcEid, msgTag, tagOwner, response);
    msgSignal.signal_send();
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "Response signal sent");
}

static std::string getMessageType(uint8_t msgType)
{
    // TODO: Support for OEM message types
    std::string msgTypeValue = "Unknown";
    switch (msgType)
    {
        case MCTP_MESSAGE_TYPE_MCTP_CTRL: // 0x00
            msgTypeValue = "MctpControl";
            break;
        case MCTP_MESSAGE_TYPE_PLDM: // 0x01
            msgTypeValue = "PLDM";
            break;
        case MCTP_MESSAGE_TYPE_NCSI: // 0x02
            msgTypeValue = "NCSI";
            break;
        case MCTP_MESSAGE_TYPE_ETHERNET: // 0x03
            msgTypeValue = "Ethernet";
            break;
        case MCTP_MESSAGE_TYPE_NVME: // 0x04
            msgTypeValue = "NVMeMgmtMsg";
            break;
        case MCTP_MESSAGE_TYPE_SPDM: // 0x05
            msgTypeValue = "SPDM";
            break;
        case MCTP_MESSAGE_TYPE_VDPCI: // 0x7E
            msgTypeValue = "VDPCI";
            break;
        case MCTP_MESSAGE_TYPE_VDIANA: // 0x7F
            msgTypeValue = "VDIANA";
            break;
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        ("Message Type: " + msgTypeValue).c_str());
    return msgTypeValue;
}

void processResponse()
{
    timerExpired = false;
    delayTimer->expires_after(std::chrono::milliseconds(retryTimeMilliSec));
    delayTimer->async_wait([](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            // timer aborted do nothing
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "delayTimer operation_aborted");
            return;
        }
        else if (ec)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Timer failed");
            return;
        }

        respQueue.erase(
            std::remove_if(respQueue.begin(), respQueue.end(),
                           [](auto& resp) {
                               if (resp.first > 0)
                               {
                                   return false;
                               }

                               uint8_t msgType;
                               uint8_t srcEid;
                               uint8_t msgTag;
                               bool tagOwner;
                               std::vector<uint8_t> response;
                               std::tie(msgType, srcEid, msgTag, tagOwner,
                                        response) = resp.second;
                               sendMessageReceivedSignal(
                                   msgType, srcEid, msgTag, tagOwner, response);
                               return true;
                           }),
            respQueue.end());

        if (respQueue.empty())
        {
            delayTimer->cancel();
            timerExpired = true;
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "Response queue empty, canceling timer");
        }
        else
        {
            std::for_each(respQueue.begin(), respQueue.end(),
                          [](auto& resp) { resp.first -= retryTimeMilliSec; });
            processResponse();
        }
    });
}

static std::unordered_map<uint16_t, std::string> vendorMap = {
    {0x8086, "Intel"},
};

void processMctpCommand(uint8_t dstEid, const std::vector<uint8_t>& payload)
{
    uint8_t msgType;
    uint8_t srcEid = dstEid;
    uint8_t msgTag = 0;    // Hardcode Message Tag until a usecase arrives
    bool tagOwner = false; // This is false for responders
    std::string messageType;

    msgType = payload.at(0);

    std::ifstream jsonFile(reqRespDataFile);
    if (!jsonFile.good())
    {
        std::cerr << "unable to open " << reqRespDataFile << "\n";
        return;
    }

    json reqResp = json::parse(jsonFile, nullptr, false);

    json reqRespData = nullptr;
    try
    {
        messageType = getMessageType(msgType);
        reqRespData = reqResp[messageType];
    }
    catch (json::exception& e)
    {
        std::cerr << "message: " << e.what() << '\n'
                  << "exception id: " << e.id << std::endl;
        return;
    }

    std::vector<uint8_t> reqHeader;
    if (messageType == "VDPCI")
    {
        if (payload.size() < sizeof(mctp_vdpci_intel_hdr))
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "mctp-emulator: Invalid VDPCI message: Insufficient bytes in "
                "Payload");
            return;
        }

        const auto* vdpciMessage =
            reinterpret_cast<const mctp_vdpci_intel_hdr*>(payload.data());
        auto vendorIter =
            vendorMap.find(be16toh(vdpciMessage->vdpci_hdr.vendor_id));
        if (vendorIter == vendorMap.end())
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "mctp-emulator: Invalid VDPCI message: Unknown Vendor ID");
            return;
        }

        const auto& vendorString = vendorIter->second;
        if (vendorString == "Intel" && vdpciMessage->reserved != 0x80)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "mctp-emulator: Invalid VDPCI message: Unexpected value in "
                "reserved byte");
            return;
        }
        try
        {
            auto vendorTypeCode =
                std::to_string(vdpciMessage->vendor_type_code);
            reqRespData = reqRespData[vendorString][vendorTypeCode];
        }
        catch (json::exception& e)
        {
            std::cerr << "message: " << e.what() << '\n'
                      << "exception id: " << e.id << std::endl;
            return;
        }
        reqHeader.insert(reqHeader.end(), payload.begin(),
                         payload.begin() + sizeof(mctp_vdpci_intel_hdr));
    }
    else
    {
        reqHeader.push_back(msgType);
    }

    for (auto iter : reqRespData)
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "mctp-emulator: Parsing commands..");

        int processingDelayMilliSec = 0;
        std::vector<uint8_t> req = {};
        std::vector<uint8_t> response = {};
        try
        {
            if (iter.contains("processing-delay"))
            {
                processingDelayMilliSec = iter["processing-delay"];
            }
            req.assign(std::begin(iter["request"]), std::end(iter["request"]));
            response.assign(std::begin(iter["response"]),
                            std::end(iter["response"]));
        }
        catch (json::exception& e)
        {
            std::cerr << "message: " << e.what() << '\n'
                      << "exception id: " << e.id << std::endl;
            continue;
        }

        req.insert(req.begin(), reqHeader.begin(), reqHeader.end());
        if (req == payload)
        {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "mctp-emulator: Request Matched");

            if (processingDelayMilliSec == 0)
            {
                sendMessageReceivedSignal(msgType, srcEid, msgTag, tagOwner,
                                          response);
            }
            // Negative processing delay is considered as no response
            else if (processingDelayMilliSec == -1)
            {
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "mctp-emulator: No response, Infinite delay");
            }
            else if (processingDelayMilliSec > 0)
            {
                respQueue.push_back(
                    std::make_pair(processingDelayMilliSec,
                                   std::make_tuple(msgType, srcEid, msgTag,
                                                   tagOwner, response)));
                if (timerExpired)
                {
                    processResponse();
                }
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "mctp-emulator: Response added to process queue");
            }
            else
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "mctp-emulator: Invalid processing delay");
            }
            return;
        }
    }
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "mctp-emulator: No matching request found");
}

MctpBinding::MctpBinding(
    std::shared_ptr<sdbusplus::asio::object_server>& objServer,
    std::string& objPath)
{
    eid = 8;

    // TODO:Probably read these from mctp_config.json ?
    uint8_t bindingType = 2;
    uint8_t bindingMedium = 3;
    bool staticEidSupport = false;
    std::string uuid("MCTPDBG_EMULATOR");
    std::string bindingMode("xyz.openbmc_project.MCTP.BusOwner");
    delayTimer =
        std::make_unique<boost::asio::steady_timer>(bus->get_io_context());

    mctpInterface = objServer->add_interface(objPath, mctpIntf.c_str());

    mctpInterface->register_method(
        "SendMctpMessagePayload",
        [](uint8_t DstEid, uint8_t MsgTag, bool TagOwner,
           std::vector<uint8_t> payload) {
            uint8_t rc = 0;

            // Dummy entries to get around unused-variable compiler errors
            DstEid = DstEid;
            MsgTag = MsgTag;
            TagOwner = TagOwner;

            phosphor::logging::log<phosphor::logging::level::INFO>(
                "mctp-emulator: Received Payload");

            processMctpCommand(DstEid, payload);

            return rc;
        });

    mctpInterface->register_signal<uint8_t, uint8_t, uint8_t, bool,
                                   std::vector<uint8_t>>(
        "MessageReceivedSignal");

    mctpInterface->register_property("Eid", eid);

    // TODO:Use the enum from D-Bus interface
    mctpInterface->register_property("BindingID", bindingType);

    mctpInterface->register_property("BindingMediumID", bindingMedium);

    mctpInterface->register_property("StaticEidSupport", staticEidSupport);

    mctpInterface->register_property(
        "UUID", std::vector<uint8_t>(uuid.begin(), uuid.end()));

    mctpInterface->register_property("BindingMode", bindingMode);

    mctpInterface->initialize();
}
