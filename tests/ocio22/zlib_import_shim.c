/* A static OpenColorIO 2.2.1 built here has its minizip-ng compiled for a zlib
 * DLL (references to __imp_*). These pointers satisfy them from the static
 * zlib (zlibstatic.lib) the same build produced. Only for the test executable
 * (tests/ocio22/CMakeLists.txt, OCIO_ZLIB_IMPORT_SHIM). */
extern int crc32();
extern int deflate();
extern int deflateEnd();
extern int deflateInit2_();
extern int inflate();
extern int inflateEnd();
extern int inflateInit2_();

void *__imp_crc32 = (void *)crc32;
void *__imp_deflate = (void *)deflate;
void *__imp_deflateEnd = (void *)deflateEnd;
void *__imp_deflateInit2_ = (void *)deflateInit2_;
void *__imp_inflate = (void *)inflate;
void *__imp_inflateEnd = (void *)inflateEnd;
void *__imp_inflateInit2_ = (void *)inflateInit2_;
