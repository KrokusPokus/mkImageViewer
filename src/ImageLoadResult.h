#ifndef IMAGELOADRESULT_H
#define IMAGELOADRESULT_H

#include <QString>

class CBaseImage;

// Why an image could not be loaded
enum class ELoadError {
    None,               // loaded
    FileNotFound,       // the file does not exist (any more)
    FileNotReadable,    // the file (or the archive entry) could not be opened or read completely
    ArchiveError,       // the archive could not be opened or listed
    ArchiveEmpty,       // the archive contains no image entries
    NotDecodable,       // the data is corrupt or the format is not supported
    TooLarge,           // file size or image dimensions above the limits
    OutOfMemory,        // memory could not be allocated
    Exception,          // unexpected exception in a decoder
    Aborted,            // read ahead stopped by the owner before processing (internal, never shown)
    ReadAheadSkipped    // read ahead not started: less than Helpers::READ_AHEAD_MIN_FREE_PERCENT of the memory is
                        // available (internal, never shown; the image is loaded when the user goes to it)
};

// Where an image is: file and frame (page of an archive, frame of an animation or multi page file).
// Known for every load attempt, also if the image could not be read. Navigation uses this, not the image.
struct CImagePosition {
    QString FileName;
    int FrameIndex = 0;
    int FrameCount = 1;         // 1 if the file has a single frame or the count is unknown (failed load outside of an archive)
    bool IsAnimation = false;
    bool InArchive = false;
    QString EntryName;              // archives: path of the image file within the archive (empty otherwise)
    unsigned long long EntrySize = 0; // archives: size of that file in bytes (uncompressed)

    // Page of an archive with several pages (the archive is navigated page by page)
    bool IsMultiPageArchive() const { return InArchive && FrameCount > 1; }
};

// Result of an image request: the image (nullptr if it could not be loaded), the reason and the position
struct CImageLoadResult {
    CBaseImage* Image = nullptr;
    ELoadError Error = ELoadError::None;
    CImagePosition Position;
};

#endif // IMAGELOADRESULT_H
