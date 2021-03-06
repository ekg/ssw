/* The MIT License

   Copyright (c) 2012-1015 Boston College.

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

/* Contact: Mengyao Zhao <zhangmp@bc.edu> */
/* Contact: Erik Garrison <erik.garrison@bc.edu> */

/*
 *  ssw.c
 *
 *  Created by Mengyao Zhao on 6/22/10.
 *  Copyright 2010 Boston College. All rights reserved.
 *	Version 0.1.4
 *	Last revision by Erik Garrison 01/02/2014
 *
 */

#include <emmintrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "ssw.h"

#ifdef __GNUC__
#define LIKELY(x) __builtin_expect((x),1)
#define UNLIKELY(x) __builtin_expect((x),0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* Convert the coordinate in the scoring matrix into the coordinate in one line of the band. */
#define set_u(u, w, i, j) { int x=(i)-(w); x=x>0?x:0; (u)=(j)-x+1; }

/* Convert the coordinate in the direction matrix into the coordinate in one line of the band. */
#define set_d(u, w, i, j, p) { int x=(i)-(w); x=x>0?x:0; x=(j)-x; (u)=x*3+p; }

/*! @function
  @abstract  Round an integer to the next closest power-2 integer.
  @param  x  integer to be rounded (in place)
  @discussion x will be modified.
 */
#define kroundup32(x) (--(x), (x)|=(x)>>1, (x)|=(x)>>2, (x)|=(x)>>4, (x)|=(x)>>8, (x)|=(x)>>16, ++(x))


/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch. */
__m128i* qP_byte (const int8_t* read_num,
				  const int8_t* mat,
				  const int32_t readLen,
				  const int32_t n,	/* the edge length of the squre matrix mat */
				  uint8_t bias) {

	int32_t segLen = (readLen + 15) / 16; /* Split the 128 bit register into 16 pieces.
								     Each piece is 8 bit. Split the read into 16 segments.
								     Calculat 16 segments in parallel.
								   */
	__m128i* vProfile = (__m128i*)malloc(n * segLen * sizeof(__m128i));
	int8_t* t = (int8_t*)vProfile;
	int32_t nt, i, j, segNum;

	/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch */
	for (nt = 0; LIKELY(nt < n); nt ++) {
		for (i = 0; i < segLen; i ++) {
			j = i;
			for (segNum = 0; LIKELY(segNum < 16) ; segNum ++) {
				*t++ = j>= readLen ? bias : mat[nt * n + read_num[j]] + bias;
				j += segLen;
			}
		}
	}
	return vProfile;
}

/* To determine the maximum values within each vector, rather than between vectors. */

#define m128i_max16(m, vm) \
    (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 8)); \
    (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 4)); \
    (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 2)); \
    (vm) = _mm_max_epu8((vm), _mm_srli_si128((vm), 1)); \
    (m) = _mm_extract_epi16((vm), 0)

#define m128i_max8(m, vm) \
    (vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 8)); \
    (vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 4)); \
    (vm) = _mm_max_epi16((vm), _mm_srli_si128((vm), 2)); \
    (m) = _mm_extract_epi16((vm), 0)

/* Striped Smith-Waterman
   Record the highest score of each reference position.
   Return the alignment score and ending position of the best alignment, 2nd best alignment, etc.
   Gap begin and gap extension are different.
   wight_match > 0, all other weights < 0.
   The returned positions are 0-based.
 */
