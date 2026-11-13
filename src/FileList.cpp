#include "FileList.h"

#include "DirectoryWatcher.h"
#include "Helpers.h"

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cassert>
#include <limits>
#include <random>

///////////////////////////////////////////////////////////////////////////////////
// Static initialization
///////////////////////////////////////////////////////////////////////////////////

Helpers::ESorting CFileDesc::sm_eSorting = Helpers::FS_LastModTime;
bool CFileDesc::sm_bSortAscending = true;
Helpers::ENavigationMode CFileList::sm_eMode = Helpers::NM_LoopDirectory;

namespace {

// C++20 Zufallsgenerator für Random-Sortierung
static std::mt19937& GetRng() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    return gen;
}

static int GenerateRandomNumber() {
    std::uniform_int_distribution<int> dist(0, std::numeric_limits<int>::max());
    return dist(GetRng());
}

// Globaler QCollator für natürliche Zahlensortierung (file9.txt vor file10.txt)
static const QCollator& GetLogicalCollator() {
    static const QCollator collator = []() {
        QCollator c;
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }();
    return collator;
}

// Order of folder names for the folder navigation modes: the same name order as the files (natural,
// case-insensitive, the exact name as tie-breaker: "Foto" and "foto" can both exist on Linux),
// reversed when sorting descending.
// Folders are always ordered by name, also when the files are sorted by date, size or randomly.
static bool FolderNameAscending(const QString& sName, const QString& sOther) {
    const int nByName = GetLogicalCollator().compare(sName, sOther);
    if (nByName != 0) {
        return nByName < 0;
    }
    return sName < sOther;
}

static bool FolderNameLess(const QString& sName, const QString& sOther) {
    // Descending: "other before this", see CFileDesc::operator < for why not "not this before other"
    return CFileDesc::IsSortedAscending() ? FolderNameAscending(sName, sOther) : FolderNameAscending(sOther, sName);
}

// Folder paths are absolute and clean, without '/' at the end except for the root folder "/"
static QString ChildPath(const QString& sDirectory, const QString& sName) {
    return sDirectory.endsWith(QLatin1Char('/')) ? sDirectory + sName : sDirectory + QLatin1Char('/') + sName;
}

// Gets the names of the subfolders of the given folder, in folder order. Hidden folders are skipped.
// bSkipSymLinks skips symbolic links to folders, they can form cycles.
static QStringList GetSubFolders(const QString& sDirectory, bool bSkipSymLinks) {
    QDir::Filters filters = QDir::Dirs | QDir::NoDotAndDotDot;
    if (bSkipSymLinks) {
        filters |= QDir::NoSymLinks;
    }
    QStringList dirList = QDir(sDirectory).entryList(filters);
    std::sort(dirList.begin(), dirList.end(), FolderNameLess);
    return dirList;
}

// Unterstützte Dateiendungen
static const QStringList& GetSupportedFileEndingList() {
    static const QStringList endings = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jfif"), QStringLiteral("bmp"),
        QStringLiteral("png"), QStringLiteral("apng"), QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("gif"),
        QStringLiteral("webp"), QStringLiteral("jxl"),  QStringLiteral("avif"), QStringLiteral("heif"), 
        QStringLiteral("heic"), QStringLiteral("tga"),  QStringLiteral("qoi"),  QStringLiteral("psd"), 
        QStringLiteral("psb"), QStringLiteral("ico"),  QStringLiteral("cbz"),  QStringLiteral("cb7")
    };
    return endings;
}

} // namespace

///////////////////////////////////////////////////////////////////////////////////
// CFileDesc Implementation
///////////////////////////////////////////////////////////////////////////////////

CFileDesc::CFileDesc(const QString& sName, const QDateTime& lastModTime, const QDateTime& creationTime, qint64 fileSize)
    : m_sName(QDir::cleanPath(sName))
    , m_lastModTime(lastModTime)
    , m_creationTime(creationTime)
    , m_nRandomOrderNumber(GenerateRandomNumber())
    , m_fileSize(fileSize)
{
    m_sTitle = QFileInfo(m_sName).fileName();
}

