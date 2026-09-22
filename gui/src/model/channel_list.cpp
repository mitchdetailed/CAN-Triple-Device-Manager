#include "channel_list.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace ct {

namespace {
const QLatin1String kKeyFileType("fileType");
const QLatin1String kKeyFormatVersion("formatVersion");
const QLatin1String kKeyChannels("channels");
} // namespace

QString channelListExtension()
{
    return QStringLiteral("ct3l");
}

QString channelListFilter()
{
    return QStringLiteral("CAN Triple Channel Lists (*.ct3l);;All Files (*)");
}

bool writeChannelList(const QString &path, const QStringList &names, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    QJsonObject body;
    body[kKeyFileType] = QLatin1String(kChannelListFileType);
    body[kKeyFormatVersion] = kChannelListFormatVersion;
    body[kKeyChannels] = QJsonArray::fromStringList(names);
    // QSaveFile: the old list survives a failure part-way through, rather
    // than being left half-written.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("Could not write \"%1\":\n%2").arg(path, file.errorString()));
    // Indented, because the whole point of a plain file is that it can be read.
    file.write(QJsonDocument(body).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return fail(QStringLiteral("Could not write \"%1\":\n%2").arg(path, file.errorString()));
    return true;
}

bool readChannelList(const QString &path, QStringList *names, QString *error)
{
    names->clear();
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not open \"%1\":\n%2").arg(path, file.errorString()));
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    const QString name = QFileInfo(path).fileName();
    if (!doc.isObject())
        return fail(QStringLiteral("\"%1\" is not a channel list: %2")
                        .arg(name, parse.error == QJsonParseError::NoError
                                       ? QStringLiteral("the file does not hold a JSON object")
                                       : parse.errorString()));
    const QJsonObject body = doc.object();
    if (body.value(kKeyFileType).toString() != QLatin1String(kChannelListFileType))
        return fail(QStringLiteral("\"%1\" is not a channel list.").arg(name));
    if (body.value(kKeyFormatVersion).toInt(0) > kChannelListFormatVersion)
        return fail(QStringLiteral("\"%1\" was saved by a newer version of CAN Triple Device "
                                   "Manager and cannot be read by this one.")
                        .arg(name));
    const QJsonArray channels = body.value(kKeyChannels).toArray();
    for (const QJsonValue &value : channels) {
        const QString channel = value.toString().trimmed();
        if (!channel.isEmpty() && !names->contains(channel))
            names->append(channel);
    }
    return true;
}

} // namespace ct