alignment_end* sw_sse2_byte (const int8_t* ref,
                             int8_t ref_dir,	// 0: forward ref; 1: reverse ref
                             int32_t refLen,
                             int32_t readLen,
                             const uint8_t weight_gapO, /* will be used as - */
                             const uint8_t weight_gapE, /* will be used as - */
                             __m128i* vProfile,
                             uint8_t terminate,	/* the best alignment score: used to terminate
                                                   the matrix calculation when locating the
                                                   alignment beginning point. If this score
                                                   is set to 0, it will not be used */
                             uint8_t bias,  /* Shift 0 point to a positive value. */
                             int32_t maskLen,
                             s_align* alignment, /* to save seed and matrix */
                             const s_seed* seed) {     /* to seed the alignment */

	uint8_t max = 0;		                     /* the max alignment score */
	int32_t end_read = readLen - 1;
	int32_t end_ref = -1; /* 0_based best alignment ending point; Initialized as isn't aligned -1. */
	int32_t segLen = (readLen + 15) / 16; /* number of segment */

    /* Initialize buffers used in alignment */
	__m128i* pvHStore;
    __m128i* pvHLoad;
    __m128i* pvHmax;
    __m128i* pvE;
    uint8_t* mH; // used to save matrix for external traceback
    /* Note use of aligned memory.  Return value of 0 means success for posix_memalign. */
    if (!(!posix_memalign((void**)&pvHStore,     sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&pvHLoad,      sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&pvHmax,       sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&pvE,          sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&alignment->seed.pvE,      sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&alignment->seed.pvHStore, sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&mH,           sizeof(__m128i), segLen*refLen*sizeof(__m128i)))) {
        fprintf(stderr, "Could not allocate memory required for alignment buffers.\n");
        exit(1);
    }

    /* Workaround because we don't have an aligned calloc */
    memset(pvHStore,                 0, segLen*sizeof(__m128i));
    memset(pvHLoad,                  0, segLen*sizeof(__m128i));
    memset(pvHmax,                   0, segLen*sizeof(__m128i));
    memset(pvE,                      0, segLen*sizeof(__m128i));
    memset(alignment->seed.pvE,      0, segLen*sizeof(__m128i));
    memset(alignment->seed.pvHStore, 0, segLen*sizeof(__m128i));
    memset(mH,                       0, segLen*refLen*sizeof(__m128i));

    /* if we are running a seeded alignment, copy over the seeds */
    if (seed) {
        memcpy(pvE, seed->pvE, segLen*sizeof(__m128i));
        memcpy(pvHStore, seed->pvHStore, segLen*sizeof(__m128i));
    }

    /* Set external H matrix pointer */
    alignment->mH = mH;

    /* Record that we have done a byte-order alignment */
    alignment->is_byte = 1;

	/* Define 16 byte 0 vector. */
	__m128i vZero = _mm_set1_epi32(0);

    /* Used for iteration */
	int32_t i, j;

    /* 16 byte insertion begin vector */
	__m128i vGapO = _mm_set1_epi8(weight_gapO);

	/* 16 byte insertion extension vector */
	__m128i vGapE = _mm_set1_epi8(weight_gapE);

	/* 16 byte bias vector */
	__m128i vBias = _mm_set1_epi8(bias);

	__m128i vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	__m128i vMaxMark = vZero; /* Trace the highest score till the previous column. */
	__m128i vTemp;
	int32_t begin = 0, end = refLen, step = 1;

	/* outer loop to process the reference sequence */
	if (ref_dir == 1) {
		begin = refLen - 1;
		end = -1;
		step = -1;
	}
	for (i = begin; LIKELY(i != end); i += step) {
		int32_t cmp;
		__m128i e = vZero, vF = vZero, vMaxColumn = vZero; /* Initialize F value to 0.
							   Any errors to vH values will be corrected in the Lazy_F loop.
							 */
		//max16(maxColumn[i], vMaxColumn);
		//fprintf(stderr, "middle[%d]: %d\n", i, maxColumn[i]);

		//__m128i vH = pvHStore[segLen - 1];
        __m128i vH = _mm_load_si128 (pvHStore + (segLen - 1));
		vH = _mm_slli_si128 (vH, 1); /* Shift the 128-bit value in vH left by 1 byte. */
		__m128i* vP = vProfile + ref[i] * segLen; /* Right part of the vProfile */

		/* Swap the 2 H buffers. */
		__m128i* pv = pvHLoad;
		pvHLoad = pvHStore;
		pvHStore = pv;

		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); ++j) {

			vH = _mm_adds_epu8(vH, _mm_load_si128(vP + j));
			vH = _mm_subs_epu8(vH, vBias); /* vH will be always > 0 */
	//	max16(maxColumn[i], vH);
	//	fprintf(stderr, "H[%d]: %d\n", i, maxColumn[i]);
            /*
            int8_t* t;
            int32_t ti;
            fprintf(stdout, "%d\n", i);
            for (t = (int8_t*)&vH, ti = 0; ti < 16; ++ti) fprintf(stdout, "%d\t", *t++);
            fprintf(stdout, "\n");
            */

			/* Get max from vH, vE and vF. */
			e = _mm_load_si128(pvE + j);
			//_mm_store_si128(vE + j, e);

			vH = _mm_max_epu8(vH, e);
			vH = _mm_max_epu8(vH, vF);
			vMaxColumn = _mm_max_epu8(vMaxColumn, vH);

            // max16(maxColumn[i], vMaxColumn);
            //fprintf(stdout, "middle[%d]: %d\n", i, maxColumn[i]);
            //fprintf(stdout, "i=%d, j=%d\t", i, j);
            //for (t = (int8_t*)&vMaxColumn, ti = 0; ti < 16; ++ti) fprintf(stdout, "%d\t", *t++);
            //fprintf(stdout, "\n");

			/* Save vH values. */
			_mm_store_si128(pvHStore + j, vH);

			/* Update vE value. */
			vH = _mm_subs_epu8(vH, vGapO); /* saturation arithmetic, result >= 0 */
			e = _mm_subs_epu8(e, vGapE);
			e = _mm_max_epu8(e, vH);

			/* Update vF value. */
			vF = _mm_subs_epu8(vF, vGapE);
			vF = _mm_max_epu8(vF, vH);

            /* Save E */
			_mm_store_si128(pvE + j, e);

			/* Load the next vH. */
			vH = _mm_load_si128(pvHLoad + j);
		}


		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
        /* reset pointers to the start of the saved data */
        j = 0;
        vH = _mm_load_si128 (pvHStore + j);

        /*  the computed vF value is for the given column.  since */
        /*  we are at the end, we need to shift the vF value over */
        /*  to the next column. */
        vF = _mm_slli_si128 (vF, 1);

        vTemp = _mm_subs_epu8 (vH, vGapO);
		vTemp = _mm_subs_epu8 (vF, vTemp);
		vTemp = _mm_cmpeq_epi8 (vTemp, vZero);
		cmp  = _mm_movemask_epi8 (vTemp);
        while (cmp != 0xffff)
        {
            vH = _mm_max_epu8 (vH, vF);
			vMaxColumn = _mm_max_epu8(vMaxColumn, vH);
            _mm_store_si128 (pvHStore + j, vH);

            vF = _mm_subs_epu8 (vF, vGapE);

            j++;
            if (j >= segLen)
            {
                j = 0;
                vF = _mm_slli_si128 (vF, 1);
            }

            vH = _mm_load_si128 (pvHStore + j);
            vTemp = _mm_subs_epu8 (vH, vGapO);
            vTemp = _mm_subs_epu8 (vF, vTemp);
            vTemp = _mm_cmpeq_epi8 (vTemp, vZero);
            cmp  = _mm_movemask_epi8 (vTemp);
        }

		vMaxScore = _mm_max_epu8(vMaxScore, vMaxColumn);
		vTemp = _mm_cmpeq_epi8(vMaxMark, vMaxScore);
		cmp = _mm_movemask_epi8(vTemp);
		if (cmp != 0xffff) {
			uint8_t temp;
			vMaxMark = vMaxScore;
			m128i_max16(temp, vMaxScore);
			vMaxScore = vMaxMark;

			if (LIKELY(temp > max)) {
				max = temp;
				if (max + bias >= 255) break;	//overflow
				end_ref = i;

				/* Store the column with the highest alignment score in order to trace the alignment ending position on read. */
				for (j = 0; LIKELY(j < segLen); ++j) pvHmax[j] = pvHStore[j];

			}
		}

        // save the current column

        //fprintf(stdout, "%i %i\n", i, j);
        for (j = 0; LIKELY(j < segLen); ++j) {
            uint8_t* t;
            int32_t ti;
            vH = pvHStore[j];
            for (t = (uint8_t*)&vH, ti = 0; ti < 16; ++ti) {
                //fprintf(stderr, "%d\t", *t);
                ((uint8_t*)mH)[i*readLen + ti*segLen + j] = *t++;
            }
            //fprintf(stderr, "\n");
        }


		/* Record the max score of current column. */
		//max16(maxColumn[i], vMaxColumn);
		//fprintf(stderr, "maxColumn[%d]: %d\n", i, maxColumn[i]);
		//if (maxColumn[i] == terminate) break;

	}
        
    //fprintf(stderr, "%p %p %p %p %p %p\n", *pmH, mH, pvHmax, pvE, pvHLoad, pvHStore);
    // save the last vH
    memcpy(alignment->seed.pvE,      pvE,      segLen*sizeof(__m128i));
    memcpy(alignment->seed.pvHStore, pvHStore, segLen*sizeof(__m128i));

	/* Trace the alignment ending position on read. */
	uint8_t *t = (uint8_t*)pvHmax;
	int32_t column_len = segLen * 16;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		int32_t temp;
		if (*t == max) {
			temp = i / 16 + i % 16 * segLen;
			if (temp < end_read) end_read = temp;
		}
	}

    //fprintf(stderr, "%p %p %p %p %p %p\n", *pmH, mH, pvHmax, pvE, pvHLoad, pvHStore);

	free(pvE);
	free(pvHmax);
	free(pvHLoad);
    free(pvHStore);

	/* Find the most possible 2nd best alignment. */
	alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max + bias >= 255 ? 255 : max;
	bests[0].ref = end_ref;
	bests[0].read = end_read;


	return bests;
}

