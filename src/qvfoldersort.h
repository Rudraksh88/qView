#ifndef QVFOLDERSORT_H
#define QVFOLDERSORT_H

#include <QString>

// Describes the order of the files in a folder. Comes either from qView's own
// sort settings or from the file manager's per-folder view settings (Dolphin).
struct QVFolderSort
{
    enum class Role
    {
        Name,
        Modified,
        Created,
        Size,
        Type,
        Random
    };

    enum class NameCompare
    {
        Natural,          // numeric-aware, case-insensitive, base name before extension
        CaseInsensitive,
        CaseSensitive
    };

    Role role = Role::Name;
    // Qt::DescendingOrder semantics: the reverse of the ascending order
    // (Z to A, newest first, largest first)
    bool descending = false;
    NameCompare nameCompare = NameCompare::Natural;

    // Reads the sort settings the file manager (Dolphin) uses to show dirPath.
    // Returns false when Dolphin is not in use, leaving result untouched.
    static bool fromFileManager(const QString &dirPath, QVFolderSort &result);
};

#endif // QVFOLDERSORT_H
