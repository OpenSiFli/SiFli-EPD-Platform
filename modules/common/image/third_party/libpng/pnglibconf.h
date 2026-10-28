/* pnglibconf.h - library build configuration */

/* libpng version 1.6.58 */

/* Copyright (c) 2018-2026 Cosmin Truta */
/* Copyright (c) 1998-2002,2004,2006-2018 Glenn Randers-Pehrson */

/* This code is released under the libpng license. */
/* For conditions of distribution and use, see the disclaimer */
/* and license in png.h */

/* pnglibconf.h */
/* Gallery sequential read configuration. */
/* Derived from: scripts/pnglibconf.dfa */
#ifndef PNGLCONF_H
#define PNGLCONF_H
/* options */
#define PNG_16BIT_SUPPORTED
#define PNG_ALIGNED_MEMORY_SUPPORTED
/*#undef PNG_ARM_NEON_API_SUPPORTED*/
/*#undef PNG_ARM_NEON_CHECK_SUPPORTED*/
#define PNG_BENIGN_ERRORS_SUPPORTED
#define PNG_BENIGN_READ_ERRORS_SUPPORTED
/*#undef PNG_BENIGN_WRITE_ERRORS_SUPPORTED*/
/*#undef PNG_DISABLE_ADLER32_CHECK_SUPPORTED*/
/*#undef PNG_ERROR_NUMBERS_SUPPORTED*/
#define PNG_ERROR_TEXT_SUPPORTED
#define PNG_EASY_ACCESS_SUPPORTED
#define PNG_FIXED_POINT_SUPPORTED
#define PNG_IO_STATE_SUPPORTED
/*#undef PNG_MIPS_MMI_API_SUPPORTED*/
/*#undef PNG_MIPS_MMI_CHECK_SUPPORTED*/
/*#undef PNG_MIPS_MSA_API_SUPPORTED*/
/*#undef PNG_MIPS_MSA_CHECK_SUPPORTED*/
#define PNG_POINTER_INDEXING_SUPPORTED
/*#undef PNG_POWERPC_VSX_API_SUPPORTED*/
/*#undef PNG_POWERPC_VSX_CHECK_SUPPORTED*/
#define PNG_READ_16BIT_SUPPORTED
#define PNG_READ_ANCILLARY_CHUNKS_SUPPORTED
#define PNG_READ_EXPAND_SUPPORTED
#define PNG_READ_FILLER_SUPPORTED
#define PNG_READ_GRAY_TO_RGB_SUPPORTED
#define PNG_READ_INTERLACING_SUPPORTED
#define PNG_READ_INT_FUNCTIONS_SUPPORTED
#define PNG_READ_PACK_SUPPORTED
#define PNG_READ_STRIP_16_TO_8_SUPPORTED
#define PNG_READ_SUPPORTED
#define PNG_READ_TRANSFORMS_SUPPORTED
#define PNG_READ_tRNS_SUPPORTED
#define PNG_SEQUENTIAL_READ_SUPPORTED
#define PNG_SETJMP_SUPPORTED
#define PNG_SET_USER_LIMITS_SUPPORTED
#define PNG_USER_LIMITS_SUPPORTED
#define PNG_USER_MEM_SUPPORTED
#define PNG_WARNINGS_SUPPORTED
#define PNG_tRNS_SUPPORTED
/* end of options */
/* settings */
#define PNG_API_RULE 0
#define PNG_DEFAULT_READ_MACROS 1
#define PNG_GAMMA_THRESHOLD_FIXED 5000
#define PNG_IDAT_READ_SIZE PNG_ZBUF_SIZE
#define PNG_INFLATE_BUF_SIZE 1024
#define PNG_LINKAGE_API extern
#define PNG_LINKAGE_CALLBACK extern
#define PNG_LINKAGE_DATA extern
#define PNG_LINKAGE_FUNCTION extern
#define PNG_MAX_GAMMA_8 11
#define PNG_QUANTIZE_BLUE_BITS 5
#define PNG_QUANTIZE_GREEN_BITS 5
#define PNG_QUANTIZE_RED_BITS 5
#define PNG_TEXT_Z_DEFAULT_COMPRESSION (-1)
#define PNG_TEXT_Z_DEFAULT_STRATEGY 0
#define PNG_USER_CHUNK_CACHE_MAX 0
#define PNG_USER_CHUNK_MALLOC_MAX 0
/* PNG dimensions are positive 31-bit integers. Allocations use the app heap. */
#define PNG_USER_HEIGHT_MAX 0x7fffffffU
#define PNG_USER_WIDTH_MAX 0x7fffffffU
#define PNG_ZBUF_SIZE 8192
#define PNG_ZLIB_VERNUM 0 /* unknown */
#define PNG_Z_DEFAULT_COMPRESSION (-1)
#define PNG_Z_DEFAULT_NOFILTER_STRATEGY 0
#define PNG_Z_DEFAULT_STRATEGY 1
#define PNG_sCAL_PRECISION 5
#define PNG_sRGB_PROFILE_CHECKS 2
/* end of settings */
#endif /* PNGLCONF_H */
