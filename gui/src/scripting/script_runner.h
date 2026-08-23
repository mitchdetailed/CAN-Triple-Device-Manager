// Runs one Lua script against the open document, transactionally.
//
// Before the script runs, the document's content is snapshotted
// (Configuration::copyContentTo — the same primitive buildLiveView uses). If
// the script fails for any reason — error(), a type mistake three loops deep,
// the time limit, the memory cap — and it had already changed something, the
// snapshot is put back. A batch tool that dies halfway must not leave 400 of
// 900 channels renamed; "all of it or none of it" is the only contract a user
// can reason about.
//
// Headless on purpose: no widget includes, callbacks for output. The console
// dialog drives it interactively and test_lua drives it in CI, and they run
// the same code.
#pragma once

#include <QString>

#include <functional>

namespace ct {

class Configuration;

struct ScriptResult {
    bool ok = false;
    bool mutated = false;    // the script changed the document (and it was kept)
    bool rolledBack = false; // it changed the document, failed, and was undone
    QString error;           // Lua error with traceback, when !ok
    qint64 elapsedMs = 0;
};

class ScriptRunner
{
public:
    explicit ScriptRunner(Configuration &config);

    // print() output, one line per call.
    void setOutputHandler(std::function<void(const QString &)> fn);
    void setTimeLimitMs(qint64 ms) { m_timeLimitMs = ms; }

    // Each run gets a FRESH Lua state: no globals leak between runs, so a
    // script's behaviour never depends on what ran before it — the property
    // that makes a shared script reproducible on someone else's machine.
    ScriptResult run(const QString &source, const QString &chunkName);

private:
    Configuration &m_config;
    std::function<void(const QString &)> m_output;
    qint64 m_timeLimitMs = 10000;
};

} // namespace ct
