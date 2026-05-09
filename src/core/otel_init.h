#pragma once

#include <string_view>

namespace pi::core {

// Initialise the global OTel tracer provider with a BatchSpanProcessor and
// OTLP/HTTP exporter pointed at `endpoint` (e.g. "http://localhost:4318").
// No-op when PI_CPP_OTEL_SDK_ENABLED is not defined.
// Must be called before any agent loop runs if real export is wanted.
void init_otel(std::string_view endpoint);

// Flush pending spans and shut down the tracer provider.
// Safe to call multiple times. Call before process exit.
void shutdown_otel();

} // namespace pi::core