void CFileDesc::SetName(const QString& sName) {
    // same normalization as in the constructor
    m_sName = QDir::cleanPath(sName);
    m_sTitle = QFileInfo(m_sName).fileName();
}

bool CFileDesc::SortAscending(const CFileDesc& other) const {
    // The sort key; if it is equal (same date or size is common, e.g. the pages of an extracted archive), the name
    // decides, and if that is equal too (natural, case-insensitive order), the full path: the order is always unique.
    if (sm_eSorting == Helpers::FS_LastModTime) {
        if (m_lastModTime != other.m_lastModTime) {
            return m_lastModTime < other.m_lastModTime;
        }
    }
    else if (sm_eSorting == Helpers::FS_Random) {
        if (m_nRandomOrderNumber != other.m_nRandomOrderNumber) {
            return m_nRandomOrderNumber < other.m_nRandomOrderNumber;
        }
    }
    else if (sm_eSorting == Helpers::FS_FileSize) {
        if (m_fileSize != other.m_fileSize) {
            return m_fileSize < other.m_fileSize;
        }
    }
    // Helpers::FS_FileName, and the tie-breaker of the other sortings
    const int nByName = GetLogicalCollator().compare(m_sTitle, other.m_sTitle);
    if (nByName != 0) {
        return nByName < 0;
    }
    return m_sName < other.m_sName;
}

bool CFileDesc::operator < (const CFileDesc& other) const {
    // Descending: "other before this". Not "not this before other", that is true in both directions for equal
    // entries and breaks the rules std::list::sort() relies on.
    return sm_bSortAscending ? SortAscending(other) : other.SortAscending(*this);
}

///////////////////////////////////////////////////////////////////////////////////
// CFileList Implementation
///////////////////////////////////////////////////////////////////////////////////

CFileList::CFileList(const QString& sInitialFile, CDirectoryWatcher& directoryWatcher, 
    Helpers::ESorting eInitialSorting, bool isSortedAscending, bool bWrapAroundFolder, int nLevel)
    : m_directoryWatcher(directoryWatcher)
{
    CFileDesc::SetSorting(eInitialSorting, isSortedAscending);
    m_bDeleteHistory = true;
    m_bWrapAroundFolder = bWrapAroundFolder;
    m_sInitialFile = QDir::cleanPath(sInitialFile);
    m_nLevel = nLevel;

    QFileInfo initialInfo(m_sInitialFile);
    const bool bIsDirectory = initialInfo.isDir();
    m_sDirectory = QDir::cleanPath(bIsDirectory ? initialInfo.absoluteFilePath() : initialInfo.absolutePath());

    const QString sExtension = initialInfo.suffix().toLower();
    const bool bImageFile = !bIsDirectory && IsImageFile(sExtension);

    m_directoryWatcher.SetCurrentDirectory(m_sDirectory);

    if (bImageFile || bIsDirectory) {
        FindFiles();
        m_iter = FindFile(m_sInitialFile);
        m_iterStart = UsesStartFileAsLoopStart() ? m_iter : m_fileList.begin();
    } else {
        if (initialInfo.exists()) {
            m_fileList.emplace_back(m_sInitialFile, initialInfo.lastModified(), initialInfo.birthTime(), initialInfo.size());
        }
        m_iter = m_iterStart = m_fileList.begin();
    }
}

CFileList::~CFileList() {
    if (m_bDeleteHistory) {
        DeleteHistory();
    }
    m_fileList.clear();
}

void CFileList::Reload(const QString& sFileName, bool clearForwardHistory) {
    QString sCurrentFile = sFileName.isEmpty() ? Current() : sFileName;
    if (sCurrentFile.isEmpty()) {
        m_fileList.clear();
        return;
    }

    if (clearForwardHistory) {
        CFileList* pFileList = this->m_next;
        while (pFileList != nullptr && pFileList->m_prev != nullptr) {
            CFileList* thisList = pFileList;
            pFileList = pFileList->m_next;
            thisList->m_bDeleteHistory = false;
            delete thisList;
        }
        m_next = nullptr;
        m_bNextIsAdjacent = false;
    }

    FindFiles();
    m_iterStart = UsesStartFileAsLoopStart() ? FindFile(m_sInitialFile) : m_fileList.begin();
    m_iter = FindFile(sCurrentFile);
}

