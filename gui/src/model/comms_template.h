// Communications templates — the .ct3t container behind Save… / Load… in
// Communications Setup.
//
// A template is a group of messages lifted out of one configuration so it can
// be dropped into another: the supplier's ECU, the dash you fit to every car,
// the lambda controller that always speaks the same protocol. It carries what
// the section editor's two tabs hold — every Parameters field and every
// Received/Transmitted Channels row — plus the definitions of the channels
// those rows name, because a row without its channel decodes into nothing.
//
// ---------------------------------------------------------------------------
// WHY IT IS BINARY, AND WHAT THAT IS WORTH
//
// A .ct3 is indented JSON, so a CAN ID, a start bit and a scaling factor are
// all legible in Notepad. A template is the file you HAND TO SOMEONE — that is
// the whole reason it exists — so the same legibility that is convenient in
// your own configuration is the thing you are trying not to give away. The
// bytes therefore go through the .ct3s container (secure_file.h): encrypted,
// authenticated, and scattered through CSPRNG noise, with no ASCII anywhere in
// the file.
//
// Be honest about the ceiling, in the same words secure_file.h uses about
// itself: this is a STANDARD-mode container, so the key that decrypts it
// travels inside it. That defeats a hex editor, `strings`, a text search and
// any tool that does not implement this format — which is the actual threat,
// because the actual threat is a customer reading a protocol out of a file they
// were given. It does NOT defeat someone who reads this source. Nothing here
// should be described to a user as though it did.
//
// What DOES survive being handed on is the per-message marking: a Hidden or
// Protect Communication section keeps its tier and its messageKey through a
// template exactly as it does through a .ct3, so loading one into a fresh
// document produces a padlocked row, not an open one.
//
// ---------------------------------------------------------------------------
// WHY IT IS NOT A .ct3s
//
// Same container, different contents, and the two must not be confusable. The
// body carries kCommsTemplateFileType, which is what Load… checks before it
// believes a file is a template — and what Configuration::loadFromFile checks
// to refuse one that arrives at File > Open. Without that marker a template fed
// to Open would decrypt perfectly and then parse as a configuration with every
// key missing, which is to say it would quietly REPLACE the open document with
// an empty one. A wrong file must produce a sentence, never a blank document.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "channel.h"
#include "comms_types.h"

namespace ct {

class ChannelCatalog;

// The body marker described above. A constant rather than a literal because
// two places compare against it — the reader here and the refusal in
// Configuration::loadFromFile — and a typo in one of them would be a file that
// opens as the wrong kind of thing.
inline constexpr char kCommsTemplateFileType[] = "CANTripleCommsTemplate";

// The LAYOUT of the body, which is not the .ct3 schema and not the container's
// formatVersion. Three numbers because there are three things that can change
// independently: how the bytes are wrapped (secure_file), how a section is
// spelled in JSON (the .ct3 schema, carried in `configSchema`), and which keys
// this envelope puts around them.
inline constexpr int kCommsTemplateFormatVersion = 1;

// "ct3t", and the dialog filters built from it. Not ".ct3s": a template is not
// a configuration and must not be offered where one is expected.
QString commsTemplateExtension();
QString commsTemplateFilter();

// Where templates live, and why it is under Documents rather than inside the
// installed program, are both in user_paths.h — ct::commsTemplatesDirectory().
// This header used to answer that itself, which made it the fourth file with an
// opinion about the folder layout.

// One template, in memory.
struct CommsTemplate
{
    QString name;         // what the file calls itself; the base name at save time
    QString writtenBy;    // CT_APP_VERSION of the build that wrote it, for people
    int formatVersion = kCommsTemplateFormatVersion;
    // The .ct3 schema the sections were spelled at, handed to
    // CommsSection::fromJson so a template written today still reads correctly
    // when the schema has moved on. Not optional and not guessable — see the
    // note on CommsSection::fromJson about what applying the wrong migration
    // does to a protection tier.
    int configSchema = 0;

    // The bus the sections came off. Carried because a template for a device
    // that speaks at 500k is not much use if it arrives on a bus running at 1M
    // and says nothing about it; applied only if the user agrees, since the bus
    // belongs to their configuration and not to this file.
    bool hasBusSettings = false;
    int rateKbps = 0;
    int dataRateKbps = 0;
    bool termination = false;

    QList<CommsSection> sections;
    // Definitions for the user channels the sections name — units, data type,
    // base resolution, decimals, range. WITHOUT these a loaded message names
    // channels that do not exist, and a receive row whose channel is missing
    // decodes to nowhere. Device channels are NOT here: they exist in every
    // document by construction, so the template refers to them by name.
    QList<Channel> channels;

    bool isEmpty() const { return sections.isEmpty(); }
};

// Collect `sections` and everything they depend on out of `catalog`. `bus`
// contributes the rate/FD/termination record; pass the values the user is
// looking at, not the document's, or a template saved after a rate change
// records the rate that was replaced.
CommsTemplate buildCommsTemplate(const ChannelCatalog &catalog, const BusConfig &bus,
                                 const QList<CommsSection> &sections, const QString &name);

bool writeCommsTemplate(const QString &path, const CommsTemplate &tmpl, QString *error = nullptr);
// Reports "this is a secure configuration, not a template" apart from "this
// file is damaged": both are wrong-file mistakes, and only one of them is
// worth telling the user how to fix.
bool readCommsTemplate(const QString &path, CommsTemplate *out, QString *error = nullptr);

// What loading a template did to the document it landed in.
struct CommsTemplateMerge
{
    // Ready to append to the target bus, with every channel reference already
    // repointed at the name the channel actually ended up with.
    QList<CommsSection> sections;
    int channelsCreated = 0;
    int channelsReused = 0;
    // Renames, stripped self-routes, references nothing in this document
    // answers to. One line each, shown in the summary's details pane — a
    // thirty-message template can produce a lot of them, and a merge that
    // silently changed a channel's name is the one that costs an afternoon.
    QStringList notes;
};

// Resolve `tmpl` against `catalog`, CREATING the channels it needs, and produce
// the sections to append to bus `busIndex`.
//
// The channel rules, in the order they are asked:
//   - a name that belongs to a DEVICE channel cannot be taken by a user channel,
//     so the template's is renamed;
//   - a name already held by an EQUIVALENT user channel is reused, so loading a
//     template twice does not litter the catalogue with copies;
//   - a name held by a DIFFERENT user channel is renamed, never overwritten:
//     silently re-scaling an existing channel would change what every other
//     message using it decodes to, with nothing on screen to say why;
//   - anything else is created.
//
// This MUTATES the catalogue. That is deliberate and matches DBC import, which
// has always added its channels at import time — the sections it produces are
// held in the dialog's working copies and may still be cancelled, and a
// cancelled import leaves its channels behind. Being consistent about that is
// worth more than being clever in one of the two places.
bool mergeCommsTemplate(ChannelCatalog &catalog, int busIndex, const CommsTemplate &tmpl,
                        CommsTemplateMerge *out, QString *error = nullptr);

} // namespace ct
