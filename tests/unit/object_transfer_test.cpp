#include "clip_worker/client/object_transfer.hpp"

#include <string>

#include <gtest/gtest.h>

namespace clip_worker::client {
namespace {

constexpr const char* kSensitiveTestUrl = "ftp://signed.example.invalid/private/object";

TEST(ObjectTransferTest, ReportsCurlReasonWithoutExposingPresignedUrl) {
    const ObjectTransfer transfer;
    try {
        static_cast<void>(transfer.download(kSensitiveTestUrl, 1024U));
        FAIL() << "Expected ObjectTransferError";
    } catch (const ObjectTransferError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Object download transfer failed:"), std::string::npos);
        EXPECT_EQ(message.find(kSensitiveTestUrl), std::string::npos);
        EXPECT_EQ(message.find("signed.example.invalid"), std::string::npos);
    }
}

TEST(ObjectTransferTest, StopsAStreamBeforeOpeningTheRemoteRequest) {
    const ObjectTransfer transfer;
    EXPECT_THROW(static_cast<void>(transfer.inspect(
                         "https://signed.example.invalid/private/object",
                         1024U, [] { return false; })),
                 ObjectTransferCancelledError);
}

}  // namespace
}  // namespace clip_worker::client
