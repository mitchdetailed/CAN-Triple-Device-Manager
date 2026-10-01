#include "unit_target.h"

#include <QSet>

#include "configuration.h"
#include "validation.h"

namespace ct {

namespace {

// What the configuration is judged to be with `target` as its target: its
// validation errors and warnings, as Check Channels words them, then the
// mapper's. Info lines are left out: they report, they do not warn.
void judge(const Configuration &config, const DeviceCapacity &target,
           const std::function<MappingResult(const Configuration &)> &map, QStringList *errors,
           QStringList *warnings)
{
    Configuration copy;
    config.copyContentTo(copy);
    copy.setCapacity(target);
    for (const ValidationIssue &vi : validateConfiguration(copy)) {
        const QString line =
            vi.location.isEmpty() ? vi.message : vi.location + QStringLiteral(": ") + vi.message;
        if (vi.severity == ValidationIssue::Error)
            errors->append(line);
        else if (vi.severity == ValidationIssue::Warning)
            warnings->append(line);
    }
    const MappingResult mapped = map(copy);
    *errors += mapped.errors;
    *warnings += mapped.warnings;
}

// The lines of `a` that `b` does not have, once each, in `a`'s order.
QStringList onlyIn(const QStringList &a, const QStringList &b)
{
    const QSet<QString> other(b.begin(), b.end());
    QStringList out;
    for (const QString &line : a)
        if (!other.contains(line) && !out.contains(line))
            out.append(line);
    return out;
}

} // namespace

UnitTargetCheck checkUnitTarget(const Configuration &config, const DeviceCapacity &unit,
                                const std::function<MappingResult(const Configuration &)> &map)
{
    UnitTargetCheck out;
    out.differs = !config.capacity().sameTarget(unit);
    if (!out.differs)
        return out;
    QStringList targetErrors, targetWarnings, unitErrors, unitWarnings;
    judge(config, config.capacity(), map, &targetErrors, &targetWarnings);
    judge(config, unit, map, &unitErrors, &unitWarnings);
    out.problems = onlyIn(unitErrors, targetErrors);
    out.changes = onlyIn(unitWarnings, targetWarnings);
    return out;
}

QString targetName(const DeviceCapacity &target)
{
    return target.label.isEmpty() ? QStringLiteral("this program's built-in numbers")
                                  : target.label;
}

} // namespace ct