void CFileList::FileHasRenamed(const QString& sOldFileName, const QString& sNewFileName) {
    // Entries are stored as clean absolute paths (FindFiles() uses absoluteFilePath(), CFileDesc cleans),
    // so normalize the parameters the same way.
    const QString sOld = QDir::cleanPath(QFileInfo(sOldFileName).absoluteFilePath());
    const QString sNew = QDir::cleanPath(QFileInfo(sNewFileName).absoluteFilePath());
    if (sOld.isEmpty() || sNew.isEmpty() || sOld == sNew) {
        return;
    }

    // The file the list was created for (used e.g. by Reload() as the start of the wrap-around).
    // File names are case-sensitive: renaming "a.jpg" must not hit an unrelated "A.jpg".
    if (!m_sInitialFile.isEmpty() && QDir::cleanPath(QFileInfo(m_sInitialFile).absoluteFilePath()) == sOld) {
        m_sInitialFile = sNew;
    }

    auto iterRenamed = m_fileList.end();
    for (auto iter = m_fileList.begin(); iter != m_fileList.end(); ++iter) {
        const QString& sName = iter->GetName();
        // Names are normally absolute already; the QFileInfo detour is only for the rare relative entry
        // (e.g. a single non-image file given on the command line).
        if (sName == sOld || (!QDir::isAbsolutePath(sName) && QDir::cleanPath(QFileInfo(sName).absoluteFilePath()) == sOld)) {
            iter->SetName(sNew);
            iterRenamed = iter;
            break;   // names are unique within one list
        }
    }
    if (iterRenamed == m_fileList.end()) {
        return;
    }

    // A rename can change the position of the entry with every sorting: by name directly, by date, size or
    // random number when the key is equal to that of other entries (the name decides then, see operator<).
    //
    // The entry is moved alone instead of calling m_fileList.sort(): the rest of the list is still sorted,
    // so one linear pass is enough. std::list::splice() keeps all iterators valid (m_iter, m_iterStart,
    // m_iterCheckPoint), m_iter still points to the renamed file.
    auto iterPos = m_fileList.begin();
    while (iterPos != m_fileList.end() && (iterPos == iterRenamed || *iterPos < *iterRenamed)) {
        ++iterPos;
    }
    m_fileList.splice(iterPos, m_fileList, iterRenamed);

    // Unless the loop starts at the start file, the iteration starts at the first element, which may have
    // changed. (Otherwise m_iterStart is a position, i.e. an iterator, which stays valid.)
    if (!UsesStartFileAsLoopStart()) {
        m_iterStart = m_fileList.begin();
    }
}

bool CFileList::CurrentFileExists() const {
    const QString cur = Current();
    return !cur.isEmpty() && QFileInfo::exists(cur);
}

CFileList* CFileList::Next() {
    if (sm_eMode == Helpers::NM_LoopSameDirectoryLevel) {
        return NextInFolderList();
    }
    if (!m_fileList.empty()) {
        auto iterTemp = m_iter;
        if (iterTemp == m_fileList.end()) return this;

        iterTemp++;
        if (iterTemp == m_fileList.end()) {
            iterTemp = m_fileList.begin();
        }
        if (iterTemp == m_iterStart) {
            if (!WrapsAround() && sm_eMode == Helpers::NM_LoopDirectory) {
                return this;
            }
            CFileList* pNextList = WrapToNextImage();
            if (pNextList == nullptr) {
                pNextList = this;
                while (pNextList->m_prev != nullptr) pNextList = pNextList->m_prev;
                if (pNextList != this) {
                    return WrapsAround() ? GotoFirstShown() : this;
                }
            }
            if (pNextList != this) {
                return pNextList;
            }
        }
        NextInFolder();
    } else {
        CFileList* pNextList = WrapToNextImage();
        if (pNextList != nullptr) {
            return pNextList;
        }
    }
    return this;
}