__m128i* qP_word (const int8_t* read_num,
				  const int8_t* mat,
				  const int32_t readLen,
				  const int32_t n) {

	int32_t segLen = (readLen + 7) / 8;
	__m128i* vProfile = (__m128i*)malloc(n * segLen * sizeof(__m128i));
	int16_t* t = (int16_t*)vProfile;
	int32_t nt, i, j;
	int32_t segNum;

	/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch */
	for (nt = 0; LIKELY(nt < n); nt ++) {
		for (i = 0; i < segLen; i ++) {
			j = i;
			for (segNum = 0; LIKELY(segNum < 8) ; segNum ++) {
				*t++ = j>= readLen ? 0 : mat[nt * n + read_num[j]];
				j += segLen;
			}
		}
	}
	return vProfile;
}

alignment_end* sw_sse2_word (const int8_t* ref,
							 int8_t ref_dir,	// 0: forward ref; 1: reverse ref
							 int32_t refLen,
							 int32_t readLen,
							 const uint8_t weight_gapO, /* will be used as - */
							 const uint8_t weight_gapE, /* will be used as - */
						     __m128i* vProfile,
							 uint16_t terminate,
							 int32_t maskLen,
                             s_align* alignment, /* to save seed and matrix */
                             const s_seed* seed) {     /* to seed the alignment */
    

	uint16_t max = 0;		                     /* the max alignment score */
	int32_t end_read = readLen - 1;
	int32_t end_ref = 0; /* 1_based best alignment ending point; Initialized as isn't aligned - 0. */
	int32_t segLen = (readLen + 7) / 8; /* number of segment */

    /* Initialize buffers used in alignment */
	__m128i* pvHStore;
    __m128i* pvHLoad;
    __m128i* pvHmax;
    __m128i* pvE;
    uint16_t* mH; // used to save matrix for external traceback
    /* Note use of aligned memory */

    if (!(!posix_memalign((void**)&pvHStore,     sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&pvHLoad,      sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&pvHmax,       sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&pvE,          sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&alignment->seed.pvE,      sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&alignment->seed.pvHStore, sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&mH,           sizeof(__m128i), segLen*refLen*sizeof(__m128i)))) {
        fprintf(stderr, "Could not allocate memory required for alignment buffers.\n");
        exit(1);
    }

    /* Workaround because we don't have an aligned calloc */
    memset(pvHStore,                 0, segLen*sizeof(__m128i));
    memset(pvHLoad,                  0, segLen*sizeof(__m128i));
    memset(pvHmax,                   0, segLen*sizeof(__m128i));
    memset(pvE,                      0, segLen*sizeof(__m128i));
    memset(alignment->seed.pvE,      0, segLen*sizeof(__m128i));
    memset(alignment->seed.pvHStore, 0, segLen*sizeof(__m128i));
    memset(mH,                       0, segLen*refLen*sizeof(__m128i));

    /* if we are running a seeded alignment, copy over the seeds */
    if (seed) {
        memcpy(pvE, seed->pvE, segLen*sizeof(__m128i));
        memcpy(pvHStore, seed->pvHStore, segLen*sizeof(__m128i));
    }

    /* Set external H matrix pointer */
    alignment->mH = mH;

    /* Record that we have done a word-order alignment */
    alignment->is_byte = 0;

	/* Define 16 byte 0 vector. */
	__m128i vZero = _mm_set1_epi32(0);

    /* Used for iteration */
	int32_t i, j, k;

	/* 16 byte insertion begin vector */
	__m128i vGapO = _mm_set1_epi16(weight_gapO);

	/* 16 byte insertion extension vector */
	__m128i vGapE = _mm_set1_epi16(weight_gapE);

	/* 16 byte bias vector */
	__m128i vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	__m128i vMaxMark = vZero; /* Trace the highest score till the previous column. */
	__m128i vTemp;
	int32_t begin = 0, end = refLen, step = 1;

	/* outer loop to process the reference sequence */
	if (ref_dir == 1) {
		begin = refLen - 1;
		end = -1;
		step = -1;
	}
	for (i = begin; LIKELY(i != end); i += step) {
		int32_t cmp;
		__m128i e = vZero, vF = vZero; /* Initialize F value to 0.
							   Any errors to vH values will be corrected in the Lazy_F loop.
							 */
		__m128i vH = pvHStore[segLen - 1];
		vH = _mm_slli_si128 (vH, 2); /* Shift the 128-bit value in vH left by 2 byte. */

		/* Swap the 2 H buffers. */
		__m128i* pv = pvHLoad;

		__m128i vMaxColumn = vZero; /* vMaxColumn is used to record the max values of column i. */

		__m128i* vP = vProfile + ref[i] * segLen; /* Right part of the vProfile */
		pvHLoad = pvHStore;
		pvHStore = pv;

		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); j ++) {
			vH = _mm_adds_epi16(vH, _mm_load_si128(vP + j));

			/* Get max from vH, vE and vF. */
			e = _mm_load_si128(pvE + j);
			vH = _mm_max_epi16(vH, e);
			vH = _mm_max_epi16(vH, vF);
			vMaxColumn = _mm_max_epi16(vMaxColumn, vH);

			/* Save vH values. */
			_mm_store_si128(pvHStore + j, vH);

			/* Update vE value. */
			vH = _mm_subs_epu16(vH, vGapO); /* saturation arithmetic, result >= 0 */
			e = _mm_subs_epu16(e, vGapE);
			e = _mm_max_epi16(e, vH);
			_mm_store_si128(pvE + j, e);

			/* Update vF value. */
			vF = _mm_subs_epu16(vF, vGapE);
			vF = _mm_max_epi16(vF, vH);

			/* Load the next vH. */
			vH = _mm_load_si128(pvHLoad + j);
		}

		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
		for (k = 0; LIKELY(k < 8); ++k) {
			vF = _mm_slli_si128 (vF, 2);
			for (j = 0; LIKELY(j < segLen); ++j) {
				vH = _mm_load_si128(pvHStore + j);
				vH = _mm_max_epi16(vH, vF);
				_mm_store_si128(pvHStore + j, vH);
				vH = _mm_subs_epu16(vH, vGapO);
				vF = _mm_subs_epu16(vF, vGapE);
				if (UNLIKELY(! _mm_movemask_epi8(_mm_cmpgt_epi16(vF, vH)))) goto end;
			}
		}

