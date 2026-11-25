#include <lootlocker/client.h>

#include <lootlocker/curl_http_transport.h>
#include <lootlocker/http_transport.h>
#include <lootlocker/log.h>

#include "clock.h"
#include "executor.h"

#include <cstdio>
#include <random>
#include <utility>

namespace lootlocker {

struct Client::Impl {
    Config config;
    Error  config_error;

    // Sent as `LL-Instance-Identifier`. Generated once, at construction, so it is stable for
    // the client's lifetime and two clients in one process differ (D20).
    std::string instance_identifier;

    // The default logger, owned here because `Config::logger` is a `weak_ptr` and the
    // default has to live somewhere. Constructed lazily: a client that never logs should
    // not pay for it.
    std::shared_ptr<Logger> owned_logger;

    // The default transport, owned here for the same reason. Empty when curl was not built
    // in, which is a supported configuration: the adopter injects their own.
    std::shared_ptr<IHttpTransport> owned_transport;

    core::SteadyClock clock;
    std::unique_ptr<core::Executor> executor;
};

namespace {

// A random identifier for `LL-Instance-Identifier`.
//
// Not a security token, so `std::random_device` is the right tool rather than a
// cryptographic generator: it only has to be unlikely to collide between two clients in one
// process. Formatted as a UUID-shaped string because that is what the other SDKs send and
// what the backend's tracing expects to log.
std::string MakeInstanceIdentifier()
{
    std::random_device device;
    std::uint64_t high = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    std::uint64_t low  = (static_cast<std::uint64_t>(device()) << 32) ^ device();

    // Version 4, variant 1, so it reads as a UUID rather than as arbitrary hex.
    high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    low  = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buffer[37];
    std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>((high >> 32) & 0xFFFFFFFFULL),
                  static_cast<unsigned>((high >> 16) & 0xFFFFULL),
                  static_cast<unsigned>(high & 0xFFFFULL),
                  static_cast<unsigned>((low >> 48) & 0xFFFFULL),
                  static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFULL));
    return std::string(buffer);
}

// The logger the client will use: the injected one if it is alive, otherwise the default.
std::weak_ptr<Logger> ResolveLogger(const Config& config, std::shared_ptr<Logger>& owned)
{
    if (std::shared_ptr<Logger> injected = config.logger.lock()) {
        return injected;
    }
    if (!owned) {
        owned = std::make_shared<log::StderrLogger>(config.log_level);
    }
    return owned;
}

} // namespace

Client::Client(const Config& config) : m_impl(new Impl())
{
    m_impl->config = config;
    m_impl->config_error = ValidateConfig(config);
    m_impl->instance_identifier = MakeInstanceIdentifier();

    // A bad config leaves the client inert rather than half-built. The executor is still
    // constructed so `Update()` and `Submit` behave predictably, but nothing is ever sent
    // because every submission is rejected.
    std::weak_ptr<Logger> logger = ResolveLogger(config, m_impl->owned_logger);

    if (!m_impl->config_error.Ok()) {
        if (std::shared_ptr<Logger> sink = logger.lock()) {
            sink->Log(LogLevel::Error, "invalid configuration: " + m_impl->config_error.message);
        }
    }

    // The transport: the injected one wins, otherwise the libcurl default when it was
    // built. `Config::transport` is a `weak_ptr`, so an injected transport that has already
    // died falls through to the default rather than leaving the client with nothing.
    std::shared_ptr<IHttpTransport> transport = config.transport.lock();
    if (!transport) {
#if defined(LOOTLOCKER_HAVE_CURL)
        m_impl->owned_transport = std::make_shared<CurlHttpTransport>();
        transport = m_impl->owned_transport;
#else
        if (std::shared_ptr<Logger> sink = logger.lock()) {
            sink->Log(LogLevel::Warning,
                      "no transport: libcurl was not built in and none was injected, so no "
                      "request can be sent");
        }
#endif
    }

    core::ExecutorSettings settings;
    settings.queue = config.queue;

    // An invalid config means the policy was never validated, so it must not reach the
    // limiter: a rejected 2-billion-second window would still derive 2 billion buckets and
    // attempt a multi-gigabyte allocation during construction, which in a no-exceptions
    // build terminates the client. The client is inert anyway, so a disabled limiter is
    // exactly right.
    settings.rate_limit = config.rate_limit;
    if (!m_impl->config_error.Ok()) {
        settings.rate_limit.enabled = false;
    }

    settings.transport = transport;
    settings.clock = &m_impl->clock;
    settings.logger = logger;
    settings.config_error = m_impl->config_error;

    m_impl->executor.reset(new core::Executor(settings));
}

Client::~Client() = default;

bool Client::Update()
{
    // The executor is driven even when the config is invalid, because that is the only way
    // a rejected request's handler ever runs. `Submit` on an invalid client rejects with
    // `InvalidConfig` and queues the handler; skipping `Update()` here would leave those
    // handlers stranded for ever, which is the opposite of what "inert" should mean.
    return m_impl->executor->Update();
}

const Config& Client::GetConfig() const
{
    return m_impl->config;
}

const Error& Client::ConfigError() const
{
    return m_impl->config_error;
}

bool Client::SetTransport(std::weak_ptr<IHttpTransport> transport)
{
    return m_impl->executor->SetTransport(std::move(transport));
}

std::weak_ptr<IHttpTransport> Client::GetTransport() const
{
    return m_impl->executor->GetTransport();
}

const std::string& Client::GetInstanceIdentifier() const
{
    return m_impl->instance_identifier;
}

namespace core {

Executor& GetExecutor(Client& client)
{
    return *client.m_impl->executor;
}

} // namespace core

} // namespace lootlocker