CFileList* CFileList::Prev() {
    if (sm_eMode == Helpers::NM_LoopSameDirectoryLevel) {
        return PrevInFolderList();
    }
    if (m_iter == m_iterStart) {
        if (sm_eMode == Helpers::NM_LoopDirectory) {
            if (!WrapsAround()) {
                return this;
            }
            if (m_iter == m_fileList.begin()) {
                MoveIterToLast();
            } else {
                m_iter--;
            }
            return this;
        } else {
            return WrapToPrevImage();
        }
    }
    if (!m_fileList.empty()) {
        if (m_iter == m_fileList.begin()) {
            m_iter = m_fileList.end();
            m_iter--;
        } else {
            m_iter--;
        }
    }
    return this;
}

CFileList* CFileList::First() {
    CFileList* pFirst = this;
    if (sm_eMode == Helpers::NM_LoopSameDirectoryLevel) {
        pFirst = OuterFolder(true);
        if (pFirst == nullptr) {
            pFirst = this; // no images at all
        }
    }
    pFirst->m_iter = pFirst->m_iterStart = pFirst->m_fileList.begin();
    return pFirst;
}

CFileList* CFileList::Last() {
    CFileList* pLast = this;
    if (sm_eMode == Helpers::NM_LoopSameDirectoryLevel) {
        pLast = OuterFolder(false);
        if (pLast == nullptr) {
            pLast = this; // no images at all
        }
    }
    pLast->MoveIterToLast();
    pLast->m_iterStart = pLast->m_fileList.begin();
    return pLast;
}

bool CFileList::WrapsAround() const {
    // LoopSubFolders does not wrap around yet: at the end of the loop navigation stops, as before
    return m_bWrapAroundFolder && sm_eMode != Helpers::NM_LoopSubDirectories;
}

CFileList* CFileList::AwayFromCurrent() {
    const QString sCurrentFile = Current();
    const QString sNextFile = PeekNextPrev(1, true);

    if (sCurrentFile.isEmpty() || sNextFile.isEmpty() || sCurrentFile == sNextFile) {
        CFileList* pFileList = Prev();
        if (!Current().isEmpty() && !sCurrentFile.isEmpty() && sCurrentFile == Current()) {
            m_iter = m_fileList.end();
        }
        return pFileList;
    } else {
        return Next();
    }
}

QString CFileList::Current() const {
    if (m_iter != m_fileList.end()) {
        return m_iter->GetName();
    }
    return {};
}

QString CFileList::CurrentFileTitle() const {
    if (m_iter != m_fileList.end()) {
        return m_iter->GetTitle();
    }
    return {};
}

QString CFileList::CurrentDirectory() const {
    return m_sDirectory;
}

QString CFileList::PeekNextPrev(int nIndex, bool bForward) {
    auto thisIter = m_iter;
    QString sFileName;
    if (nIndex != 0) {
        CFileList* pFL = bForward ? Next() : Prev();
        sFileName = pFL->PeekNextPrev(nIndex - 1, bForward);
    } else {
        sFileName = Current();
    }
    m_iter = thisIter;
    return sFileName;
}

void CFileList::SetSorting(Helpers::ESorting eSorting, bool sortAscending) {
    if (eSorting != CFileDesc::GetSorting() || sortAscending != CFileDesc::IsSortedAscending()) {
        const QString sThisFile = (m_iter != m_fileList.end()) ? m_iter->GetName() : QString();
        CFileDesc::SetSorting(eSorting, sortAscending);
        m_fileList.sort();
        m_iter = FindFile(sThisFile);
        m_iterStart = UsesStartFileAsLoopStart() ? m_iter : m_fileList.begin();
        if (sm_eMode != Helpers::NM_LoopDirectory) {
            // the other loaded folders are neither resorted nor in the new folder order, reload them when needed
            DeleteHistory();
        }
    }
}

Helpers::ESorting CFileList::GetSorting() const {
    return CFileDesc::GetSorting();
}