end:
		vMaxScore = _mm_max_epi16(vMaxScore, vMaxColumn);
		vTemp = _mm_cmpeq_epi16(vMaxMark, vMaxScore);
		cmp = _mm_movemask_epi8(vTemp);
		if (cmp != 0xffff) {
			uint16_t temp;
			vMaxMark = vMaxScore;
			m128i_max8(temp, vMaxScore);
			vMaxScore = vMaxMark;

			if (LIKELY(temp > max)) {
				max = temp;
				end_ref = i;
				for (j = 0; LIKELY(j < segLen); ++j) pvHmax[j] = pvHStore[j];
			}
		}

        /* save current column */
        for (j = 0; LIKELY(j < segLen); ++j) {
            uint16_t* t;
            int32_t ti;
            vH = pvHStore[j];
            for (t = (uint16_t*)&vH, ti = 0; ti < 8; ++ti) {
                //fprintf(stdout, "%d\t", *t++);
                ((uint16_t*)mH)[i*readLen + ti*segLen + j] = *t++;
            }
            //fprintf(stdout, "\n");
        }

		/* Record the max score of current column. */
		//max8(maxColumn[i], vMaxColumn);
		//if (maxColumn[i] == terminate) break;

	}

    memcpy(alignment->seed.pvE,      pvE,      segLen*sizeof(__m128i));
    memcpy(alignment->seed.pvHStore, pvHStore, segLen*sizeof(__m128i));


	/* Trace the alignment ending position on read. */
	uint16_t *t = (uint16_t*)pvHmax;
	int32_t column_len = segLen * 8;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		int32_t temp;
		if (*t == max) {
			temp = i / 8 + i % 8 * segLen;
			if (temp < end_read) end_read = temp;
		}
	}

	free(pvE);
	free(pvHmax);
	free(pvHLoad);
    free(pvHStore);

	/* Find the most possible 2nd best alignment. */
	alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max;
	bests[0].ref = end_ref;
	bests[0].read = end_read;

	return bests;
}

int8_t* seq_reverse(const int8_t* seq, int32_t end)	/* end is 0-based alignment ending position */
{
	int8_t* reverse = (int8_t*)calloc(end + 1, sizeof(int8_t));
	int32_t start = 0;
	while (LIKELY(start <= end)) {
		reverse[start] = seq[end];
		reverse[end] = seq[start];
		++ start;
		-- end;
	}
	return reverse;
}

s_profile* ssw_init (const int8_t* read, const int32_t readLen, const int8_t* mat, const int32_t n, const int8_t score_size) {
	s_profile* p = (s_profile*)calloc(1, sizeof(struct _profile));
	p->profile_byte = 0;
	p->profile_word = 0;
	p->bias = 0;

	if (score_size == 0 || score_size == 2) {
		/* Find the bias to use in the substitution matrix */
		int32_t bias = 0, i;
		for (i = 0; i < n*n; i++) if (mat[i] < bias) bias = mat[i];
		bias = abs(bias);

		p->bias = bias;
		p->profile_byte = qP_byte (read, mat, readLen, n, bias);
	}
	if (score_size == 1 || score_size == 2) p->profile_word = qP_word (read, mat, readLen, n);
	p->read = read;
	p->mat = mat;
	p->readLen = readLen;
	p->n = n;
	return p;
}

void init_destroy (s_profile* p) {
	free(p->profile_byte);
	free(p->profile_word);
	free(p);
}

s_align* ssw_fill (const s_profile* prof,
                   const int8_t* ref,
                   const int32_t refLen,
                   const uint8_t weight_gapO,
                   const uint8_t weight_gapE,
                   const int32_t maskLen,
                   s_seed* seed) {

	alignment_end* bests = 0;
	int32_t readLen = prof->readLen;
    s_align* alignment = align_create();

	if (maskLen < 15) {
		fprintf(stderr, "When maskLen < 15, the function ssw_align doesn't return 2nd best alignment information.\n");
	}

	// Find the alignment scores and ending positions
	if (prof->profile_byte) {

		bests = sw_sse2_byte(ref, 0, refLen, readLen, weight_gapO, weight_gapE, prof->profile_byte, -1, prof->bias, maskLen,
                             alignment, seed);

		if (prof->profile_word && bests[0].score == 255) {
			free(bests);
            align_clear_matrix_and_seed(alignment);
            bests = sw_sse2_word(ref, 0, refLen, readLen, weight_gapO, weight_gapE, prof->profile_byte, -1, maskLen,
                                 alignment, seed);
        } else if (bests[0].score == 255) {
			fprintf(stderr, "Please set 2 to the score_size parameter of the function ssw_init, otherwise the alignment results will be incorrect.\n");
			return 0;
		}
	} else if (prof->profile_word) {
		bests = sw_sse2_word(ref, 0, refLen, readLen, weight_gapO, weight_gapE, prof->profile_word, -1, maskLen,
                             alignment, seed);
    } else {
		fprintf(stderr, "Please call the function ssw_init before ssw_align.\n");
		return 0;
	}
	alignment->score1 = bests[0].score;
	alignment->ref_end1 = bests[0].ref;
	alignment->read_end1 = bests[0].read;
	if (maskLen >= 15) {
		alignment->score2 = bests[1].score;
		alignment->ref_end2 = bests[1].ref;
	} else {
	    alignment->score2 = 0;
		alignment->ref_end2 = -1;
	}
	free(bests);

	return alignment;
}

s_align* align_create (void) {
    s_align* a = (s_align*)calloc(1, sizeof(s_align));
    a->seed.pvHStore = NULL;
    a->seed.pvE = NULL;
    a->mH = NULL;
	a->ref_begin1 = -1;
	a->read_begin1 = -1;
    return a;
}

void align_destroy (s_align* a) {
    align_clear_matrix_and_seed(a);
	free(a);
}

void align_clear_matrix_and_seed (s_align* a) {
    free(a->mH);
    a->mH = NULL;
    free(a->seed.pvHStore);
    a->seed.pvHStore = NULL;
    free(a->seed.pvE);
    a->seed.pvE = NULL;
}

