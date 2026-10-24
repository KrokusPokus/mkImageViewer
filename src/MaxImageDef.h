#ifndef MAXIMAGEDEF_H
#define MAXIMAGEDEF_H

// Must not be bigger than 65535 due to internal limitations
const unsigned int MAX_IMAGE_DIMENSION = 65535;

// Sizes are in bytes
const unsigned int MAX_JPEG_FILE_SIZE = 1024 * 1024 * 500;
const unsigned int MAX_PNG_FILE_SIZE = 1024 * 1024 * 300;
const unsigned int MAX_WEBP_FILE_SIZE = 1024 * 1024 * 150;
const unsigned int MAX_JXL_FILE_SIZE = 1024 * 1024 * 150;
const unsigned int MAX_HEIF_FILE_SIZE = 1024 * 1024 * 150;
const unsigned int MAX_PSD_FILE_SIZE = 1024 * 1024 * 500;
const unsigned int MAX_BMP_FILE_SIZE = 1024 * 1024 * 500;
const unsigned int MAX_IMAGE_PIXELS = (unsigned)MAX_IMAGE_DIMENSION * MAX_IMAGE_DIMENSION;

#endif // MAXIMAGEDEF_H
