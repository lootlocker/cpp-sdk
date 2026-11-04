#include <lootlocker/result.h>

namespace lootlocker {

const char* ToString(ErrorCode code)
{
    switch (code) {
    case ErrorCode::None:              return "None";
    case ErrorCode::Transport:         return "Transport";
    case ErrorCode::Http:              return "Http";
    case ErrorCode::MalformedResponse: return "MalformedResponse";
    case ErrorCode::RateLimited:       return "RateLimited";
    case ErrorCode::QueueFull:         return "QueueFull";
    case ErrorCode::Cancelled:         return "Cancelled";
    case ErrorCode::InvalidConfig:     return "InvalidConfig";
    case ErrorCode::Unsupported:       return "Unsupported";
    }
    return "Unknown";
}

} // namespace lootlocker