bool CFileList::IsSortedAscending() const {
    return CFileDesc::IsSortedAscending();
}

void CFileList::SetNavigationMode(Helpers::ENavigationMode eMode) {
    sm_eMode = eMode;
    DeleteHistory();
    m_nLevel = 0;
    m_iterStart = UsesStartFileAsLoopStart() ? m_iter : m_fileList.begin();
}

bool CFileList::CanOpenCurrentFileForReading() const {
    const QString sCurrentFile = Current();
    if (sCurrentFile.isEmpty()) {
        return false;
    }
    QFile file(sCurrentFile);
    return file.open(QIODevice::ReadOnly);
}

void CFileList::DeleteHistory(bool onlyForward) {
    CFileList* pFileList = this->m_next;
    while (pFileList != nullptr && pFileList->m_prev != nullptr) {
        CFileList* thisList = pFileList;
        pFileList = pFileList->m_next;
        thisList->m_bDeleteHistory = false;
        delete thisList;
    }
    if (!onlyForward) {
        pFileList = this->m_prev;
        while (pFileList != nullptr) {
            CFileList* thisList = pFileList;
            pFileList = pFileList->m_prev;
            thisList->m_bDeleteHistory = false;
            delete thisList;
        }
        m_prev = nullptr;
        m_bPrevIsAdjacent = false;
    }
    m_next = nullptr;
    m_bNextIsAdjacent = false;
}

///////////////////////////////////////////////////////////////////////////////////
// Private Methods
///////////////////////////////////////////////////////////////////////////////////

void CFileList::MoveIterToLast() {
    // also correct when m_iter is end(), e.g. after AwayFromCurrent() found no other image
    m_iter = m_fileList.end();
    if (!m_fileList.empty()) {
        m_iter--;
    }
}

std::list<CFileDesc>::iterator CFileList::FindFile(const QString& sName) {
    const QString searchTitle = QFileInfo(sName).fileName();
    if (searchTitle.isEmpty()) {
        return m_fileList.begin();
    }

    // First exactly this file: entries are clean absolute paths. File names on Linux are case-sensitive
    // (IMG.jpg and img.jpg are different files), and a slide show list can contain files of the same name
    // from different folders.
    const QString sPath = QDir::cleanPath(QFileInfo(sName).absoluteFilePath());
    for (auto iter = m_fileList.begin(); iter != m_fileList.end(); ++iter) {
        if (iter->GetName() == sPath) {
            return iter;
        }
    }

    // Not found as given (e.g. on a case-insensitive file system with the name in different case):
    // the first entry with this file name, ignoring case
    for (auto iter = m_fileList.begin(); iter != m_fileList.end(); ++iter) {
        if (iter->GetTitle().compare(searchTitle, Qt::CaseInsensitive) == 0) {
            return iter;
        }
    }
    return m_fileList.begin();
}

CFileList* CFileList::WrapToNextImage() {
    if (m_next != nullptr) {
        assert(sm_eMode != Helpers::NM_LoopDirectory);
        return m_next;
    }

    // LoopSameFolderLevel does not come here, see NextInFolderList()
    if (sm_eMode == Helpers::NM_LoopSubDirectories) {
        return FindFileRecursively(m_sDirectory, QString(), false, m_nLevel, 0);
    }
    return nullptr;
}

CFileList* CFileList::FindFileRecursively(const QString& sDirectory, const QString& sFindAfter,
                                           bool bSearchThisFolder, int nLevel, int nRecursion) {
    if (bSearchThisFolder) {
        CFileList* pFileList = TryCreateFileList(sDirectory, nLevel);
        if (pFileList != nullptr) {
            return pFileList;
        }
    }

    QDir dir(sDirectory);
    QStringList dirList = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    if (!dirList.isEmpty()) {
        std::sort(dirList.begin(), dirList.end(), FolderNameLess);

        bool bStart = sFindAfter.isEmpty();
        for (const QString& subDirName : dirList) {
            if (bStart) {
                const QString subDirPath = sDirectory + QDir::separator() + subDirName;
                CFileList* pFileList = FindFileRecursively(subDirPath, QString(), true, nLevel + 1, nRecursion + 1);
                if (pFileList != nullptr) {
                    return pFileList;
                }
            }
            if (subDirName == sFindAfter) {
                bStart = true;
            }
        }
    }

    if (nLevel == 0 || nRecursion > 0) {
        return nullptr;
    }

    // Backtrack to parent
    const QDir parentDir(sDirectory + QStringLiteral("/.."));
    const QString sParentDir = parentDir.canonicalPath();
    const QString sThisDir = QFileInfo(sDirectory).fileName();

    return FindFileRecursively(sParentDir, sThisDir, false, nLevel - 1, std::max(0, nRecursion - 1));
}

