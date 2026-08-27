#pragma once

#include "json_builder.hpp"

#include <cJSON.h>

#include <memory>

namespace tk {

struct JsonPrintDelete {
    void operator()(char* value) const noexcept { cJSON_free(value); }
};

using JsonPrintOwner = std::unique_ptr<char, JsonPrintDelete>;

// Exact JSON print/HTTP ordering shared by REST and MCP. Transport is deliberately a tiny seam:
// production adapters call esp_http_server, while the pinned-cJSON host gate records status/body
// calls. No success or caller-selected status is applied until the complete JSON text exists.
// A missing tree or print OOM sets 503 before sending the fixed fallback body.
template <typename Transport>
auto json_http_reply(Transport& transport,
                     JsonOwner root,
                     int success_status,
                     const char* oom_body)
    -> decltype(transport.send(oom_body)) {
    transport.set_json_type();
    if (!root) {
        transport.set_status(503);
        return transport.send(oom_body);
    }

    JsonPrintOwner body(cJSON_PrintUnformatted(root.get()));
    if (!body) {
        transport.set_status(503);
        return transport.send(oom_body);
    }

    if (success_status != 200) transport.set_status(success_status);
    return transport.send(body.get());
}

}  // namespace tk
