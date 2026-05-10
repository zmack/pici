#include "core/otel_init.h"
#include <string_view>

#ifdef PI_CPP_OTEL_SDK_ENABLED

#include <chrono>
#include <memory>
#include <string>

#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

namespace pi::core {

void init_otel(std::string_view endpoint) {
  namespace otlp = opentelemetry::exporter::otlp;
  namespace sdk = opentelemetry::sdk::trace;

  otlp::OtlpHttpExporterOptions exp_opts;
  exp_opts.url = std::string(endpoint) + "/v1/traces";

  auto exporter = otlp::OtlpHttpExporterFactory::Create(exp_opts);

  sdk::BatchSpanProcessorOptions bp_opts;
  bp_opts.schedule_delay_millis = std::chrono::milliseconds(1000);

  auto processor =
      sdk::BatchSpanProcessorFactory::Create(std::move(exporter), bp_opts);
  auto provider = sdk::TracerProviderFactory::Create(std::move(processor));

  opentelemetry::trace::Provider::SetTracerProvider(std::move(provider));
}

void shutdown_otel() {
  auto p = opentelemetry::trace::Provider::GetTracerProvider();
  if (auto sdk =
          std::dynamic_pointer_cast<opentelemetry::sdk::trace::TracerProvider>(
              p)) {
    sdk->ForceFlush(std::chrono::seconds(5));
    sdk->Shutdown();
  }
}

} // namespace pi::core

#else

namespace pi::core {
void init_otel(std::string_view) {}
void shutdown_otel() {}
} // namespace pi::core

#endif