void print_score_matrix (char* ref, int32_t refLen, char* read, int32_t readLen, s_align* alignment) {

    int32_t i, j;

    fprintf(stdout, "\t");
    for (i = 0; LIKELY(i < refLen); ++i) {
        fprintf(stdout, "%c\t\t", ref[i]);
    }
    fprintf(stdout, "\n");

    if (is_byte(alignment)) {
        uint8_t* mH = alignment->mH;
        for (j = 0; LIKELY(j < readLen); ++j) {
            fprintf(stdout, "%c\t", read[j]);
            for (i = 0; LIKELY(i < refLen); ++i) {
                fprintf(stdout, "(%u, %u) %u\t", i, j,
                        ((uint8_t*)mH)[i*readLen + j]);
            }
            fprintf(stdout, "\n");
        }
    } else {
        uint16_t* mH = alignment->mH;
        for (j = 0; LIKELY(j < readLen); ++j) {
            fprintf(stdout, "%c\t", read[j]);
            for (i = 0; LIKELY(i < refLen); ++i) {
                fprintf(stdout, "(%u, %u) %u\t", i, j,
                        ((uint16_t*)mH)[i*readLen + j]);
            }
            fprintf(stdout, "\n");
        }
    }

    fprintf(stdout, "\n");

}

void graph_print_score_matrices(graph* graph, char* read, int32_t readLen) {
    uint32_t i = 0, gs = graph->size;
    node** npp = graph->nodes;
    for (i=0; i<gs; ++i, ++npp) {
        node* n = *npp;
        fprintf(stdout, "node %u\n", n->id);
        print_score_matrix(n->seq, n->len, read, readLen, n->alignment);
    }
}

inline int is_byte (s_align* alignment) {
    if (alignment->is_byte) {
        return 1;
    } else {
        return 0;
    }
}

cigar* alignment_trace_back (s_align* alignment,
                             uint16_t* score,
                             int32_t* refEnd,
                             int32_t* readEnd,
                             char* ref,
                             int32_t refLen,
                             char* read,
                             int32_t readLen,
                             int32_t match,
                             int32_t mismatch,
                             int32_t gap_open,
                             int32_t gap_extension) {
    if (LIKELY(is_byte(alignment))) {
        return alignment_trace_back_byte(alignment,
                                         score,
                                         refEnd,
                                         readEnd,
                                         ref,
                                         refLen,
                                         read,
                                         readLen,
                                         match,
                                         mismatch,
                                         gap_open,
                                         gap_extension);
    } else {
        return alignment_trace_back_word(alignment,
                                         score,
                                         refEnd,
                                         readEnd,
                                         ref,
                                         refLen,
                                         read,
                                         readLen,
                                         match,
                                         mismatch,
                                         gap_open,
                                         gap_extension);
    }
}

cigar* alignment_trace_back_byte (s_align* alignment,
                                  uint16_t* score,
                                  int32_t* refEnd,
                                  int32_t* readEnd,
                                  char* ref,
                                  int32_t refLen,
                                  char* read,
                                  int32_t readLen,
                                  int32_t match,
                                  int32_t mismatch,
                                  int32_t gap_open,
                                  int32_t gap_extension) {

    uint8_t* mH = (uint8_t*)alignment->mH;
    int32_t i = *refEnd;
    int32_t j = *readEnd;
    // find maximum
    uint8_t h = mH[readLen*i + j];
	cigar* result = (cigar*)calloc(1, sizeof(cigar));
    result->length = 0;

    while (LIKELY(h != 0 && i >= 0 && j >= 0)) {
        // look at neighbors
        int32_t d = 0, l = 0, u = 0;
        if (i > 0 && j > 0) {
            d = mH[readLen*(i-1) + (j-1)];
        }
        if (i > 0) {
            l = mH[readLen*(i-1) + j];
        }
        if (j > 0) {
            u = mH[readLen*i + (j-1)];
        }
        // get the max of the three directions
        int32_t n = (l > u ? l : u);
        n = (h > n ? h : n);
        if (h == n &&
            ((d + match == h && ref[i] == read[j])
             || (d - mismatch == h && ref[i] != read[j]))) {
            add_element(result, 'M', 1);
            h = d;
            --i; --j;
        } else if (l == n && (l - gap_open == h || l - gap_extension == h)) {
            add_element(result, 'D', 1);
            h = l;
            --i;
        } else if (u == n && (u - gap_open == h || u - gap_extension == h)) {
            add_element(result, 'I', 1);
            h = u;
            --j;
        } else {
            // this is not an error if we are tracing back across a graph
            break;
            fprintf(stderr, "traceback error\n");
            fprintf(stderr, "h=%i i=%i j=%i\n", h, i, j);
            fprintf(stderr, "d=%i, u=%i, l=%i\n", d, u, l);
            h = d; --i; --j;
        }
    }

    *score = h;

    reverse_cigar(result);
    *refEnd = i;
    *readEnd = j;
    return result;
}


// copy of the above but for 16 bit ints
// sometimes there are good reasons for C++'s templates... sigh

cigar* alignment_trace_back_word (s_align* alignment,
                                  uint16_t* score,
                                  int32_t* refEnd,
                                  int32_t* readEnd,
                                  char* ref,
                                  int32_t refLen,
                                  char* read,
                                  int32_t readLen,
                                  int32_t match,
                                  int32_t mismatch,
                                  int32_t gap_open,
                                  int32_t gap_extension) {

    uint16_t* mH = (uint16_t*)alignment->mH;
    int32_t i = *refEnd;
    int32_t j = *readEnd;
    // find maximum
    uint16_t h = mH[readLen*i + j];
	cigar* result = (cigar*)calloc(1, sizeof(cigar));
    result->length = 0;

    while (LIKELY(h != 0 && i >= 0 && j >= 0)) {
        // look at neighbors
        int32_t d = 0, l = 0, u = 0;
        if (i > 0 && j > 0) {
            d = mH[readLen*(i-1) + (j-1)];
        }
        if (i > 0) {
            l = mH[readLen*(i-1) + j];
        }
        if (j > 0) {
            u = mH[readLen*i + (j-1)];
        }
        // get the max of the three directions
        int32_t n = (l > u ? l : u);
        n = (h > n ? h : n);
        if (h == n &&
            ((d + match == h && ref[i] == read[j])
             || (d - mismatch == h && ref[i] != read[j]))) {
            add_element(result, 'M', 1);
            h = d;
            --i; --j;
        } else if (l == n && (l - gap_open == h || l - gap_extension == h)) {
            add_element(result, 'D', 1);
            h = l;
            --i;
        } else if (u == n && (u - gap_open == h || u - gap_extension == h)) {
            add_element(result, 'I', 1);
            h = u;
            --j;
        } else {
            // this is not an error if we are tracing back across a graph
            break;
            fprintf(stderr, "traceback error\n");
            fprintf(stderr, "h=%i i=%i j=%i\n", h, i, j);
            fprintf(stderr, "d=%i, u=%i, l=%i\n", d, u, l);
            h = d; --i; --j;
        }
    }

    *score = h;

    reverse_cigar(result);
    *refEnd = i;
    *readEnd = j;
    return result;
}

