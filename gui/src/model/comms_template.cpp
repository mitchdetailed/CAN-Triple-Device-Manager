// Reading order for the template writer, reader and merge.
//
// comms_template.h is the specification — what a template is for, why it is
// binary, and what that is worth. This file is the three operations, and the
// only one with any real thinking in it is the third:
//
//   WRITE   Collect the sections and the channel definitions they depend on
//           into a JSON body, stamp it with the marker, the envelope version
//           and the .ct3 schema, and hand it to writeSecureFile(). Nothing here
//           encrypts anything itself; the container is the one in secure_file.h
//           that the .ct3s already uses, so a template inherits its
//           authenticated encryption, its noise carrier and its atomic write.
//
//   READ    That backwards, refusing anything whose body is not a template
//           before a caller can mistake it for one.
//
//   MERGE   The interesting half. A template arrives naming channels, and the
//           document it lands in has its own — some of them the same names
//           meaning the same thing, some of them the same names meaning
//           something else, some of them device channels that cannot be
//           displaced at all. Resolving that is what the rest of this file is,
//           and the rule it obeys is: NEVER change a channel the document
//           already had. Re-scaling somebody's existing "Coolant Temp" because
//           an imported template happened to disagree about its resolution
//           would silently change what every other message using it decodes to.
//           Renaming the incoming one is visible, reversible and says so in the
//           summary; overwriting is none of those.
#include "comms_template.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "../protocol/wire_structs.h" // MAX_CHANNEL_NAME_BYTES
#include "channel_catalog.h"
#include "configuration.h"            // configSchemaVersion()
#include "secure_file.h"
#include "user_paths.h"

#ifndef CT_APP_VERSION
#define CT_APP_VERSION "unknown"
#endif

