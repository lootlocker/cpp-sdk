// T9's verification: the error model.
//
// The two things the task asks for are here: `RawBody()` is populated on both a success
// and a failure, and `Result<T>` never throws.

#include "test_harness.h"

#include <lootlocker/result.h>

#include <string>
#include <utility>

namespace {

using lootlocker::Empty;
using lootlocker::Error;
using lootlocker::ErrorCode;
using lootlocker::Result;

void TestSuccessCarriesValueAndBody()
{
    const Result<int> r = Result<int>::Ok(42, "{\"value\":42}");

    CHECK(r.Ok());
    CHECK(static_cast<bool>(r));
    CHECK_EQ_INT(r.Value(), 42);
    CHECK_EQ(r.RawBody(), "{\"value\":42}");
}

void TestFailureCarriesErrorAndBody()
{
    Error e;
    e.code = ErrorCode::Http;
    e.status_code = 404;
    e.message = "not found";
    e.api_code = "player_not_found";
    e.request_id = "req-1";

    const Result<int> r = Result<int>::Fail(e, "<html>404</html>");

    CHECK(!r.Ok());
    CHECK(!static_cast<bool>(r));
    CHECK(r.Error().code == ErrorCode::Http);
    CHECK_EQ_INT(r.Error().status_code, 404);
    CHECK_EQ(r.Error().message, "not found");
    CHECK_EQ(r.Error().api_code, "player_not_found");
    CHECK_EQ(r.Error().request_id, "req-1");

    // D7: the raw body is carried on failure too, so an adopter can see the Cloudflare
    // page for themselves.
    CHECK_EQ(r.RawBody(), "<html>404</html>");
}

void TestRawBodyIsCarriedOnBothPaths()
{
    // The task's verification, stated as one assertion: a 200 and a 4xx both carry it.
    const Result<Empty> ok = Result<Empty>::Ok(Empty{}, "{\"ok\":true}");
    const Result<Empty> bad = Result<Empty>::Fail(Error{}, "{\"error\":\"nope\"}");

    CHECK_EQ(ok.RawBody(), "{\"ok\":true}");
    CHECK_EQ(bad.RawBody(), "{\"error\":\"nope\"}");
}

void TestTakeRawBodyMovesItOut()
{
    Result<int> r = Result<int>::Ok(1, "body");
    const std::string taken = r.TakeRawBody();

    CHECK_EQ(taken, "body");
    CHECK_EQ(r.RawBody(), "");   // moved out, so empty
}

void TestEmptyBodyIsAllowed()
{
    // A 204 has no body. That is not an error.
    const Result<Empty> r = Result<Empty>::Ok(Empty{});
    CHECK(r.Ok());
    CHECK_EQ(r.RawBody(), "");
}

void TestErrorOkHelper()
{
    Error e;
    CHECK(e.Ok());   // default is ErrorCode::None

    e.code = ErrorCode::Transport;
    CHECK(!e.Ok());
}

void TestToStringCoversEveryCode()
{
    // Every enumerator has a name, and none of them is "Unknown".
    const ErrorCode codes[] = {
        ErrorCode::None,       ErrorCode::Transport,   ErrorCode::Http,
        ErrorCode::MalformedResponse, ErrorCode::RateLimited, ErrorCode::QueueFull,
        ErrorCode::Cancelled,  ErrorCode::InvalidConfig, ErrorCode::Unsupported,
    };

    for (const ErrorCode c : codes) {
        const std::string name = lootlocker::ToString(c);
        CHECK(!name.empty());
        CHECK(name != "Unknown");
    }
}

void TestResultIsCopyableWhenValueIs()
{
    const Result<int> a = Result<int>::Ok(7, "b");
    const Result<int> b = a;

    CHECK_EQ_INT(b.Value(), 7);
    CHECK_EQ(b.RawBody(), "b");
}

} // namespace

int main()
{
    TestSuccessCarriesValueAndBody();
    TestFailureCarriesErrorAndBody();
    TestRawBodyIsCarriedOnBothPaths();
    TestTakeRawBodyMovesItOut();
    TestEmptyBodyIsAllowed();
    TestErrorOkHelper();
    TestToStringCoversEveryCode();
    TestResultIsCopyableWhenValueIs();

    return testing::Report();
}
