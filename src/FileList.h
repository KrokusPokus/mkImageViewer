#ifndef FILELIST_H
#define FILELIST_H

#include "Helpers.h"

#include <QString>
#include <QDateTime>
#include <QStringList>

#include <list>

// Entry in the file list, allowing sorting by different sort criteria
class CFileDesc
{
public:
    CFileDesc(const QString& sName, const QDateTime& lastModTime, const QDateTime& creationTime, qint64 fileSize);

    // STL / C++20 sort operator
    bool operator < (const CFileDesc& other) const;
    bool SortAscending(const CFileDesc& other) const;

    // Get and set the sorting method - the sorting method is global
    static Helpers::ESorting GetSorting() { return sm_eSorting; }
    static bool IsSortedAscending() { return sm_bSortAscending; }
    static void SetSorting(Helpers::ESorting eSorting, bool bAscending) { sm_eSorting = eSorting; sm_bSortAscending = bAscending; }

    // Full name of file
    const QString& GetName() const { return m_sName; }

    // File title (without path)
    const QString& GetTitle() const { return m_sTitle; }

    // Gets last modification time
    const QDateTime& GetLastModTime() const { return m_lastModTime; }

    // Gets creation time
    const QDateTime& GetCreationTime() const { return m_creationTime; }

    // Gets file size in bytes
    qint64 GetFileSize() const { return m_fileSize; }

    // Renames the entry (full path). Only name and title change; times and size stay the same,
    // as a rename does not touch the modification time. Does NOT re-sort, see CFileList::FileHasRenamed().
    void SetName(const QString& sName);

private:
    static Helpers::ESorting sm_eSorting;
    static bool sm_bSortAscending;

    QString m_sName;
    QString m_sTitle;
    QDateTime m_lastModTime;
    QDateTime m_creationTime;
    int m_nRandomOrderNumber{ 0 };
    qint64 m_fileSize{ 0 };
};


// Manages list of files to display and navigation between image files
class CFileList
{
public:
    // sInitialFile is the initial file to show with path. It can either be an image file, a directory
    // or a text file containing file names to display.
    // The list does not touch the directory watcher: it also creates lists of other folders while searching
    // the next folder of the loop. The owner watches CurrentDirectory() of the list it shows.
    CFileList(const QString& sInitialFile,
              Helpers::ESorting eInitialSorting, bool isSortedAscending, bool bWrapAroundFolder, int nLevel = 0);
    ~CFileList();

    // Reload file list for given file, if empty for current file
    void Reload(const QString& sFileName = QString(), bool clearForwardHistory = true);

    // Move to next file in list, wrap according to navigation mode.
    CFileList* Next();

    // Move to previous file in list.
    CFileList* Prev();

    // Move to first file (according to sort order): of the current folder, or in the LoopSameFolderLevel and
    // LoopSubFolders modes of the first folder of the loop containing images. The filelist object of that file is returned.
    CFileList* First();

    // Move to last file (according to sort order): of the current folder, or in the LoopSameFolderLevel and
    // LoopSubFolders modes of the last folder of the loop containing images. The filelist object of that file is returned.
    CFileList* Last();

    // Returns if navigation wraps around from the last to the first file and vice versa in the current navigation mode
    bool WrapsAround() const;

    // Move away from current image, either forward or backward (if on last image)
    CFileList* AwayFromCurrent();

    // Filename (with path) of the current item in the list, empty QString if none
    QString Current() const;

    // File title of the current item in the list, empty QString if none
    QString CurrentFileTitle() const;

    // Current directory without file name, empty QString if none
    QString CurrentDirectory() const;

    // Get the n-next file, does not change the internal state
    QString PeekNextPrev(int nIndex, bool bForward);

    // Number of files in file list (for current directory)
    int Size() const { return static_cast<int>(m_fileList.size()); }

    // Sets the sorting of the file list and resorts the list
    void SetSorting(Helpers::ESorting eSorting, bool sortAscending);

    // Gets the sorting mode
    Helpers::ESorting GetSorting() const;
    bool IsSortedAscending() const;

    // Set the directory navigation mode
    void SetNavigationMode(Helpers::ENavigationMode eMode);

    // Gets current navigation mode
    Helpers::ENavigationMode GetNavigationMode() const { return sm_eMode; }

    // Sets a checkpoint on the current image
    void SetCheckpoint() { m_iterCheckPoint = m_iter; }

    // Check if we are now on another image since the last checkpoint was set
    bool ChangedSinceCheckpoint() const { return m_iterCheckPoint != m_iter; }

    // Tells the list that a file was renamed (both parameters with full path, same directory).
    // The entry is renamed in place and moved to its new position if the list is sorted by name,
    // so the iterator (current file) keeps pointing to the renamed file. No directory access, no reload.
    // Only the list of the current folder is touched: lists chained for other folders cannot contain the
    // file (GetFolderList() never creates two lists for one folder).
    void FileHasRenamed(const QString& sOldFileName, const QString& sNewFileName);

    // Returns if the current file exists
    bool CurrentFileExists() const;

    // Returns if the current file exists and can be opened for reading
    bool CanOpenCurrentFileForReading() const;

    // delete the chain of CFileLists forward and backward and only leave the current node alive
    void DeleteHistory(bool onlyForward = false);

    bool IsEndpoint(); // Check if at the beginning or end of the file list.

private:
    static Helpers::ENavigationMode sm_eMode;

    bool m_bDeleteHistory{ true };
    bool m_bWrapAroundFolder{ false };
    int m_nLevel{ 0 }; // LoopSubFolders: depth of this folder below the start folder of the loop
    QString m_sInitialFile;
    QString m_sDirectory;

    // filelists for several folders are chained
    CFileList* m_next{ nullptr };
    CFileList* m_prev{ nullptr };
    // LoopSameFolderLevel and LoopSubFolders: the chain is a cache of the folders of the loop loaded so far,
    // in folder order. These flags are true if it is known that no other folder of the loop with images lies
    // between this folder and m_next (m_prev), so the folders on disk need not be enumerated again.
    bool m_bNextIsAdjacent{ false };
    bool m_bPrevIsAdjacent{ false };
    std::list<CFileDesc> m_fileList;
    std::list<CFileDesc>::iterator m_iter;          // current position in m_fileList
    std::list<CFileDesc>::iterator m_iterStart;     // start of iteration in m_fileList
    std::list<CFileDesc>::iterator m_iterCheckPoint;

    void MoveIterToLast();
    void NextInFolder();
    std::list<CFileDesc>::iterator FindFile(const QString& sName);
    bool UsesStartFileAsLoopStart() const;
    // LoopSameFolderLevel and LoopSubFolders: all folders of the loop form one file list, see NextInFolderList()
    CFileList* NextInFolderList();
    CFileList* PrevInFolderList();
    CFileList* AdjacentFolder(bool bForward);
    CFileList* OuterFolder(bool bFirst);
    bool GetSiblingFolders(QString& sParentDir, QStringList& dirList) const;
    CFileList* AdjacentSiblingFolder(bool bForward);
    CFileList* OuterSiblingFolder(bool bFirst);
    CFileList* AdjacentSubFolder(bool bForward);
    CFileList* OuterSubFolder(bool bFirst);
    QString RootDirectory() const;
    CFileList* GetFolderList(const QString& sDirectory, int nLevel);
    void InsertAfter(CFileList* pList, bool bAdjacent);
    void InsertBefore(CFileList* pList, bool bAdjacent);
    void Unlink();
    void FindFiles();
    static bool IsImageFile(const QString& sEnding);
};

#endif // FILELIST_H
