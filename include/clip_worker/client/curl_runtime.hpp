#pragma once

namespace clip_worker::client {

/** Ensures libcurl remains initialized for every HTTP client in the process. */
void ensureCurlInitialized();

}  // namespace clip_worker::client