CFileList* CFileList::WrapToPrevImage() {
    if (m_prev != nullptr) {
        return m_prev;
    }
    return this;
}

bool CFileList::UsesStartFileAsLoopStart() const {
    // Only LoopFolder with wrap around starts the loop at the start file. In LoopSameFolderLevel all
    // sibling folders form one file list and the start file is just a position in it. LoopSubFolders
    // does not wrap around (see WrapsAround()), so it starts at the beginning of the folder as before.
    return m_bWrapAroundFolder && sm_eMode == Helpers::NM_LoopDirectory;
}

// LoopSameFolderLevel: the files of all sibling folders (this folder included) form one list, in folder
// order and within each folder in file order. With wrap around, the last file of the last folder is
// followed by the first file of the first folder; without, navigation stops there.
// The chain m_prev/m_next is a cache of the folders loaded so far, kept in folder order.
CFileList* CFileList::NextInFolderList() {
    if (!m_fileList.empty()) {
        if (m_iter == m_fileList.end()) {
            return this;
        }
        auto iterNext = std::next(m_iter);
        if (iterNext != m_fileList.end()) {
            m_iter = iterNext;
            return this;
        }
    }
    // last file of this folder (or no files): continue with the first file of the next folder
    CFileList* pNext = AdjacentFolder(true);
    if (pNext == nullptr) {
        if (!WrapsAround()) {
            return this;
        }
        pNext = OuterFolder(true);
        if (pNext == nullptr) {
            return this;
        }
    }
    pNext->m_iter = pNext->m_fileList.begin();
    return pNext;
}

CFileList* CFileList::PrevInFolderList() {
    if (!m_fileList.empty() && m_iter != m_fileList.begin()) {
        m_iter--;
        return this;
    }
    // first file of this folder (or no files): continue with the last file of the previous folder
    CFileList* pPrev = AdjacentFolder(false);
    if (pPrev == nullptr) {
        if (!WrapsAround()) {
            return this;
        }
        pPrev = OuterFolder(false);
        if (pPrev == nullptr) {
            return this;
        }
    }
    pPrev->MoveIterToLast();
    return pPrev;
}

// Returns the nearest folder of the loop with images after (before) this folder, without wrap around,
// linked into the chain. nullptr if there is none.
CFileList* CFileList::AdjacentFolder(bool bForward) {
    if (bForward && m_next != nullptr && m_bNextIsAdjacent) {
        return m_next;
    }
    if (!bForward && m_prev != nullptr && m_bPrevIsAdjacent) {
        return m_prev;
    }
    if (m_sDirectory.isEmpty()) {
        return nullptr;
    }
    CFileList* pList = AdjacentSiblingFolder(bForward);
    if (pList == nullptr || pList == this) {
        return nullptr;
    }
    if (bForward) {
        InsertAfter(pList, true);
    } else {
        InsertBefore(pList, true);
    }
    return pList;
}

// Returns the first (last) folder of the loop with images, this folder included, linked into the chain.
// nullptr if there is none.
CFileList* CFileList::OuterFolder(bool bFirst) {
    if (m_sDirectory.isEmpty()) {
        return m_fileList.empty() ? nullptr : this;
    }
    CFileList* pList = OuterSiblingFolder(bFirst);
    if (pList != nullptr && pList != this) {
        // keep the chain in folder order: the first (last) folder belongs to its start (end)
        CFileList* pOuter = this;
        if (bFirst) {
            while (pOuter->m_prev != nullptr) pOuter = pOuter->m_prev;
            if (pOuter != pList) pOuter->InsertBefore(pList, false);
        } else {
            while (pOuter->m_next != nullptr) pOuter = pOuter->m_next;
            if (pOuter != pList) pOuter->InsertAfter(pList, false);
        }
    }
    return pList;
}

