#include "qvfoldersort.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#ifdef Q_OS_LINUX
#include <sys/xattr.h>
#endif

namespace {

// Minimal reader for KDE's INI-style config files (KConfig format)
class KdeConfig
{
public:
    KdeConfig() = default;

    explicit KdeConfig(const QByteArray &data)
    {
        QString group;
        const QStringList lines = QString::fromUtf8(data).split('\n');
        for (QString line : lines)
        {
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith('#'))
                continue;

            if (line.startsWith('[') && line.endsWith(']'))
            {
                group = line.mid(1, line.length() - 2);
                groups.insert(group);
                continue;
            }

            const int separator = line.indexOf('=');
            if (separator <= 0)
                continue;

            QString key = line.left(separator).trimmed();
            // Strip KConfig key markers like "[$e]"
            const int marker = key.indexOf('[');
            if (marker > 0)
                key.truncate(marker);

            entries.insert(group + '/' + key, line.mid(separator + 1).trimmed());
        }
    }

    static KdeConfig fromFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return KdeConfig();
        return KdeConfig(file.readAll());
    }

    bool hasGroup(const QString &group) const
    {
        return groups.contains(group);
    }

    QString value(const QString &group, const QString &key, const QString &defaultValue = QString()) const
    {
        return entries.value(group + '/' + key, defaultValue);
    }

    bool boolValue(const QString &group, const QString &key, bool defaultValue) const
    {
        const QString stringValue = value(group, key);
        if (stringValue.isEmpty())
            return defaultValue;
        return stringValue.compare("true", Qt::CaseInsensitive) == 0 || stringValue == "1";
    }

    // KConfig stores date-times as "year,month,day,hour,minute,second[.msec]"
    QDateTime dateTime(const QString &group, const QString &key) const
    {
        const QStringList parts = value(group, key).split(',');
        if (parts.size() < 6)
            return QDateTime();

        const QDate date(parts[0].toInt(), parts[1].toInt(), parts[2].toInt());
        const double seconds = parts[5].toDouble();
        const int wholeSeconds = static_cast<int>(seconds);
        const QTime time(parts[3].toInt(), parts[4].toInt(), wholeSeconds, qRound((seconds - wholeSeconds) * 1000));
        return QDateTime(date, time);
    }

private:
    QSet<QString> groups;
    QHash<QString, QString> entries;
};

QByteArray readExtendedAttribute(const QString &path, const QByteArray &name)
{
#ifdef Q_OS_LINUX
    const QByteArray nativePath = QFile::encodeName(path);
    const ssize_t size = getxattr(nativePath.constData(), name.constData(), nullptr, 0);
    if (size <= 0)
        return QByteArray();

    QByteArray value(static_cast<int>(size), Qt::Uninitialized);
    const ssize_t bytesRead = getxattr(nativePath.constData(), name.constData(), value.data(), value.size());
    if (bytesRead <= 0)
        return QByteArray();

    value.resize(static_cast<int>(bytesRead));
    return value;
#else
    Q_UNUSED(path)
    Q_UNUSED(name)
    return QByteArray();
#endif
}

// Dolphin keeps a folder's view properties in a ".directory" file inside folderPath
// or, since newer versions, in the extended attribute "user.kde.fm.viewproperties"
// of the folder (split into "#1", "#2", ... chunks by KFileMetaData).
bool loadDolphinProperties(const QString &folderPath, KdeConfig &result)
{
    const KdeConfig fileConfig = KdeConfig::fromFile(folderPath + "/.directory");
    if (fileConfig.hasGroup("Dolphin") || fileConfig.hasGroup("Settings"))
    {
        result = fileConfig;
        return true;
    }

    const QByteArray attributeName = "user.kde.fm.viewproperties";
    QByteArray data = readExtendedAttribute(folderPath, attributeName);
    if (data.isEmpty())
    {
        for (int chunk = 1; ; chunk++)
        {
            const QByteArray part = readExtendedAttribute(folderPath, attributeName + '#' + QByteArray::number(chunk));
            if (part.isEmpty())
                break;
            data += part;
        }
    }
    if (data.isEmpty())
        return false;

    result = KdeConfig(data);
    return true;
}