namespace ct {

namespace {

// Body keys. Named once so the writer and the reader cannot drift.
const QLatin1String kKeyFileType("fileType");
const QLatin1String kKeyFormatVersion("formatVersion");
const QLatin1String kKeyConfigSchema("configSchema");
const QLatin1String kKeyWrittenBy("writtenBy");
const QLatin1String kKeyName("name");
const QLatin1String kKeyBus("bus");
const QLatin1String kKeyRate("rateKbps");
const QLatin1String kKeyDataRate("dataRateKbps");
const QLatin1String kKeyTermination("termination");
const QLatin1String kKeySections("sections");
const QLatin1String kKeyChannels("channels");

// Clip a name to what the device's signal label holds, without cutting a
// multi-byte codepoint in half. Lifted from the DBC import for the same reason
// it exists there — a name past the budget is truncated by the mapper later,
// silently — and kept identical in behaviour so the two importers cannot
// produce different names for the same input.
QString clipName(const QString &s, int budget)
{
    QByteArray utf8 = s.toUtf8();
    if (utf8.size() <= budget)
        return s;
    const bool splitsCodepoint = (quint8(utf8[budget]) & 0xC0) == 0x80;
    utf8.truncate(budget);
    if (splitsCodepoint) {
        while (!utf8.isEmpty() && (quint8(utf8.back()) & 0xC0) == 0x80)
            utf8.chop(1);
        if (!utf8.isEmpty())
            utf8.chop(1); // the lead byte of the split codepoint
    }
    return QString::fromUtf8(utf8).trimmed();
}

// Same quantity, same wire shape, same meaning per count. Category is left out
// on purpose: it decides which folder the channel appears under and nothing
// else, so a template whose author filed a channel differently should reuse the
// document's channel rather than fork it over a display detail.
bool sameChannelDefinition(const Channel &a, const Channel &b)
{
    return a.quantity == b.quantity && a.unit == b.unit && a.dataType == b.dataType
           && a.baseResolution == b.baseResolution && a.decimalPlaces == b.decimalPlaces
           && a.minValue == b.minValue && a.maxValue == b.maxValue;
}

// Every channel name a section names, in one place, so the writer and the
// merge's unresolved-reference sweep ask the same question. The three fields
// beyond the rows are easy to forget and each one is a real reference: the
// receive diagnostic, the CRC8 publish channel, and a Triggered message's
// condition.
QStringList mentionedChannels(const CommsSection &s)
{
    QStringList names;
    for (const CommsChannelRow &r : s.allRows())
        if (!r.channelName.isEmpty())
            names << r.channelName;
    if (!s.diagnosticChannel.isEmpty())
        names << s.diagnosticChannel;
    if (s.isCrc8() && !s.crcChannel.isEmpty())
        names << s.crcChannel;
    if (s.isTransmit() && !s.cyclic && !s.transmitCondition.isEmpty())
        names << s.transmitCondition;
    return names;
}

// THE DOCUMENT'S OWN user channels, and only those.
//
// Not ChannelCatalog::findByName(), which falls back to the device channels
// when no user channel matches — right for "does this name resolve to
// anything?", wrong for both questions asked here. A document is ALLOWED to
// hold a user channel that shadows a device channel: findByName says so in as
// many words, because such a channel predates the device one and re-typing it
// out from under the configuration would be worse than the shadowing.
//
// Asking findByName on the export side would then skip that channel's
// definition as though it were the device's, and the row would arrive in the
// target document bound to a value the FIRMWARE publishes — a receive message
// decoding into a channel the device also writes every tick. Silently binding
// to the wrong channel is worse than any of the alternatives, so the two are
// separated: the export walks the user channels, and the merge renames the
// incoming one rather than recreating the shadow in a document that does not
// have it yet.
Channel findUserChannel(const ChannelCatalog &catalog, const QString &name)
{
    for (const Channel &c : catalog.userChannels())
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
            return c;
    return {};
}

// A Triggered message's condition is a reference and NOT a definition, which is
// why it is asked about separately. Its channel is the OUTPUT of a User
// Condition, and a template carries no conditions — so writing a definition for
// it would create a bare channel that looks like the condition exists when
// nothing drives it, and the message would sit there never transmitting with no
// clue on screen. Left dangling instead, and reported.
bool isConditionReference(const CommsSection &s, const QString &name)
{
    return s.isTransmit() && !s.cyclic
           && s.transmitCondition.compare(name, Qt::CaseInsensitive) == 0;
}

// Every name used as a trigger condition ANYWHERE in the set, gathered before
// anything is exported. Asked across the whole template rather than per section
// because a name is a condition output or it is not — and a per-section test
// would answer differently depending on which section reached it first, which
// is to say the answer would depend on list order.
QSet<QString> conditionReferences(const QList<CommsSection> &sections)
{
    QSet<QString> names;
    for (const CommsSection &s : sections)
        if (s.isTransmit() && !s.cyclic && !s.transmitCondition.isEmpty())
            names.insert(s.transmitCondition.toLower());
    return names;
}

} // namespace

QString commsTemplateExtension()
{
    return QStringLiteral("ct3t");
}

QString commsTemplateFilter()
{
    return QStringLiteral("CAN Triple Communications Templates (*.ct3t);;All Files (*)");
}

CommsTemplate buildCommsTemplate(const ChannelCatalog &catalog, const BusConfig &bus,
                                 const QList<CommsSection> &sections, const QString &name)
{
    CommsTemplate tmpl;
    tmpl.name = name;
    tmpl.writtenBy = QStringLiteral(CT_APP_VERSION);
    tmpl.configSchema = configSchemaVersion();
    tmpl.hasBusSettings = true;
    tmpl.rateKbps = bus.rateKbps;
    tmpl.dataRateKbps = bus.dataRateKbps;
    tmpl.termination = bus.termination;
    tmpl.sections = sections;

    // Definitions for the USER channels the sections name, once each. Device
    // channels are skipped: they are compiled into every build, never stored in
    // a .ct3, and cannot be created — carrying one would be carrying a name the
    // target document already has and could not have lost.
    //
    // A name the catalogue does not know is skipped too. That is a dangling
    // reference in the SOURCE document — a channel deleted out from under a row
    // — and inventing a definition for it here would repair, in the template,
    // something that is still broken in the configuration it came from.
    const QSet<QString> conditions = conditionReferences(sections);
    QSet<QString> seen;
    for (const CommsSection &s : sections) {
        for (const QString &ref : mentionedChannels(s)) {
            const QString key = ref.toLower();
            if (seen.contains(key))
                continue;
            seen.insert(key);
            if (conditions.contains(key))
                continue;
            // Not gated on isDeviceChannel: a USER channel shadowing a device
            // name is still this document's own and still has to travel, or the
            // row arrives bound to the firmware's value. A name with no user
            // channel behind it — a real device channel, or a reference the
            // source document had already lost — exports nothing, which is
            // right for both.
            const Channel c = findUserChannel(catalog, ref);
            if (c.isValid())
                tmpl.channels.append(c);
        }
    }
    return tmpl;
}

bool writeCommsTemplate(const QString &path, const CommsTemplate &tmpl, QString *error)
{
    if (tmpl.sections.isEmpty()) {
        if (error)
            *error = QStringLiteral("There is nothing to save: no messages were selected.");
        return false;
    }

    QJsonObject body;
    body[kKeyFileType] = QLatin1String(kCommsTemplateFileType);
    body[kKeyFormatVersion] = kCommsTemplateFormatVersion;
    // The .ct3 schema the sections below are spelled at. CommsSection::fromJson
    // needs it and will not guess — see the note at its definition about what
    // applying a migration to a file that does not need one does to a
    // protection tier.
    body[kKeyConfigSchema] = tmpl.configSchema;
    // For people, never for the parser: which build produced this, the question
    // actually asked when a template turns up behaving oddly. configSchema is
    // the number that decides how to read it, and two numbers answering one
    // question is how they drift apart.
    body[kKeyWrittenBy] = tmpl.writtenBy;
    body[kKeyName] = tmpl.name;

    if (tmpl.hasBusSettings) {
        QJsonObject busObj;
        busObj[kKeyRate] = tmpl.rateKbps;
        busObj[kKeyDataRate] = tmpl.dataRateKbps;
        busObj[kKeyTermination] = tmpl.termination;
        body[kKeyBus] = busObj;
    }

    QJsonArray sectionArr;
    for (const CommsSection &s : tmpl.sections)
        sectionArr.append(s.toJson());
    body[kKeySections] = sectionArr;

    QJsonArray channelArr;
    for (const Channel &c : tmpl.channels)
        channelArr.append(c.toJson());
    body[kKeyChannels] = channelArr;

    // Compact. These bytes are the plaintext of an encrypted container and
    // indentation would only pad the ciphertext without making anything
    // legible — the same call saveSecureToFile makes for the same reason.
    QByteArray plain = QJsonDocument(body).toJson(QJsonDocument::Compact);

    // Standard mode: no password. A template is meant to be openable by anyone
    // holding this program — that is what makes it useful to hand over — while
    // being opaque to everything else. The container's password mode exists and
    // is not used here; wiring it up would be a second, differently-shaped
    // secret for the user to lose.
    SecureSaveOptions options;
    options.requirePassword = false;
    options.embeddedCommsKey = kNoAccessKey;

    const bool ok = writeSecureFile(path, plain, options, error);
    // The section detail this container exists to hide passed through this
    // buffer; do not leave it in freed heap.
    plain.fill('\0');
    return ok;
}

bool readCommsTemplate(const QString &path, CommsTemplate *out, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    if (!isSecureFile(path))
        return fail(QStringLiteral("This is not a communications template. A template is a "
                                   ".ct3t file written by Save… in Communications Setup."));

    QByteArray plain;
    SecureFileInfo info;
    if (!readSecureFile(path, QString(), &plain, &info, error))
        return false;

    const QJsonDocument doc = QJsonDocument::fromJson(plain);
    plain.fill('\0');
    if (!doc.isObject())
        return fail(QStringLiteral("This template file is damaged and cannot be read."));
    const QJsonObject body = doc.object();

    // THE marker check. Without it a .ct3s would decrypt cleanly here and then
    // parse as a template with no sections in it, and the user would be told
    // "this template is empty" about a file that is a complete configuration.
    if (body.value(kKeyFileType).toString() != QLatin1String(kCommsTemplateFileType))
        return fail(QStringLiteral("This is a secure configuration file (.ct3s), not a "
                                   "communications template. Open it with File > Open."));

    CommsTemplate tmpl;
    tmpl.formatVersion = body.value(kKeyFormatVersion).toInt(0);
    if (tmpl.formatVersion == 0)
        return fail(QStringLiteral("This template file is damaged and cannot be read."));
    if (tmpl.formatVersion > kCommsTemplateFormatVersion)
        return fail(QStringLiteral("This template was saved by a newer version of CAN Triple "
                                   "Device Manager and can't be loaded."));

    // A template written against a LATER .ct3 schema is refused for exactly the
    // reason readWrapper refuses a later .ct3: the section keys it carries are
    // not the ones this build knows how to read, and reading them anyway
    // produces a message that looks right and is not.
    tmpl.configSchema = body.value(kKeyConfigSchema).toInt(1);
    if (tmpl.configSchema > configSchemaVersion())
        return fail(QStringLiteral("This template was saved by a newer version of CAN Triple "
                                   "Device Manager and can't be loaded."));

    tmpl.writtenBy = body.value(kKeyWrittenBy).toString();
    tmpl.name = body.value(kKeyName).toString();

    if (body.contains(kKeyBus)) {
        const QJsonObject busObj = body.value(kKeyBus).toObject();
        tmpl.hasBusSettings = true;
        tmpl.rateKbps = busObj.value(kKeyRate).toInt(0);
        tmpl.dataRateKbps = busObj.value(kKeyDataRate).toInt(0);
        tmpl.termination = busObj.value(kKeyTermination).toBool(false);
    }

    for (const QJsonValue &v : body.value(kKeySections).toArray())
        tmpl.sections.append(CommsSection::fromJson(v.toObject(), tmpl.configSchema));
    for (const QJsonValue &v : body.value(kKeyChannels).toArray())
        tmpl.channels.append(Channel::fromJson(v.toObject()));

    if (tmpl.sections.isEmpty())
        return fail(QStringLiteral("This template contains no messages."));

    if (out)
        *out = tmpl;
    return true;
}

bool mergeCommsTemplate(ChannelCatalog &catalog, int busIndex, const CommsTemplate &tmpl,
                        CommsTemplateMerge *out, QString *error)
{
    if (tmpl.sections.isEmpty()) {
        if (error)
            *error = QStringLiteral("This template contains no messages.");
        return false;
    }

    CommsTemplateMerge result;
    result.sections = tmpl.sections;

    // Names already spoken for in the TARGET document, plus the ones this merge
    // hands out as it goes. Lower-cased, because every channel comparison in
    // this app is case-insensitive.
    QSet<QString> taken;
    for (const Channel &c : catalog.userChannels())
        taken.insert(c.name.toLower());
    for (const Channel &c : ChannelCatalog::deviceChannels())
        taken.insert(c.name.toLower());

    // PASS 1 — decide every final name before touching a single reference.
    //
    // Deciding and applying in one loop is the bug worth spelling out, because
    // it looks correct and is not: rename template channel "X" to "X 2" and the
    // rows that said "X" now say "X 2" — including, from that moment,
    // indistinguishably from the rows of the template's OWN channel called
    // "X 2". Renaming that one next would move both. So the mapping is computed
    // whole here and applied below, and no reference is ever renamed twice.
    struct Plan
    {
        Channel channel;   // the definition, under its final name
        QString from;      // the name the template's rows currently use
        QString to;        // the name they must end up using
        bool create = false;
    };
    QList<Plan> plans;

    for (const Channel &incoming : tmpl.channels) {
        if (incoming.name.isEmpty())
            continue;
        Plan plan;
        plan.channel = incoming;
        plan.from = incoming.name;
        plan.to = incoming.name;

        const bool isDevice = ChannelCatalog::isDeviceChannel(incoming.name);
        const Channel existing = findUserChannel(catalog, incoming.name);

        if (!isDevice && existing.isValid() && sameChannelDefinition(existing, incoming)) {
            // The document already has this channel and means the same thing by
            // it. Reuse, so loading a template twice does not produce a second
            // copy of every channel in it.
            plan.create = false;
            ++result.channelsReused;
            plans.append(plan);
            continue;
        }

        // A NAME OF ITS OWN, if the one it came with is not available.
        //
        // Two independent reasons it might not be, and the clip is applied
        // FIRST because it can create the second: a name over the device's label
        // budget is shortened, and the shortened form can land on a name that is
        // already spoken for. Testing the original against `taken` and only then
        // clipping — which is what this did — lets exactly that case through with
        // no disambiguator at all, and two channels then share a name.
        const QString root = incoming.name;
        QString candidate = clipName(root, MAX_CHANNEL_NAME_BYTES);
        const bool shortened = candidate != root;
        const bool collided = taken.contains(candidate.toLower());
        // The " 2" disambiguator has to fit the label budget too, so it eats
        // into the root rather than pushing the name back over it.
        for (int n = 2; taken.contains(candidate.toLower()); ++n) {
            const QString suffix = QStringLiteral(" %1").arg(n);
            candidate =
                clipName(root, MAX_CHANNEL_NAME_BYTES - int(suffix.toUtf8().size())) + suffix;
        }
        plan.to = candidate;
        plan.channel.name = candidate;

        // Both notes when both happened. They are different facts and the user
        // may need either: one says the name in the template was too long for
        // the device, the other says this configuration was already using it.
        if (shortened)
            result.notes.append(QStringLiteral("Channel '%1' shortened to fit — names are "
                                               "limited to %2 bytes on the device.")
                                    .arg(root)
                                    .arg(MAX_CHANNEL_NAME_BYTES));
        if (collided)
            result.notes.append(
                isDevice
                    ? QStringLiteral("Channel '%1' renamed to '%2' — that name belongs to a "
                                     "device channel.")
                          .arg(root, candidate)
                    : QStringLiteral("Channel '%1' renamed to '%2' — this configuration already "
                                     "has a different channel by that name, and it was left "
                                     "alone.")
                          .arg(root, candidate));

        plan.create = true;
        plan.channel.userDefined = true;
        taken.insert(plan.to.toLower());
        ++result.channelsCreated;
        plans.append(plan);
    }

    // PASS 2 — apply the mapping, in two rounds through a sentinel.
    //
    // The sentinel is what makes the mapping simultaneous rather than
    // sequential: every reference that must move goes to a name nothing else in
    // the document could hold, and only then to its destination. Two rounds of
    // renameChannelRefs() rather than a bespoke walk over the reference fields,
    // because that function is where the knowledge of WHICH fields are
    // references lives — a receive diagnostic, a CRC8 publish channel and a
    // Triggered message's condition are all easy to miss, and a second copy of
    // that list is a second copy to forget to update.
    //
    // U+0001 leads the sentinel: it cannot be typed into a channel name, no
    // catalogue holds one, and clipName never produces one, so no real name can
    // collide with it. Built with QChar(1) rather than written into a string
    // literal on purpose — "\x01ct3t" does not mean what it looks like, because
    // a hex escape has no length limit and swallows the 'c' as another digit.
    const auto sentinelFor = [](int i) {
        return QChar(1) + QStringLiteral("ct3t/") + QString::number(i);
    };
    for (int i = 0; i < plans.size(); ++i) {
        const Plan &plan = plans.at(i);
        if (plan.from == plan.to)
            continue;
        renameChannelRefs(result.sections, plan.from, sentinelFor(i));
    }
    for (int i = 0; i < plans.size(); ++i) {
        const Plan &plan = plans.at(i);
        if (plan.from == plan.to)
            continue;
        renameChannelRefs(result.sections, sentinelFor(i), plan.to);
    }

    // A RELAY OR ROUTE THAT NOW POINTS AT ITSELF. routeBusMask is the set of
    // buses to forward to, and it is meaningful only relative to the bus the
    // section sits on — so a rule saved from CAN 1 forwarding to CAN 2 and CAN 3
    // becomes, on CAN 2, a rule that forwards to itself. The mapper strips that
    // bit for a relay and validation reports it for a route; neither is a good
    // way to find out. Strip it here, where the bus actually changed, and say so.
    const int ownBit = 1 << busIndex;
    for (CommsSection &s : result.sections) {
        if (!s.isRelay() && !s.routeEnable)
            continue;
        if (!(s.routeBusMask & ownBit))
            continue;
        s.routeBusMask &= ~ownBit;
        result.notes.append(
            (s.routeBusMask & 0x7)
                ? QStringLiteral("'%1' forwarded to CAN %2, which is the bus you loaded it onto; "
                                 "that target was dropped.")
                      .arg(s.name)
                      .arg(busIndex + 1)
                : QStringLiteral("'%1' forwarded only to CAN %2, which is the bus you loaded it "
                                 "onto, so it now has no target. Open it and choose one.")
                      .arg(s.name)
                      .arg(busIndex + 1));
    }

    // Create the channels, now that every name is settled.
    for (const Plan &plan : plans)
        if (plan.create)
            catalog.addOrUpdateUserChannel(plan.channel);

    // WHAT THE TEMPLATE STILL ASKS FOR AND THIS DOCUMENT DOES NOT HAVE. Two
    // shapes, and they need different sentences: a Triggered message's
    // condition, which a template cannot carry and the user must build; and any
    // other reference the source document had already lost, which arrives
    // dangling because inventing a definition for it would have repaired the
    // symptom in the copy and not the original.
    QSet<QString> reported;
    for (const CommsSection &s : result.sections) {
        for (const QString &ref : mentionedChannels(s)) {
            if (ChannelCatalog::isDeviceChannel(ref) || catalog.findByName(ref).isValid())
                continue;
            const QString key = ref.toLower();
            if (reported.contains(key))
                continue;
            reported.insert(key);
            result.notes.append(
                isConditionReference(s, ref)
                    ? QStringLiteral("'%1' transmits only while the User Condition '%2' holds, "
                                     "and this configuration has no channel by that name. It "
                                     "will not transmit until you create the condition.")
                          .arg(s.name, ref)
                    : QStringLiteral("'%1' refers to a channel named '%2', which this "
                                     "configuration does not have.")
                          .arg(s.name, ref));
        }
    }

    if (out)
        *out = result;
    return true;
}

} // namespace ct