graph_mapping* graph_mapping_create(void) {
    graph_mapping* m = (graph_mapping*)calloc(1, sizeof(graph_mapping));
    return m;
}

void graph_mapping_destroy(graph_mapping* m) {
    //graph_cigar_destroy(&m->cigar);
    int32_t i;
    graph_cigar* g = &m->cigar;
    for (i = 0; i < g->length; ++i) {
        cigar_destroy(g->elements[i].cigar);
    }
    free(g->elements);
    //free(g);
    free(m);
}

graph_cigar* graph_cigar_create(void) {
    return (graph_cigar*)calloc(1, sizeof(graph_cigar));
}

void graph_cigar_destroy(graph_cigar* g) {
    int32_t i;
    for (i = 0; i < g->length; ++i) {
        cigar_destroy(g->elements[i].cigar);
    }
    free(g->elements);
    free(g);
}

void print_graph_cigar(graph_cigar* g) {
    int32_t i;
    node_cigar* nc = g->elements;
    for (i = 0; i < g->length; ++i, ++nc) {
        fprintf(stdout, "%u[", nc->node->id);
        print_cigar(nc->cigar);
        fprintf(stdout, "]");
    }
    fprintf(stdout, "\n");
}

void print_graph_mapping(graph_mapping* gm) {
    fprintf(stdout, "%i:", gm->position);
    print_graph_cigar(&gm->cigar);
}

void reverse_graph_cigar(graph_cigar* c) {
	graph_cigar* reversed = (graph_cigar*)malloc(sizeof(graph_cigar));
    reversed->length = c->length;
	reversed->elements = (node_cigar*) malloc(c->length * sizeof(node_cigar));
    node_cigar* c1 = c->elements;
    node_cigar* c2 = reversed->elements;
	int32_t s = 0;
	int32_t e = c->length - 1;
	while (LIKELY(s <= e)) {
		c2[s] = c1[e];
		c2[e] = c1[s];
		++ s;
		-- e;
	}
    free(c->elements);
    c->elements = reversed->elements;
    free(reversed);
}

graph_mapping* graph_trace_back (graph* graph,
                                 char* read,
                                 int32_t readLen,
                                 int32_t match,
                                 int32_t mismatch,
                                 int32_t gap_open,
                                 int32_t gap_extension) {

    graph_mapping* gm = graph_mapping_create();
    graph_cigar* gc = &gm->cigar;
    uint32_t GRAPH_CIGAR_ALLOC_SIZE = 10;
    gc->elements = calloc(1, GRAPH_CIGAR_ALLOC_SIZE*sizeof(node_cigar));
    gc->length = 0;

    node* n = graph->max_node;
    if (!n) {
        fprintf(stderr, "Cannot trace back because graph alignment has not been run.\n");
        fprintf(stderr, "You must call graph_fill(...) before tracing back.\n");
        exit(1);
    }
    uint16_t score = n->alignment->score1;
    uint8_t score_is_byte = (score >= 255) ? 0 : 1;
    int32_t refEnd = n->alignment->ref_end1;
    int32_t readEnd = n->alignment->read_end1;

    node_cigar* nc = gc->elements;

    while (score > 0) {
        if (gc->length > 0 && gc->length == GRAPH_CIGAR_ALLOC_SIZE) {
            gc->elements = realloc((void*)gc->elements,
                                   gc->length + GRAPH_CIGAR_ALLOC_SIZE*sizeof(node_cigar));
        }
        nc->cigar = alignment_trace_back (n->alignment,
                                          &score,
                                          &refEnd,
                                          &readEnd,
                                          n->seq,
                                          n->len,
                                          read,
                                          readLen,
                                          match,
                                          mismatch,
                                          gap_open,
                                          gap_extension);
        nc->node = n;
        ++gc->length;
        if (score == 0) {
            break;
        }
        // the read did not complete here
        // check that we are at 0 in reference and > 0 in read
        /*
        if (readEnd == 0 || readEnd != 0) {
            fprintf(stderr, "graph traceback error, at end of read or ref but score not 0\n");
            exit(1);
        }
        */

        // so check its inbound nodes at the given read end position
        int32_t i;
        node* max_prev = NULL;
        uint16_t l = 0, d = 0, max_score = 0;
        uint8_t max_diag = 1;


        // determine direction across edge

        // rationale: we have to check the left and diagonal directions
        // vertical would stay on this node even if we are in the last column

        // note that the loop is split depending on alignment score width...
        // this is done out of paranoia that optimization will not factor two loops into two if there
        // is an if statement with a consistent result inside of each iteration
        if (score_is_byte) {
            for (i = 0; i < n->count_prev; ++i) {
                node* cn = n->prev[i];
                l = ((uint8_t*)cn->alignment->mH)[readLen*(cn->len-1) + readEnd];
                d = ((uint8_t*)cn->alignment->mH)[readLen*(cn->len-1) + (readEnd-1)];
                if (d > max_score) {
                    max_score = d;
                    max_prev = cn;
                    max_diag = 1;
                } else if (l > max_score) {
                    max_score = l;
                    max_prev = cn;
                    max_diag = 0;
                }
            }
        } else {
            for (i = 0; i < n->count_prev; ++i) {
                node* cn = n->prev[i];
                l = ((uint16_t*)cn->alignment->mH)[readLen*(cn->len-1) + readEnd];
                d = ((uint16_t*)cn->alignment->mH)[readLen*(cn->len-1) + (readEnd-1)];
                if (d > max_score) {
                    max_score = d;
                    max_prev = cn;
                    max_diag = 1;
                } else if (l > max_score) {
                    max_score = l;
                    max_prev = cn;
                    max_diag = 0;
                }
            }
        }
    
        // and determine max among possible transitions
        // set node
        // determine traceback direction
        // did the read complete here?
        // go to ending position, look at neighbors across all inbound nodes
        n = max_prev;
        // update ref end repeat
        refEnd = n->len - 1;
        if (max_diag) {
            --readEnd;
            add_element(nc->cigar, 'M', 1);
        } else {
            add_element(nc->cigar, 'D', 1);
        }
        ++nc;

    }

    // 
    reverse_graph_cigar(gc);

    gm->position = refEnd + 1;

    return gm;

}

