#include "script_runner.h"

#include "../model/configuration.h"
#include "config_bindings.h"
#include "lua_sandbox.h"

#include <QElapsedTimer>

namespace ct {

ScriptRunner::ScriptRunner(Configuration &config)
    : m_config(config)
{
}

void ScriptRunner::setOutputHandler(std::function<void(const QString &)> fn)
{
    m_output = std::move(fn);
}

ScriptResult ScriptRunner::run(const QString &source, const QString &chunkName)
{
    ScriptResult result;
    QElapsedTimer elapsed;
    elapsed.start();

    // The snapshot. copyContentTo covers everything content — buses, sections,
    // every calculation row, comments, the channel catalog — and deliberately
    // not the title, which the header classifies as document state. Scripts CAN
    // set the title, so it is saved by hand alongside.
    Configuration snapshot;
    m_config.copyContentTo(snapshot);
    const QString savedTitle = m_config.configTitle();

    BindingContext context;
    context.config = &m_config;

    LuaSandbox sandbox;
    sandbox.setTimeLimitMs(m_timeLimitMs);
    sandbox.setOutput(m_output);

    QString error;
    if (!sandbox.open(&error)) {
        result.error = error;
        result.elapsedMs = elapsed.elapsed();
        return result;
    }
    installConfigBindings(sandbox.state(), &context);

    if (sandbox.run(source, chunkName, &error)) {
        result.ok = true;
        result.mutated = context.mutated;
        if (context.mutated) {
            m_config.setDirty(true);
        }
    } else {
        result.error = error;
        // Restore UNCONDITIONALLY, not only when context.mutated is set. A
        // binding can change the live Configuration in place before it calls
        // touch() - l_setBus writes bus.enabled and then raises on a bad
        // rateKbps, for instance - so a partial change can land with mutated
        // still false. Restoring identical content when nothing changed is
        // harmless; the reverse copy goes through the same primitive that took
        // the snapshot, which test_lua round-trips. context.mutated now gates
        // only the success-path setDirty above.
        snapshot.copyContentTo(m_config);
        m_config.setConfigTitle(savedTitle);
        result.rolledBack = context.mutated;
    }

    result.elapsedMs = elapsed.elapsed();
    return result;
}

} // namespace ct
