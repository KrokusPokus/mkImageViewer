#include "FileList.h"
#include "SettingsProvider.h"
#include "Helpers.h"
#include "DirectoryWatcher.h"

#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QCollator>
#include <QFile>
#include <QTextStream>
#include <QSet>
#include <random>
#include <algorithm>
#include <cassert>

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

// Unterstützte Dateiendungen
static const QStringList& GetSupportedFileEndingList() {
    static const QStringList endings = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jfif"), QStringLiteral("bmp"), 
        QStringLiteral("png"), QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("gif"), 
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
    m_sDirectory = bIsDirectory ? initialInfo.absoluteFilePath() : initialInfo.absolutePath();

    const QString sExtension = initialInfo.suffix().toLower();
    const bool bImageFile = !bIsDirectory && IsImageFile(sExtension);

    m_directoryWatcher.SetCurrentDirectory(m_sDirectory);

    if (bImageFile || bIsDirectory) {
        FindFiles();
        m_iter = FindFile(m_sInitialFile);
        m_iterStart = bWrapAroundFolder ? m_iter : m_fileList.begin();
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
    }

    FindFiles();
    m_iterStart = m_bWrapAroundFolder ? FindFile(m_sInitialFile) : m_fileList.begin();
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
    // The old code compared case-insensitive (Windows); file names on Linux are case-sensitive,
    // otherwise renaming "a.jpg" would also hit an unrelated "A.jpg".
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

    // Without wrap-around the iteration starts at the first element, which may have changed.
    // (With wrap-around m_iterStart is a position, i.e. an iterator, which stays valid.)
    if (!m_bWrapAroundFolder) {
        m_iterStart = m_fileList.begin();
    }
}

bool CFileList::CurrentFileExists() const {
    const QString cur = Current();
    return !cur.isEmpty() && QFileInfo::exists(cur);
}

CFileList* CFileList::Next() {
    if (!m_fileList.empty()) {
        auto iterTemp = m_iter;
        if (iterTemp == m_fileList.end()) return this;

        iterTemp++;
        if (iterTemp == m_fileList.end()) {
            iterTemp = m_fileList.begin();
        }
        if (iterTemp == m_iterStart) {
            if (!m_bWrapAroundFolder && sm_eMode == Helpers::NM_LoopDirectory) {
                return this;
            }
            CFileList* pNextList = WrapToNextImage();
            if (pNextList == nullptr) {
                pNextList = this;
                while (pNextList->m_prev != nullptr) pNextList = pNextList->m_prev;
                if (pNextList != this) {
                    return m_bWrapAroundFolder ? GotoFirstShown() : this;
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
    if (m_iter == m_iterStart) {
        if (sm_eMode == Helpers::NM_LoopDirectory) {
            if (!m_bWrapAroundFolder) {
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

void CFileList::First() {
    m_iter = m_iterStart = m_fileList.begin();
}

void CFileList::Last() {
    MoveIterToLast();
    m_iterStart = m_fileList.begin();
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
        m_iterStart = m_bWrapAroundFolder ? m_iter : m_fileList.begin();
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
    m_iterStart = m_bWrapAroundFolder ? m_iter : m_fileList.begin();
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
    }
    m_next = nullptr;
}

///////////////////////////////////////////////////////////////////////////////////
// Private Methods
///////////////////////////////////////////////////////////////////////////////////

void CFileList::MoveIterToLast() {
    auto lastIter = m_iter;
    while (m_iter != m_fileList.end()) {
        lastIter = m_iter;
        m_iter++;
    }
    m_iter = lastIter;
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

    if (sm_eMode == Helpers::NM_LoopDirectory) {
        return nullptr;
    } 
    else if (sm_eMode == Helpers::NM_LoopSameDirectoryLevel) {
        const QDir parentDir(m_sDirectory + QStringLiteral("/.."));
        const QString sNextDirRoot = parentDir.canonicalPath();
        const QString sThisDirTitle = QFileInfo(m_sDirectory).fileName();

        if (sNextDirRoot.isEmpty()) {
            return nullptr;
        }

        QDir dirRoot(sNextDirRoot);
        QStringList dirList = dirRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (dirList.isEmpty()) {
            return nullptr;
        }

        std::sort(dirList.begin(), dirList.end(), [](const QString& a, const QString& b) {
            return GetLogicalCollator().compare(a, b) < 0;
        });

        bool bFound = false;
        for (int nStep = 0; nStep < 2; nStep++) {
            for (const QString& dirName : dirList) {
                if (dirName.compare(sThisDirTitle, Qt::CaseInsensitive) == 0) {
                    bFound = true;
                } else if (bFound) {
                    const QString candidatePath = sNextDirRoot + QDir::separator() + dirName;
                    CFileList* pNewList = TryCreateFileList(candidatePath, 0);
                    if (pNewList != nullptr) {
                        return pNewList;
                    }
                }
            }
        }
    } 
    else if (sm_eMode == Helpers::NM_LoopSubDirectories) {
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
        std::sort(dirList.begin(), dirList.end(), [](const QString& a, const QString& b) {
            return GetLogicalCollator().compare(a, b) < 0;
        });

        bool bStart = sFindAfter.isEmpty();
        for (const QString& subDirName : dirList) {
            if (bStart) {
                const QString subDirPath = sDirectory + QDir::separator() + subDirName;
                CFileList* pFileList = FindFileRecursively(subDirPath, QString(), true, nLevel + 1, nRecursion + 1);
                if (pFileList != nullptr) {
                    return pFileList;
                }
            }
            if (subDirName.compare(sFindAfter, Qt::CaseInsensitive) == 0) {
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
        if (pList->m_sDirectory.compare(cleanDir, Qt::CaseInsensitive) == 0) {
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
