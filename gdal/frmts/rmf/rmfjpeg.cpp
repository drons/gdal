/******************************************************************************
 *
 * Project:  Raster Matrix Format
 * Purpose:  Implementation of the JPEG decompression algorithm as used in
 *           GIS "Panorama" raster files.
 * Author:   Andrew Sudorgin (drons [a] list dot ru)
 *
 ******************************************************************************
 * Copyright (c) 2018, Andrew Sudorgin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#ifdef HAVE_LIBJPEG
#include <algorithm>
#include "cpl_conv.h"

#include "rmfdataset.h"
#include <setjmp.h>
CPL_C_START
#include <jpeglib.h>
#include <jerror.h>
CPL_C_END


static void RMFJPEGMessage(j_common_ptr poInfo, int iMsgLevel)
{
    if(iMsgLevel > 0)
    {
        return;
    }

    poInfo->err->num_warnings++;

    if(poInfo->err->num_warnings > 10)
    {
        return;
    }

    char aszBuffer[JMSG_LENGTH_MAX] = {0};

    poInfo->err->format_message(poInfo, aszBuffer);
    CPLError(CE_Failure, CPLE_AppDefined, "RMF JPEG:%s", aszBuffer);
}

static void RMFJPEGError(j_common_ptr poInfo)
{
    jmp_buf* poJmpBuf = reinterpret_cast<jmp_buf*>(poInfo->client_data);
    char     aszBuffer[JMSG_LENGTH_MAX] = {0};

    poInfo->err->format_message(poInfo, aszBuffer);
    CPLError(CE_Failure, CPLE_AppDefined, "RMF JPEG: %s", aszBuffer);
    longjmp(*poJmpBuf, 1);
}

static void RMFJPEGNoop(j_decompress_ptr)
{
}


/**
*\brief: This function is supposed to do refilling of the input buffer,
* but as we provided everything at the beginning, if it is called, then
* we have an error.
*/
static boolean RMFJPEG_fill_input_buffer_dec(j_decompress_ptr cinfo)
{
    CPLError(CE_Failure, CPLE_AppDefined, "Invalid JPEG stream");
    cinfo->err->msg_code = JERR_INPUT_EMPTY;
    cinfo->err->error_exit((j_common_ptr)(cinfo));
    return FALSE;
}

// Might be called if there is a marker in the codestream
static void RMFJPEG_skip_input_data_dec(j_decompress_ptr cinfo, long l) {
    struct jpeg_source_mgr *src = cinfo->src;
    if (l > 0) {
        if (static_cast<size_t>(l) > src->bytes_in_buffer)
            l = static_cast<long>(src->bytes_in_buffer);
        src->bytes_in_buffer -= l;
        src->next_input_byte += l;
    }
}

/************************************************************************/
/*                          JPEGDecompress()                            */
/************************************************************************/

static int _Decompress(jmp_buf* poJmpBuf,
                       jpeg_decompress_struct* poJpegInfo,
                       GByte*      pabyOut,
                       int         nRawScanLineSize,
                       JDIMENSION  nImageHeight,
                       GByte*      pabyScanline)
{
    if(setjmp(*poJmpBuf))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "RMF JPEG: Error decompress JPEG tile");
        return 0;
    }

    while(poJpegInfo->output_scanline < nImageHeight)
    {
        JSAMPROW    pabyBuffer[1];

        if(pabyScanline)
        {
            pabyBuffer[0] = (JSAMPROW)pabyScanline;
        }
        else
        {
            pabyBuffer[0] = (JSAMPROW)pabyOut +
                            nRawScanLineSize*poJpegInfo->output_scanline;
        }

        if(jpeg_read_scanlines(poJpegInfo, pabyBuffer, 1) == 0)
        {
            return 0;
        }

        if(pabyScanline)
        {
            memcpy(pabyOut + nRawScanLineSize*(poJpegInfo->output_scanline - 1),
                   pabyScanline, nRawScanLineSize);
        }
    }
    jpeg_finish_decompress(poJpegInfo);
    return poJpegInfo->output_scanline*nRawScanLineSize;
}

/************************************************************************/
/*                          JPEGDecompress()                            */
/************************************************************************/

int RMFDataset::JPEGDecompress(const GByte* pabyIn, GUInt32 nSizeIn,
                               GByte* pabyOut, GUInt32 nSizeOut,
                               GUInt32 nRawXSize, GUInt32 nRawYSize)
{
    if(pabyIn == nullptr ||
       pabyOut == nullptr ||
       nSizeOut < nSizeIn ||
       nSizeIn < 2)
       return 0;

    jpeg_decompress_struct  oJpegInfo;
    jpeg_source_mgr         oSrc;
    jpeg_error_mgr          oJpegErr;
    jmp_buf                 oJmpBuf;

    oJpegInfo.err = jpeg_std_error(&oJpegErr);
    oJpegErr.error_exit = RMFJPEGError;
    oJpegErr.emit_message = RMFJPEGMessage;
    oJpegInfo.client_data = reinterpret_cast<void*>(&oJmpBuf);

    memset(&oSrc, 0, sizeof(jpeg_source_mgr));

    oSrc.next_input_byte = (JOCTET *)pabyIn;
    oSrc.bytes_in_buffer = (size_t)nSizeIn;
    oSrc.term_source = RMFJPEGNoop;
    oSrc.init_source = RMFJPEGNoop;
    oSrc.fill_input_buffer = RMFJPEG_fill_input_buffer_dec;
    oSrc.skip_input_data = RMFJPEG_skip_input_data_dec;

    jpeg_create_decompress(&oJpegInfo);

    memset(&oJmpBuf, 0, sizeof(jmp_buf));

    if(setjmp(oJmpBuf))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "RMF JPEG: Error decompress JPEG tile");
        jpeg_destroy_decompress(&oJpegInfo);
        return 0;
    }

    oJpegInfo.src = &oSrc;
    jpeg_read_header(&oJpegInfo, TRUE);

    if(oJpegInfo.num_components != RMF_JPEG_BAND_COUNT)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "RMF JPEG: Invalid num_components %d in tile, must be %d",
                 (int)oJpegInfo.num_components, (int)RMF_JPEG_BAND_COUNT);
        jpeg_destroy_decompress(&oJpegInfo);
        return 0;
    }

    oJpegInfo.dct_method = JDCT_FLOAT;
    oJpegInfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&oJpegInfo);

    JDIMENSION  nImageHeight = std::min(oJpegInfo.image_height,
                                        (JDIMENSION)nRawYSize);
    int         nRawScanLineSize = nRawXSize *
                                   oJpegInfo.num_components;
    GByte*      pabyScanline = nullptr;

    if((JDIMENSION)nRawXSize < oJpegInfo.image_width)
    {
        pabyScanline = reinterpret_cast<GByte *>(
                VSIMalloc(oJpegInfo.num_components*
                          oJpegInfo.image_width));
        if(!pabyScanline)
        {
            CPLError( CE_Failure, CPLE_OutOfMemory,
                      "Can't allocate scanline buffer %d.",
                      (int)oJpegInfo.num_components*
                      oJpegInfo.image_width);
            jpeg_destroy_decompress(&oJpegInfo);
            return 0;
        }
    }

    int nRet = _Decompress(&oJmpBuf, &oJpegInfo,
                          pabyOut,
                          nRawScanLineSize, nImageHeight,
                          pabyScanline);

    jpeg_destroy_decompress(&oJpegInfo);
    VSIFree(pabyScanline);

    return nRet;
}

#endif //HAVE_LIBJPEG