// LoopSameFolderLevel: gets the parent folder and the names of all its subfolders (this folder included),
// in folder order. Returns false if this folder has no parent folder.
bool CFileList::GetSiblingFolders(QString& sParentDir, QStringList& dirList) const {
    if (m_sDirectory.isEmpty() || QDir(m_sDirectory).isRoot()) {
        return false; // root folder - no siblings
    }
    sParentDir = QFileInfo(m_sDirectory).absolutePath();
    dirList = GetSubFolders(sParentDir, false); // no recursion, so no cycles through symbolic links
    return true;
}

// LoopSameFolderLevel: see AdjacentFolder(), the result is not yet linked into the chain
CFileList* CFileList::AdjacentSiblingFolder(bool bForward) {
    QString sParentDir;
    QStringList dirList;
    if (!GetSiblingFolders(sParentDir, dirList)) {
        return nullptr;
    }
    // compared by name, so this also works if this folder has been deleted or renamed meanwhile
    const QString sThisTitle = QFileInfo(m_sDirectory).fileName();
    if (!bForward) {
        std::reverse(dirList.begin(), dirList.end());
    }
    for (const QString& sName : std::as_const(dirList)) {
        if (sName == sThisTitle) {
            continue;
        }
        const bool bBehindThis = bForward ? FolderNameLess(sThisTitle, sName) : FolderNameLess(sName, sThisTitle);
        if (bBehindThis) {
            CFileList* pList = GetFolderList(ChildPath(sParentDir, sName), 0);
            if (pList != nullptr) {
                return pList;
            }
        }
    }
    return nullptr;
}

// LoopSameFolderLevel: see OuterFolder(), the result is not yet linked into the chain
CFileList* CFileList::OuterSiblingFolder(bool bFirst) {
    QString sParentDir;
    QStringList dirList;
    if (GetSiblingFolders(sParentDir, dirList)) {
        if (!bFirst) {
            std::reverse(dirList.begin(), dirList.end());
        }
        for (const QString& sName : std::as_const(dirList)) {
            CFileList* pList = GetFolderList(ChildPath(sParentDir, sName), 0);
            if (pList != nullptr) {
                return pList;
            }
        }
    }
    return m_fileList.empty() ? nullptr : this;
}

// Returns the file list of the given folder (absolute and clean), from the chain if already loaded,
// else newly created and not yet linked into the chain. nullptr if the folder has no images.
CFileList* CFileList::GetFolderList(const QString& sDirectory, int nLevel) {
    CFileList* pList = this;
    while (pList->m_prev != nullptr) pList = pList->m_prev;
    for (; pList != nullptr; pList = pList->m_next) {
        if (pList->m_sDirectory == sDirectory) { // file names are case-sensitive on Linux
            return pList->m_fileList.empty() ? nullptr : pList;
        }
    }
    auto* pNewList = new CFileList(sDirectory, m_directoryWatcher, CFileDesc::GetSorting(), CFileDesc::IsSortedAscending(), m_bWrapAroundFolder, nLevel);
    if (pNewList->m_fileList.empty()) {
        delete pNewList;
        return nullptr;
    }
    return pNewList;
}

// Inserts pList into the chain directly behind this list, taking it out of its old place first
void CFileList::InsertAfter(CFileList* pList, bool bAdjacent) {
    if (pList != m_next) {
        pList->Unlink();
        pList->m_next = m_next;
        pList->m_bNextIsAdjacent = false;
        if (m_next != nullptr) {
            m_next->m_prev = pList;
            m_next->m_bPrevIsAdjacent = false;
        }
        m_next = pList;
        pList->m_prev = this;
        m_bNextIsAdjacent = pList->m_bPrevIsAdjacent = bAdjacent;
    } else if (bAdjacent) {
        m_bNextIsAdjacent = pList->m_bPrevIsAdjacent = true; // already linked, now known to be adjacent
    }
}