// Name of the directory Dolphin uses to store the properties of folders it
// cannot or should not write into
QString dolphinDirectoryHash(const QString &canonicalDir)
{
    const QByteArray hash = QCryptographicHash::hash(QUrl::fromLocalFile(canonicalDir).toEncoded(), QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toBase64()).replace('/', '-');
}

} // namespace

bool QVFolderSort::fromFileManager(const QString &dirPath, QVFolderSort &result)
{
    const QString dolphinrcPath = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, "dolphinrc");
    if (dolphinrcPath.isEmpty())
        return false;

    const KdeConfig dolphinrc = KdeConfig::fromFile(dolphinrcPath);
    const bool useGlobalProps = dolphinrc.boolValue("General", "GlobalViewProps", true);
    // Folder settings saved before "apply to all folders" was last used are ignored
    const QDateTime propsValidSince = dolphinrc.dateTime("General", "ViewPropsTimestamp");
    const QString viewPropsDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/dolphin/view_properties/";

    QString canonicalDir = QFileInfo(dirPath).canonicalFilePath();
    if (canonicalDir.isEmpty())
        canonicalDir = QDir::cleanPath(dirPath);

    const auto isOutdated = [&propsValidSince](const KdeConfig &props) {
        return propsValidSince.isValid() && props.dateTime("Dolphin", "Timestamp") < propsValidSince;
    };

    KdeConfig props;
    bool usingDefaults = true;
    if (!useGlobalProps)
    {
        // Dolphin stores the settings inside the folder itself when it is a writable
        // folder in the home directory, otherwise in its own data directory under a
        // hash of the folder's URL (older versions: under the folder's path)
        QStringList locations;
        if (canonicalDir.startsWith(QDir::homePath()) && QFileInfo(canonicalDir).isWritable())
            locations << canonicalDir;
        locations << viewPropsDir + "local/" + dolphinDirectoryHash(canonicalDir)
                  << viewPropsDir + "local" + canonicalDir;

        for (const QString &location : locations)
        {
            if (loadDolphinProperties(location, props))
            {
                usingDefaults = isOutdated(props);
                break;
            }
        }
    }

    if (usingDefaults)
    {
        // The global view properties act as defaults for folders without their own
        const bool hasGlobalProps = loadDolphinProperties(viewPropsDir + "global", props);
        if (useGlobalProps)
            usingDefaults = !hasGlobalProps || isOutdated(props);
    }

    QString role = props.value("Dolphin", "SortRole", "text");
    bool descending = props.value("Dolphin", "SortOrder", "0") == "1";

    // Dolphin shows the Downloads folder newest first unless told otherwise
    const QString downloadsDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (usingDefaults && (dirPath == downloadsDir || canonicalDir == downloadsDir))
    {
        role = "modificationtime";
        descending = true;
    }

    result = QVFolderSort();
    if (role == "modificationtime" || role == "date")
        result.role = Role::Modified;
    else if (role == "creationtime")
        result.role = Role::Created;
    else if (role == "size")
        result.role = Role::Size;
    else if (role == "type")
        result.role = Role::Type;
    else // "text", "name" and roles qView cannot sort by
        result.role = Role::Name;
    result.descending = descending;

    const QString sortingChoice = dolphinrc.value("General", "SortingChoice", "NaturalSorting");
    if (sortingChoice == "CaseSensitiveSorting")
        result.nameCompare = NameCompare::CaseSensitive;
    else if (sortingChoice == "CaseInsensitiveSorting")
        result.nameCompare = NameCompare::CaseInsensitive;
    else
        result.nameCompare = NameCompare::Natural;

    return true;
}
