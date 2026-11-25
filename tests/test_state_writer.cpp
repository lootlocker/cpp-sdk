// The IStateWriter seam (T12a).
//
// The verification the task asks for: each typed accessor round-trips its type; the null
// writer persists nothing and the client still works; a `uint64` above `INT64_MAX`
// round-trips exactly (the `player_id` case, D4).

#include <lootlocker/client.h>
#include <lootlocker/config.h>
#include <lootlocker/fake_http_transport.h>
#include <lootlocker/state_writer.h>

#include "test_harness.h"

#include <clocale>
#include <cstdio>
#include <memory>
#include <string>

namespace {

using lootlocker::Client;
using lootlocker::Config;
using lootlocker::FileStateWriter;
using lootlocker::NullStateWriter;
using lootlocker::IStateWriter;

// A scratch file that removes itself, so a failing test does not leave state behind for the
// next run to trip over.
struct TempFile {
    std::string path;

    explicit TempFile(const char* name)
    {
        path = std::string("test-state-") + name + ".txt";
        std::remove(path.c_str());
    }

    ~TempFile() { std::remove(path.c_str()); }
};

// --- typed round trips -----------------------------------------------------

void TestStringRoundTrip()
{
    TempFile file("string");
    FileStateWriter writer(file.path);

    writer.SetString("session_token", "abc123");

    std::string out;
    CHECK(writer.GetString("session_token", out));
    CHECK_EQ(out, "abc123");
}

void TestInt64RoundTrip()
{
    TempFile file("int64");
    FileStateWriter writer(file.path);

    writer.SetInt64("negative", -9007199254740993LL);

    std::int64_t out = 0;
    CHECK(writer.GetInt64("negative", out));
    CHECK_EQ_INT(static_cast<long long>(out), -9007199254740993LL);
}

// The `player_id` case (D4): a valid id above INT64_MAX must round-trip exactly. This is the
// reason the interface has a `uint64` accessor at all.
void TestUInt64AboveInt64MaxRoundTrips()
{
    TempFile file("uint64");
    FileStateWriter writer(file.path);

    const std::uint64_t big = 18446744073709551615ULL;   // UINT64_MAX
    writer.SetUInt64("player_id", big);

    std::uint64_t out = 0;
    CHECK(writer.GetUInt64("player_id", out));
    CHECK(out == big);

    // And it survives a reload, which is the whole point of persistence.
    FileStateWriter reopened(file.path);
    std::uint64_t reloaded = 0;
    CHECK(reopened.GetUInt64("player_id", reloaded));
    CHECK(reloaded == big);
}

void TestDoubleRoundTrip()
{
    TempFile file("double");
    FileStateWriter writer(file.path);

    writer.SetDouble("ratio", 0.1);

    double out = 0.0;
    CHECK(writer.GetDouble("ratio", out));
    CHECK(out == 0.1);   // exact, because the format round-trips
}

void TestBoolRoundTrip()
{
    TempFile file("bool");
    FileStateWriter writer(file.path);

    writer.SetBool("seen_before", true);
    writer.SetBool("check_dlc", false);

    bool out = false;
    CHECK(writer.GetBool("seen_before", out));
    CHECK(out);

    CHECK(writer.GetBool("check_dlc", out));
    CHECK(!out);
}

// --- type safety -----------------------------------------------------------

// A key written as one type is not readable as another. Guessing is how a corrupt store
// becomes a wrong player id.
void TestTypeMismatchIsNotConverted()
{
    TempFile file("mismatch");
    FileStateWriter writer(file.path);

    writer.SetString("player_id", "12345");

    std::uint64_t out = 0;
    CHECK(!writer.GetUInt64("player_id", out));
    CHECK_EQ_INT(static_cast<long long>(out), 0);   // untouched
}

void TestAbsentKey()
{
    TempFile file("absent");
    FileStateWriter writer(file.path);

    std::string out = "untouched";
    CHECK(!writer.GetString("nope", out));
    CHECK_EQ(out, "untouched");
    CHECK(!writer.Has("nope"));
}

void TestErase()
{
    TempFile file("erase");
    FileStateWriter writer(file.path);

    writer.SetString("k", "v");
    CHECK(writer.Has("k"));
    writer.Erase("k");
    CHECK(!writer.Has("k"));

    // And the erase is persisted, not just cached.
    FileStateWriter reopened(file.path);
    CHECK(!reopened.Has("k"));
}

// --- persistence across instances ------------------------------------------

void TestSurvivesReopen()
{
    TempFile file("reopen");

    {
        FileStateWriter writer(file.path);
        writer.SetString("player_identifier", "guest-42");
        writer.SetUInt64("player_id", 9007199254740993ULL);
        writer.SetBool("seen_before", true);
    }

    FileStateWriter reopened(file.path);

    std::string identifier;
    CHECK(reopened.GetString("player_identifier", identifier));
    CHECK_EQ(identifier, "guest-42");

    std::uint64_t id = 0;
    CHECK(reopened.GetUInt64("player_id", id));
    CHECK(id == 9007199254740993ULL);

    bool seen = false;
    CHECK(reopened.GetBool("seen_before", seen));
    CHECK(seen);
}

// A corrupt line is skipped rather than losing the whole file. One bad line must not cost
// the player their session.
void TestCorruptLineIsSkipped()
{
    TempFile file("corrupt");

    {
        std::FILE* f = std::fopen(file.path.c_str(), "wb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            std::fputs("s:good=kept\n", f);
            std::fputs("this line has no separator\n", f);
            std::fputs("x:badtype=1\n", f);
            std::fputs("s:also_good=yes\n", f);
            std::fclose(f);
        }
    }

    FileStateWriter writer(file.path);

    std::string out;
    CHECK(writer.GetString("good", out));
    CHECK_EQ(out, "kept");

    CHECK(writer.GetString("also_good", out));
    CHECK_EQ(out, "yes");

    CHECK(!writer.Has("badtype"));
}

// A newline in a value must not break the file format. A player name could contain
// anything, and losing the whole write over a display name would be worse than replacing it.
void TestNewlineInValueIsSanitised()
{
    TempFile file("newline");
    FileStateWriter writer(file.path);

    writer.SetString("player_name", "line one\nline two");

    std::string out;
    CHECK(writer.GetString("player_name", out));
    CHECK_EQ(out, "line one line two");

    // The other keys still load, which is what proves the file was not corrupted.
    writer.SetString("after", "ok");
    FileStateWriter reopened(file.path);
    CHECK(reopened.GetString("after", out));
    CHECK_EQ(out, "ok");
}

// --- the null writer -------------------------------------------------------

void TestNullWriterPersistsNothing()
{
    NullStateWriter writer;

    writer.SetString("k", "v");
    writer.SetUInt64("player_id", 42);
    writer.SetBool("b", true);

    std::string s;
    std::uint64_t u = 0;
    bool b = false;

    CHECK(!writer.GetString("k", s));
    CHECK(!writer.GetUInt64("player_id", u));
    CHECK(!writer.GetBool("b", b));
    CHECK(!writer.Has("k"));
}

// --- the client seam -------------------------------------------------------

Config MakeConfig()
{
    Config config;
    config.domain_key   = "testdomain";
    config.game_key     = "testgamekey";
    config.game_version = "1.0.0";
    return config;
}

// A client with no injected writer still has one, and it is the file default.
void TestClientHasADefaultWriter()
{
    Client client(MakeConfig());
    CHECK(!client.GetStateWriter().expired());
}

// The default is constructed lazily, so a client that never persists anything performs no
// file I/O. That is what keeps the "no implicit config" non-negotiable true (D9): the SDK
// must not read a file on its own.
//
// The discriminator is *when* the read happens. `FileStateWriter` reads its file in its
// constructor, so the probe is deleted between constructing the client and first asking for
// the writer: a lazy default then finds nothing to load, an eager one has already loaded it.
void TestDefaultWriterIsLazy()
{
    const std::string probe = "lootlocker-state.txt";
    std::remove(probe.c_str());

    {
        std::FILE* f = std::fopen(probe.c_str(), "wb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            std::fputs("s:probe=loaded\n", f);
            std::fclose(f);
        }
    }

    Client client(MakeConfig());

    // Remove the file before the writer is ever asked for. An eager default has already read
    // it; a lazy one has not.
    std::remove(probe.c_str());

    std::shared_ptr<IStateWriter> writer = client.GetStateWriter().lock();
    CHECK(writer != nullptr);
    if (writer != nullptr) {
        std::string out;
        CHECK(!writer->GetString("probe", out));
    }

    std::remove(probe.c_str());
}

// An injected writer means the default is never constructed at all, so no file is touched
// even when the writer is used.
void TestInjectedWriterMeansNoFileIo()
{
    const std::string probe = "lootlocker-state.txt";
    std::remove(probe.c_str());

    std::shared_ptr<NullStateWriter> writer = std::make_shared<NullStateWriter>();

    Config config = MakeConfig();
    config.state_writer = writer;

    Client client(config);
    client.GetStateWriter().lock()->SetString("k", "v");

    std::FILE* f = std::fopen(probe.c_str(), "rb");
    CHECK(f == nullptr);
    if (f != nullptr) {
        std::fclose(f);
    }

    std::remove(probe.c_str());
}

// An injected writer is the one used.
void TestClientUsesInjectedWriter()
{
    std::shared_ptr<NullStateWriter> writer = std::make_shared<NullStateWriter>();

    Config config = MakeConfig();
    config.state_writer = writer;

    Client client(config);

    std::shared_ptr<IStateWriter> in_use = client.GetStateWriter().lock();
    CHECK(in_use != nullptr);
    CHECK(in_use.get() == writer.get());
}

// The null writer is a supported configuration: the client still works, it just does not
// persist.
void TestClientWorksWithNullWriter()
{
    std::shared_ptr<NullStateWriter> writer = std::make_shared<NullStateWriter>();
    std::shared_ptr<lootlocker::FakeHttpTransport> transport =
        std::make_shared<lootlocker::FakeHttpTransport>();

    Config config = MakeConfig();
    config.state_writer = writer;
    config.transport    = transport;

    Client client(config);

    CHECK(client.ConfigError().Ok());
    CHECK(!client.GetStateWriter().expired());

    // Nothing was written anywhere.
    std::string out;
    CHECK(!writer->GetString("session_token", out));
}

// `SetStateWriter` is allowed at any time, unlike `SetTransport`, because a writer has no
// in-flight state to strand.
void TestSetStateWriterAtRuntime()
{
    Client client(MakeConfig());

    std::shared_ptr<NullStateWriter> replacement = std::make_shared<NullStateWriter>();
    client.SetStateWriter(replacement);

    std::shared_ptr<IStateWriter> in_use = client.GetStateWriter().lock();
    CHECK(in_use != nullptr);
    CHECK(in_use.get() == replacement.get());
}

// An empty or dead writer falls back to the default rather than leaving the client with
// nothing. A game that wants no persistence injects a `NullStateWriter` explicitly, which is
// a different statement from "I forgot".
void TestSetStateWriterFallsBackWhenEmpty()
{
    Client client(MakeConfig());

    client.SetStateWriter(std::weak_ptr<IStateWriter>());
    CHECK(!client.GetStateWriter().expired());

    {
        std::shared_ptr<NullStateWriter> temporary = std::make_shared<NullStateWriter>();
        client.SetStateWriter(temporary);
    }
    // The injected writer died with its scope.
    CHECK(!client.GetStateWriter().expired());
}

// A double must round-trip even when the process locale uses a decimal comma. The file is a
// persisted format, so it has to be locale-independent: a file written under one locale must
// read back under another.
void TestDoubleIsLocaleIndependent()
{
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = previous == nullptr ? std::string("C") : std::string(previous);

    std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
    std::setlocale(LC_NUMERIC, "German_Germany.1252");

    TempFile file("locale");
    {
        FileStateWriter writer(file.path);
        writer.SetDouble("ratio", 0.1);
    }

    // Read back under the C locale, which is the "written on another machine" case.
    std::setlocale(LC_NUMERIC, "C");

    FileStateWriter reopened(file.path);
    double out = 0.0;
    const bool ok = reopened.GetDouble("ratio", out);

    std::setlocale(LC_NUMERIC, saved.c_str());

    CHECK(ok);
    CHECK(out == 0.1);
}

} // namespace

int main()
{
    TestStringRoundTrip();
    TestInt64RoundTrip();
    TestUInt64AboveInt64MaxRoundTrips();
    TestDoubleRoundTrip();
    TestBoolRoundTrip();
    TestTypeMismatchIsNotConverted();
    TestAbsentKey();
    TestErase();
    TestSurvivesReopen();
    TestCorruptLineIsSkipped();
    TestNewlineInValueIsSanitised();
    TestNullWriterPersistsNothing();
    TestClientHasADefaultWriter();
    TestDefaultWriterIsLazy();
    TestInjectedWriterMeansNoFileIo();
    TestClientUsesInjectedWriter();
    TestClientWorksWithNullWriter();
    TestSetStateWriterAtRuntime();
    TestSetStateWriterFallsBackWhenEmpty();
    TestDoubleIsLocaleIndependent();
    return testing::Report();
}