// Inserts pList into the chain directly in front of this list, taking it out of its old place first
void CFileList::InsertBefore(CFileList* pList, bool bAdjacent) {
    if (pList != m_prev) {
        pList->Unlink();
        pList->m_prev = m_prev;
        pList->m_bPrevIsAdjacent = false;
        if (m_prev != nullptr) {
            m_prev->m_next = pList;
            m_prev->m_bNextIsAdjacent = false;
        }
        m_prev = pList;
        pList->m_next = this;
        m_bPrevIsAdjacent = pList->m_bNextIsAdjacent = bAdjacent;
    } else if (bAdjacent) {
        m_bPrevIsAdjacent = pList->m_bNextIsAdjacent = true; // already linked, now known to be adjacent
    }
}

// Takes this list out of the chain, its neighbors are linked to each other
void CFileList::Unlink() {
    if (m_prev != nullptr) {
        m_prev->m_next = m_next;
        m_prev->m_bNextIsAdjacent = false;
    }
    if (m_next != nullptr) {
        m_next->m_prev = m_prev;
        m_next->m_bPrevIsAdjacent = false;
    }
    m_prev = m_next = nullptr;
    m_bPrevIsAdjacent = m_bNextIsAdjacent = false;
}

void CFileList::NextInFolder() {
    if (!m_fileList.empty()) {
        m_iter++;
        if (m_iter == m_fileList.end()) {
            m_iter = m_fileList.begin();
        }
    }
}

CFileList* CFileList::GotoFirstShown() {
    if (sm_eMode == Helpers::NM_LoopDirectory) return this;

    QString thisFile, prevFile;
    const QString firstFile = Current();
    CFileList* pThis = this;
    do {
        thisFile = pThis->Current();
        CFileList* pPrev = pThis->Prev();
        pThis = pPrev;
        prevFile = pThis->Current();
    } while (!thisFile.isEmpty() && !prevFile.isEmpty() && !firstFile.isEmpty() && thisFile != prevFile && prevFile != firstFile);

    return pThis;
}

CFileList* CFileList::TryCreateFileList(const QString& directory, int nNewLevel) {
    const QString cleanDir = QDir::cleanPath(directory);

    CFileList* pList = this;
    while (pList != nullptr) {
        if (pList->m_sDirectory == cleanDir) {
            return nullptr;
        }
        pList = pList->m_prev;
    }

    auto* pNewList = new CFileList(cleanDir, m_directoryWatcher, CFileDesc::GetSorting(), CFileDesc::IsSortedAscending(), m_bWrapAroundFolder, nNewLevel);
    if (pNewList->Size() > 0) {
        pNewList->m_prev = this;
        m_next = pNewList;
        return pNewList;
    } else {
        delete pNewList;
        return nullptr;
    }
}

void CFileList::FindFiles() {
    m_fileList.clear();
    if (m_sDirectory.isEmpty()) return;

    static const auto supportedEndings = GetSupportedFileEndingList();
    QSet<QString> validExtensions(supportedEndings.begin(), supportedEndings.end());

    QDirIterator it(m_sDirectory, QDir::Files | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        const QString ext = info.suffix().toLower();

        if (validExtensions.contains(ext)) {
            m_fileList.emplace_back(info.absoluteFilePath(), info.lastModified(), info.birthTime(), info.size());
        }
    }

    m_fileList.sort();
}

bool CFileList::IsImageFile(const QString& sEnding) {
    return GetSupportedFileEndingList().contains(sEnding.toLower());
}

bool CFileList::IsEndpoint() {
    if (m_iter == m_fileList.begin()) {
        return true;
    }

    if (!m_fileList.empty()) {
        auto iterTemp = m_iter;
        iterTemp++;
        if (iterTemp == m_fileList.end()) {
            return true;
        }
    }

    return false;
}
