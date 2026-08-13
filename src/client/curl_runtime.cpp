#include "clip_worker/client/curl_runtime.hpp"

#include <stdexcept>

#include <curl/curl.h>

namespace clip_worker::client {
namespace {

class CurlRuntime final {
public:
    CurlRuntime() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("Failed to initialize libcurl");
        }
    }

    ~CurlRuntime() {
        curl_global_cleanup();
    }
};

}  // namespace

void ensureCurlInitialized() {
    static const CurlRuntime runtime;
    static_cast<void>(runtime);
}

}  // namespace clip_worker::client
