/* Reuse the project inflater without exporting a second copy of miniz's API. */
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_STDIO
#define MINIZ_NO_MALLOC
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define mz_adler32 crf_mz_adler32
#define mz_crc32 crf_mz_crc32
#define mz_free crf_mz_free
#define mz_version crf_mz_version
#define miniz_def_alloc_func crf_miniz_def_alloc_func
#define miniz_def_free_func crf_miniz_def_free_func
#define miniz_def_realloc_func crf_miniz_def_realloc_func
#define tinfl_decompress crf_tinfl_decompress
#define tinfl_decompress_mem_to_heap crf_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem crf_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback crf_tinfl_decompress_mem_to_callback
#define tdefl_init crf_tdefl_init
#define tdefl_compress crf_tdefl_compress
#define tdefl_compress_buffer crf_tdefl_compress_buffer
#define tdefl_get_prev_return_status crf_tdefl_get_prev_return_status
#define tdefl_get_adler32 crf_tdefl_get_adler32
#define tdefl_create_comp_flags_from_zip_params crf_tdefl_create_comp_flags_from_zip_params
#define tdefl_compress_mem_to_output crf_tdefl_compress_mem_to_output
#define tdefl_compress_mem_to_heap crf_tdefl_compress_mem_to_heap
#define tdefl_compress_mem_to_mem crf_tdefl_compress_mem_to_mem
#define tdefl_write_image_to_png_file_in_memory crf_tdefl_write_image_to_png_file_in_memory
#define tdefl_write_image_to_png_file_in_memory_ex crf_tdefl_write_image_to_png_file_in_memory_ex
/* Keep the ACPU inflater optimized independently of the image's size policy. */
#if defined(CORE_ACPU) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("O2")
#endif
#include "../../src/thirdparty/miniz/miniz.c"
#if defined(CORE_ACPU) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
