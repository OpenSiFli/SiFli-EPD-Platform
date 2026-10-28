/*----------------------------------------------------------------------------/
/ TJpgDec - Tiny JPEG Decompressor R0.03 include file         (C)ChaN, 2021
/----------------------------------------------------------------------------*/
#ifndef EPD_TJPGD_H
#define EPD_TJPGD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "epd_tjpgdcnf.h"
#include <string.h>

#if defined(_WIN32)	/* VC++ or some compiler without stdint.h */
typedef unsigned char	uint8_t;
typedef unsigned short	uint16_t;
typedef short			int16_t;
typedef unsigned long	uint32_t;
typedef long			int32_t;
#else				/* Embedded platform */
#include <stdint.h>
#endif

#if JD_FASTDECODE >= 1
typedef int16_t jd_yuv_t;
#else
typedef uint8_t jd_yuv_t;
#endif


/* Error code */
typedef enum {
	JDR_OK = 0,	/* 0: Succeeded */
	JDR_INTR,	/* 1: Interrupted by output or control function */
	JDR_INP,	/* 2: Device error or wrong termination of input stream */
	JDR_MEM1,	/* 3: Insufficient memory pool for the image */
	JDR_MEM2,	/* 4: Insufficient stream input buffer */
	JDR_PAR,	/* 5: Parameter error */
	JDR_FMT1,	/* 6: Data format error (may be broken data) */
	JDR_FMT2,	/* 7: Right format but not supported */
	JDR_FMT3	/* 8: Not supported JPEG standard */
} JRESULT;



/* Rectangular region in the output image */
typedef struct {
	uint16_t left;		/* Left end */
	uint16_t right;		/* Right end */
	uint16_t top;		/* Top end */
	uint16_t bottom;	/* Bottom end */
} JRECT;



/* Decompressor object structure */
typedef struct JDEC JDEC;
struct JDEC {
	size_t dctr;				/* Number of bytes available in the input buffer */
	uint8_t* dptr;				/* Current data read ptr */
	uint8_t* inbuf;				/* Bit stream input buffer */
	uint8_t dbit;				/* Number of bits availavble in wreg or reading bit mask */
	uint8_t scale;				/* Output scaling ratio */
	uint8_t msx, msy;			/* MCU size in unit of block (width, height) */
	uint8_t qtid[3];			/* Quantization table ID of each component, Y, Cb, Cr */
	uint8_t ncomp;				/* Number of color components 1:grayscale, 3:color */
	int16_t dcv[3];				/* Previous DC element of each component */
	uint16_t nrst;				/* Restart inverval */
	uint16_t width, height;		/* Size of the input image (pixel) */
	uint8_t* huffbits[2][2];	/* Huffman bit distribution tables [id][dcac] */
	uint16_t* huffcode[2][2];	/* Huffman code word tables [id][dcac] */
	uint8_t* huffdata[2][2];	/* Huffman decoded data tables [id][dcac] */
	int32_t* qttbl[4];			/* Dequantizer tables [id] */
#if JD_FASTDECODE >= 1
	uint32_t wreg;				/* Working shift register */
	uint8_t marker;				/* Detected marker (0:None) */
#if JD_FASTDECODE == 2
	uint8_t longofs[2][2];		/* Table offset of long code [id][dcac] */
	uint16_t* hufflut_ac[2];	/* Fast huffman decode tables for AC short code [id] */
	uint8_t* hufflut_dc[2];		/* Fast huffman decode tables for DC short code [id] */
#endif
#endif
	void* workbuf;				/* Working buffer for IDCT and RGB output */
	jd_yuv_t* mcubuf;			/* Working buffer for the MCU */
	void* pool;					/* Pointer to available memory pool */
	size_t sz_pool;				/* Size of momory pool (bytes available) */
	size_t (*infunc)(JDEC*, uint8_t*, size_t);	/* Pointer to jpeg stream input function */
	void* device;				/* Pointer to I/O device identifiler for the session */
	unsigned int mcu_x, mcu_y;	/* Next MCU origin in source pixels */
	uint16_t rst, rsc;			/* Restart interval and marker counters */
	uint8_t decomp_started;		/* jd_decomp_init has initialized this scan */
	JRESULT decomp_result;		/* Terminal decode error, including cancellation */
};


/* Control is sampled before decoding and after each completed MCU. */
typedef enum {
	JD_CONTINUE = 0,
	JD_YIELD,					/* Return at this boundary; the scan can resume */
	JD_CANCEL					/* End the scan with JDR_INTR */
} JDCONTROL;

typedef enum {
	JD_STEP_MORE = 0,			/* MCU limit reached */
	JD_STEP_DONE,				/* All source MCUs consumed */
	JD_STEP_OUTPUT_FULL,		/* Next MCU would exceed the output byte budget */
	JD_STEP_YIELDED
} JDSTEP_STATE;

typedef struct {
	unsigned int mcus;			/* Completed MCUs, including scaled-away edges */
	size_t output_bytes;		/* Pixel bytes accepted by outfunc in this call */
	JDSTEP_STATE state;			/* Valid when jd_decomp_step returns JDR_OK */
} JDSTEP;


/* TJpgDec API functions */
JRESULT jd_prepare (JDEC* jd, size_t (*infunc)(JDEC*,uint8_t*,size_t), void* pool, size_t sz_pool, void* dev);
JRESULT jd_decomp (JDEC* jd, int (*outfunc)(JDEC*,void*,JRECT*), uint8_t scale);

/* Initialize once after a successful jd_prepare. Rewind/restart requires a new
 * jd_prepare; init does not rewind input or reset an already consumed scan. */
JRESULT jd_decomp_init (JDEC* jd, uint8_t scale);

/* max_mcus must be nonzero. max_output_bytes limits pixel bytes only; the
 * caller reserves any per-block metadata separately. Insufficient space is
 * reported before consuming the next MCU or its restart marker. A zero byte
 * budget may still consume MCUs with no pixels after scaling.
 *
 * outfunc has the same ordering and packed pixel format as jd_decomp. Its data
 * is borrowed until the next MCU, so copy it before returning. As in the full
 * decoder, rect.right + 1 == (jd->width >> jd->scale) ends a visible row band;
 * the end of a step does not. MCUs rounded away by scaling have no callback.
 * Returning zero from outfunc terminates the scan with JDR_INTR, not a yield.
 *
 * control may be NULL. Callbacks run synchronously on the decoding core and
 * use jd->device for context; neither callback is retained between calls.
 * Cancellation and decode errors are terminal until a new jd_prepare. DONE
 * can be queried again without consuming input. The result counters describe
 * the accepted prefix even on error; state is only meaningful on JDR_OK.
 * Decoder, input and pool must remain alive and exclusively owned throughout
 * the scan. Initialization/stepping must not run concurrently on one JDEC. */
JRESULT jd_decomp_step (JDEC* jd, int (*outfunc)(JDEC*,void*,JRECT*),
	unsigned int max_mcus, size_t max_output_bytes,
	JDCONTROL (*control)(JDEC*), JDSTEP* step);


#ifdef __cplusplus
}
#endif

#endif /* EPD_TJPGD_H */