void add_element(cigar* c, char type, uint32_t length) {
    if (c->length == 0) {
        c->length = 1;
        c->elements = (cigar_element*) malloc(c->length * sizeof(cigar_element));
        c->elements[c->length - 1].type = type;
        c->elements[c->length - 1].length = length;
    } else if (type != c->elements[c->length - 1].type) {
        c->length++;
        // change to not realloc every single freakin time
        // but e.g. on doubling
        c->elements = (cigar_element*) realloc(c->elements, c->length * sizeof(cigar_element));
        c->elements[c->length - 1].type = type;
        c->elements[c->length - 1].length = length;
    } else {
        c->elements[c->length - 1].length += length;
    }
}

void reverse_cigar(cigar* c) {
	cigar* reversed = (cigar*)malloc(sizeof(cigar));
    reversed->length = c->length;
	reversed->elements = (cigar_element*) malloc(c->length * sizeof(cigar_element));
    cigar_element* c1 = c->elements;
    cigar_element* c2 = reversed->elements;
	int32_t s = 0;
	int32_t e = c->length - 1;
	while (LIKELY(s <= e)) {
		c2[s] = c1[e];
		c2[e] = c1[s];
		++ s;
		-- e;
	}
    free(c->elements);
    c->elements = reversed->elements;
    free(reversed);
}

void print_cigar(cigar* c) {
    int i;
    int l = c->length;
    cigar_element* e = c->elements;
    for (i=0; LIKELY(i < l); ++i, ++e) {
        printf("%i%c", e->length, e->type);
    }
}

void cigar_destroy(cigar* c) {
    free(c->elements);
    c->elements = NULL;
    free(c);
}

void seed_destroy(s_seed* s) {
    free(s->pvE);
    s->pvE = NULL;
    free(s->pvHStore);
    s->pvE = NULL;
    free(s);
}

node* node_create(const char* name,
                  const uint32_t id,
                  const char* seq,
                  const int8_t* nt_table,
                  const int8_t* score_matrix) {
    node* n = calloc(1, sizeof(node));
    int32_t len = strlen(seq);
    int32_t namelen = strlen(name);
    n->id = id;
    n->len = len;
    n->seq = (char*)malloc(len);
    strncpy(n->seq, seq, len);
    n->name = (char*)malloc(namelen);
    strncpy(n->name, name, namelen);
    n->num = create_num(seq, len, nt_table);
    n->count_prev = 0; // are these be set == 0 by calloc?
    n->count_next = 0;
    n->alignment = NULL;
    return n;
}

// for reuse of graph through multiple alignments
void node_clear_alignment(node* n) {
    align_destroy(n->alignment);
    n->alignment = NULL;
}

void profile_destroy(s_profile* prof) {
    free(prof->profile_byte);
    free(prof->profile_word);
    free(prof);
}

void node_destroy(node* n) {
    free(n->name);
    free(n->seq);
    free(n->num);
    free(n->prev);
    free(n->next);
    align_destroy(n->alignment);
    free(n);
}

//void node_clear_alignment(node* n) {
//    align_clear_matrix_and_seed(n->alignment);
//}

void node_add_prev(node* n, node* m) {
    ++n->count_prev;
    n->prev = (node**)realloc(n->prev, n->count_prev*sizeof(node*));
    n->prev[n->count_prev -1] = m;
}

void node_add_next(node* n, node* m) {
    ++n->count_next;
    n->next = (node**)realloc(n->next, n->count_next*sizeof(node*));
    n->next[n->count_next -1] = m;
}

void nodes_add_edge(node* n, node* m) {
    node_add_next(n, m);
    node_add_prev(m, n);
}

s_seed* create_seed_byte(int32_t readLen, node** prev, int32_t count) {
    int32_t j = 0, k = 0;
    __m128i vZero = _mm_set1_epi32(0);
	int32_t segLen = (readLen + 15) / 16;
    s_seed* seed = (s_seed*)calloc(1, sizeof(s_seed));
    if (!(!posix_memalign((void**)&seed->pvE,      sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&seed->pvHStore, sizeof(__m128i), segLen*sizeof(__m128i)))) {
        fprintf(stderr, "Could not allocate memory for alignment seed\n"); exit(1);
    }
    memset(seed->pvE,      0, segLen*sizeof(__m128i));
    memset(seed->pvHStore, 0, segLen*sizeof(__m128i));
    // take the max of all inputs
    __m128i pvE = vZero, pvH = vZero, ovE = vZero, ovH = vZero;
    for (j = 0; j < segLen; ++j) {
        pvE = vZero; pvH = vZero;
        for (k = 0; k < count; ++k) {
            ovE = _mm_load_si128(prev[k]->alignment->seed.pvE + j);
            ovH = _mm_load_si128(prev[k]->alignment->seed.pvHStore + j);
            pvE = _mm_max_epu8(pvE, ovE);
            pvH = _mm_max_epu8(pvH, ovH);
        }
        _mm_store_si128(seed->pvHStore + j, pvH);
        _mm_store_si128(seed->pvE + j, pvE);
    }
    return seed;
}

s_seed* create_seed_word(int32_t readLen, node** prev, int32_t count) {
    int32_t j = 0, k = 0;
    __m128i vZero = _mm_set1_epi32(0);
	int32_t segLen = (readLen + 7) / 8;
    s_seed* seed = (s_seed*)calloc(1, sizeof(s_seed));
    if (!(!posix_memalign((void**)&seed->pvE,      sizeof(__m128i), segLen*sizeof(__m128i)) &&
          !posix_memalign((void**)&seed->pvHStore, sizeof(__m128i), segLen*sizeof(__m128i)))) {
        fprintf(stderr, "Could not allocate memory for alignment seed\n"); exit(1);
    }
    memset(seed->pvE,      0, segLen*sizeof(__m128i));
    memset(seed->pvHStore, 0, segLen*sizeof(__m128i));
    // take the max of all inputs
    __m128i pvE = vZero, pvH = vZero, ovE = vZero, ovH = vZero;
    for (j = 0; j < segLen; ++j) {
        pvE = vZero; pvH = vZero;
        for (k = 0; k < count; ++k) {
            ovE = _mm_load_si128(prev[k]->alignment->seed.pvE + j);
            ovH = _mm_load_si128(prev[k]->alignment->seed.pvHStore + j);
            pvE = _mm_max_epu8(pvE, ovE);
            pvH = _mm_max_epu8(pvH, ovH);
        }
        _mm_store_si128(seed->pvHStore + j, pvH);
        _mm_store_si128(seed->pvE + j, pvE);
    }
    return seed;
}


graph*
graph_fill (graph* graph,
            const char* read_seq,
            const int8_t* nt_table,
            const int8_t* score_matrix,
            const uint8_t weight_gapO,
            const uint8_t weight_gapE,
            const int32_t maskLen,
            const int8_t score_size) {

    int32_t read_length = strlen(read_seq);
    int8_t* read_num = create_num(read_seq, read_length, nt_table);
	s_profile* prof = ssw_init(read_num, read_length, score_matrix, 5, score_size);
    s_seed* seed = NULL;
    uint16_t max_score = 0;

    // for each node, from start to finish in the partial order (which should be sorted topologically)
    // generate a seed from input nodes or use existing (e.g. for subgraph traversal here)
    uint32_t i;
    node** npp = &graph->nodes[0];
    for (i = 0; i < graph->size; ++i, ++npp) {
        node* n = *npp;
        // get seed from parents (max of multiple inputs)
        if (prof->profile_byte) {
            seed = create_seed_byte(prof->readLen, n->prev, n->count_prev);
        } else {
            seed = create_seed_word(prof->readLen, n->prev, n->count_prev);
        }
        node* filled_node = node_fill(n, prof, weight_gapO, weight_gapE, maskLen, seed);
        seed_destroy(seed); seed = NULL; // cleanup seed
        // test if we have exceeded the score dynamic range
        if (prof->profile_byte && !filled_node) {
            free(prof->profile_byte);
            prof->profile_byte = NULL;
            // free previous nodes which have 8-bit stripes
            //npp = &graph->nodes[0];
            //i = 1; // reset iteration
            //max_score = 0;
            free(read_num);
            profile_destroy(prof);
            return graph_fill(graph, read_seq, nt_table, score_matrix, weight_gapO, weight_gapE, maskLen, 1);
        } else {
            if (!graph->max_node || n->alignment->score1 > max_score) {
                graph->max_node = n;
                max_score = n->alignment->score1;
            }
        }
    }

    free(read_num);
    profile_destroy(prof);

    return graph;

}

// TODO graph traceback


node*
node_fill (node* node,
           const s_profile* prof,
           const uint8_t weight_gapO,
           const uint8_t weight_gapE,
           const int32_t maskLen,
           const s_seed* seed) {

	alignment_end* bests = NULL;
	int32_t readLen = prof->readLen;

    //alignment_end* best = (alignment_end*)calloc(1, sizeof(alignment_end));
    s_align* alignment = node->alignment;

    if (alignment) {
        // clear old alignment
        align_destroy(alignment);
    }
    // and build up a new one
    node->alignment = alignment = align_create();

    
    // if we have parents, we should generate a new seed as the max of each vector
    // if one of the parents has moved into uint16_t space, we need to account for this
    // otherwise, just use the single parent alignment result as seed
    // or, if no parents, run unseeded

    // to decrease code complexity, we assume the same stripe size for the entire graph
    // this is ensured by changing the stripe size for the entire graph in graph_fill if any node scores >= 255

	// Find the alignment scores and ending positions
	if (prof->profile_byte) {
		bests = sw_sse2_byte((const int8_t*)node->num, 0, node->len, readLen, weight_gapO, weight_gapE, prof->profile_byte, -1, prof->bias, maskLen, alignment, seed);
		if (bests[0].score == 255) {
			free(bests);
            align_clear_matrix_and_seed(alignment);
            return 0; // re-run from external context
		}
	} else if (prof->profile_word) {
        bests = sw_sse2_word((const int8_t*)node->num, 0, node->len, readLen, weight_gapO, weight_gapE, prof->profile_word, -1, maskLen, alignment, seed);
    } else {
		fprintf(stderr, "Please call the function ssw_init before ssw_align.\n");
		return 0;
	}

	alignment->score1 = bests[0].score;
	alignment->ref_end1 = bests[0].ref;
	alignment->read_end1 = bests[0].read;
	if (maskLen >= 15) {
		alignment->score2 = bests[1].score;
		alignment->ref_end2 = bests[1].ref;
	} else {
	    alignment->score2 = 0;
		alignment->ref_end2 = -1;
	}
	free(bests);

	return node;

}

graph* graph_create(uint32_t size) {
    graph* g = calloc(1, sizeof(graph));
    g->nodes = malloc(size*sizeof(node*));
    if (!g || !g->nodes) { fprintf(stderr, "Could not allocate memory for graph of %u nodes.\n", size); exit(1); }
    return g;
}

void graph_clear_alignment(graph* g) {
    g->max_node = NULL;
}

void graph_destroy(graph* g) {
    g->max_node = NULL;
    free(g->nodes);
    g->nodes = NULL;
    free(g);
}

int32_t graph_add_node(graph* graph, node* node) {
    const int32_t graph_realloc_size = 1024;
    if (graph->size % graph_realloc_size == 0) {
        graph->size += graph_realloc_size;
        if (UNLIKELY(!(graph->nodes = realloc((void*)graph->nodes, graph->size * sizeof(void*))))) {
            fprintf(stderr, "could not allocate memory for graph\n"); exit(1);
        }
    }
    graph->nodes[graph->size] = node;
    ++graph->size;
    return graph->size;
}

int8_t* create_num(const char* seq,
                   const int32_t len,
                   const int8_t* nt_table) {
    int32_t m;
    int8_t* num = (int8_t*)malloc(len);
	for (m = 0; m < len; ++m) num[m] = nt_table[(int)seq[m]];
    return num;
}

int8_t* create_score_matrix(int32_t match, int32_t mismatch) {
	// initialize scoring matrix for genome sequences
	//  A  C  G  T	N (or other ambiguous code)
	//  2 -2 -2 -2 	0	A
	// -2  2 -2 -2 	0	C
	// -2 -2  2 -2 	0	G
	// -2 -2 -2  2 	0	T
	//	0  0  0  0  0	N (or other ambiguous code)
    int32_t l, k, m;
	int8_t* mat = (int8_t*)calloc(25, sizeof(int8_t));
	for (l = k = 0; l < 4; ++l) {
		for (m = 0; m < 4; ++m) mat[k++] = l == m ? match : - mismatch;	/* weight_match : -weight_mismatch */
		mat[k++] = 0; // ambiguous base: no penalty
	}
	for (m = 0; m < 5; ++m) mat[k++] = 0;
    return mat;
}

int8_t* create_nt_table(void) {
    int8_t* ret_nt_table = calloc(128, sizeof(int8_t));
    int8_t nt_table[128] = {
		4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
		4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
		4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
		4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
		4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
		4, 4, 4, 4,  3, 0, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,
		4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4,
		4, 4, 4, 4,  3, 0, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4
	};
    memcpy(ret_nt_table, nt_table, 128*sizeof(int8_t));
    return ret_nt_table;
}
