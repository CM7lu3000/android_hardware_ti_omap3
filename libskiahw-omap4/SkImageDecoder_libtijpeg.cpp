
/*
 * Copyright (C) Texas Instruments - http://www.ti.com/
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
/* ==============================================================================
*             Texas Instruments OMAP (TM) Platform Software
*  (c) Copyright Texas Instruments, Incorporated.  All Rights Reserved.
*
*  Use of this software is controlled by the terms and conditions found
*  in the license agreement under which this software has been supplied.
* ============================================================================ */
/**
* @file SkImageDecoder_libtijpeg.cpp
*
* This file implements SkTIJPEGImageDecoder
*
*/

#include "SkImageDecoder_libtijpeg.h"

#include <timm_osal_error.h>
#include <timm_osal_memory.h>
#include <unistd.h>


#define LOG_TAG "LIBSKIAHW"


#define TIME_DECODE
#define JPEG_DECODER_DUMP_INPUT_AND_OUTPUT 0 // set directory persmissions for /temp as 777
#define JPEGD_Trace(ARGS,...)  TIMM_OSAL_InfoExt(TIMM_OSAL_TRACEGRP_OMXIMGDEC,ARGS,##__VA_ARGS__) //PRINT OMAP4
#if JPEG_DECODER_DUMP_INPUT_AND_OUTPUT
    int dOutputCount = 0;
    int dInputCount = 0;
#endif

// Definitons used by TIMM_OSAL PIPEs, this is in test/////
TIMM_OSAL_U32 actualSize = 0;
TIMM_OSAL_ERRORTYPE bReturnStatus = TIMM_OSAL_ERR_NONE;
/* Data pipes for both the ports */
TIMM_OSAL_PTR dataPipes[OMX_JPEGD_TEST_NUM_PORTS];
//////////////////////////////////////////////////////////////////////////

OMX_ERRORTYPE OMX_EventHandler(OMX_HANDLETYPE hComponent,
                                    OMX_PTR pAppData,
                                    OMX_EVENTTYPE eEvent,
                                    OMX_U32 nData1,
                                    OMX_U32 nData2,
                                    OMX_PTR pEventData)
    {
    LOG_FUNCTION_NAME

    ((SkTIJPEGImageDecoder *)pAppData)->EventHandler(hComponent, eEvent, nData1, nData2, pEventData);

    LOG_FUNCTION_NAME_EXIT

    return OMX_ErrorNone;
    }


OMX_ERRORTYPE OMX_EmptyBufferDone(OMX_HANDLETYPE hComponent, OMX_PTR ptr, OMX_BUFFERHEADERTYPE* pBuffer)
    {
    LOG_FUNCTION_NAME

    SkTIJPEGImageDecoder * ImgDec = (SkTIJPEGImageDecoder *)ptr;
    ImgDec->iLastState = ImgDec->iState;
    ImgDec->iState = SkTIJPEGImageDecoder::STATE_EMPTY_BUFFER_DONE_CALLED;
    sem_post(ImgDec->semaphore) ;

    LOG_FUNCTION_NAME_EXIT

    return OMX_ErrorNone;
    }


OMX_ERRORTYPE OMX_FillBufferDone (OMX_HANDLETYPE hComponent, OMX_PTR ptr, OMX_BUFFERHEADERTYPE* pBuffHead)
    {
    LOG_FUNCTION_NAME

    LIBSKIAHW_LOGDB("\nOMX_FillBufferDone:: nFilledLen = %ld \n", pBuffHead->nFilledLen);

    if(pBuffHead->nAllocLen == 0)
        {
        LIBSKIAHW_LOGDA("[ZERO length]");
        }

    if(pBuffHead->nFlags & OMX_BUFFERFLAG_EOS)
        {
        pBuffHead->nFlags &= ~OMX_BUFFERFLAG_EOS;
        LIBSKIAHW_LOGDA("[EOS detected]");
        }

    ((SkTIJPEGImageDecoder *)ptr)->FillBufferDone(pBuffHead->pBuffer, pBuffHead->nFilledLen);

    LOG_FUNCTION_NAME_EXIT

    return OMX_ErrorNone;
    }


SkTIJPEGImageDecoder::~SkTIJPEGImageDecoder()
    {
    LOG_FUNCTION_NAME
    OMX_ERRORTYPE eError = OMX_ErrorNone;

    AutoTimeMillis atm("DeInit time: ");

    sem_destroy(semaphore);

    if (semaphore != NULL)
        {
        free(semaphore) ;
        semaphore = NULL;
        }
    /*### Assume that the inStream will be closed by some upper layer */
    /*### Assume that the Bitmap object/file need not be closed. */
    /*### More needs to be done here */
    /*### Do different things based on iLastState */
    LIBSKIAHW_LOGDA("Calling FREEHANDLE\n");
    if (pOMXHandle) {
        eError = OMX_FreeHandle(pOMXHandle);
        if ( (eError != OMX_ErrorNone) )    {
            LIBSKIAHW_LOGEA("Error in Free Handle function\n");
        }
        pOMXHandle = NULL;
        OMX_Deinit();
    }

    if (pARMHandle) {
        delete pARMHandle;
        pARMHandle=NULL;
        }

    if(JpegHeaderInfo.MPIndexIFDTags.MPEntry)
        delete[] JpegHeaderInfo.MPIndexIFDTags.MPEntry;

    LOG_FUNCTION_NAME_EXIT
    }

SkTIJPEGImageDecoder::SkTIJPEGImageDecoder()
    {
    LOG_FUNCTION_NAME

    pInBuffHead = NULL;
    pOutBuffHead = NULL;
    semaphore = NULL;
    semaphore = (sem_t*)malloc(sizeof(sem_t)) ;
    sem_init(semaphore, 0x00, 0x00);

    pOMXHandle = NULL;
    pARMHandle = NULL;
    pBeforeDecodeTime = NULL;
    pDecodeTime = NULL;
    pAfterDecodeTime = NULL;

    JpegHeaderInfo.MPIndexIFDTags.MPEntry = NULL;
    fileType = TYPE_JPG;
    finalBytesRead = 0;

    LIBSKIAHW_LOGDB("semaphore created semaphore = 0x%x", semaphore);

    LIBSKIAHW_LOGDB("Process ID using this instance 0x%x", getpid());

    LOG_FUNCTION_NAME_EXIT
    }


OMX_S16 SkTIJPEGImageDecoder::Get16m(const void * Short)
    {
    return(((OMX_U8 *)Short)[0] << 8) | ((OMX_U8 *)Short)[1];
    }

void SkTIJPEGImageDecoder::FixFrameSize(JPEG_HEADER_INFO* JpegHeaderInfo)
    {
    LOG_FUNCTION_NAME

    OMX_U32 nWidth=JpegHeaderInfo->nWidth, nHeight=JpegHeaderInfo->nHeight;

    /*round up if nWidth is not multiple of 32*/
    ( (nWidth%32 ) !=0 ) ?  nWidth=32 * (  (  nWidth/32 ) + 1 )  : nWidth;

    /*round up if nHeight is not multiple of 16*/
    ( (nHeight%16) !=0 ) ?  nHeight=16 * (  (  nHeight/16 ) + 1 )  : nHeight;

    JpegHeaderInfo->nWidth = nWidth;
    JpegHeaderInfo->nHeight = nHeight;

    LOG_FUNCTION_NAME_EXIT
    }


OMX_S16 SkTIJPEGImageDecoder::GetYUVformat(OMX_U8 * Data)
    {
    LOG_FUNCTION_NAME

    unsigned char Nf;
    OMX_U32 j;
    OMX_U32 temp_index;
    OMX_U32 temp;
    OMX_U32 image_format;
    short H[4],V[4];

    Nf = Data[7];

    for (j = 0; j < Nf; j++)
        {
       temp_index = j * 3 + 7 + 2;
        /*---------------------------------------------------------*/
       /* H[j]: upper 4 bits of a byte, horizontal sampling fator.                  */
       /* V[j]: lower 4 bits of a byte, vertical sampling factor.                    */
       /*---------------------------------------------------------*/
         H[j] = (0x0f & (Data[temp_index] >> 4));
         V[j] = (0x0f & Data[temp_index]);
        }

    /*------------------------------------------------------------------*/
    /* Set grayscale flag, namely if image is gray then set it to 1,    */
    /* else set it to 0.                                                */
    /*------------------------------------------------------------------*/
    image_format = -1;

    if (Nf == 1)
        {
        image_format = OMX_COLOR_FormatL8;
        }

    if (Nf == 3)
        {
        temp = (V[0]*H[0])/(V[1]*H[1]) ;

        if (temp == 4 && H[0] == 2)
            {
            image_format = OMX_COLOR_FormatYUV420Planar;
            }

        if (temp == 4 && H[0] == 4)
            {
            image_format = OMX_COLOR_FormatYUV411Planar;
            }

        if (temp == 2)
            {
            image_format = OMX_COLOR_FormatCbYCrY; /* YUV422 interleaved, little endian */
            }

        if (temp == 1)
            {
            image_format = OMX_COLOR_FormatYUV444Interleaved;
            }
        }

    LOG_FUNCTION_NAME_EXIT
    return (image_format);
    }

/* BigEndianRead32Bits() gets a 32-bit value into a pointer variable */
void SkTIJPEGImageDecoder::BigEndianRead32Bits(OMX_U32 *pVal,OMX_U8* ptr)
{
    *pVal = (OMX_U32)((*(ptr+3)));
    *pVal |= (OMX_U32)((*(ptr+2)) <<8);
    *pVal |= (OMX_U32)((*(ptr+1)) <<16);
    *pVal |= (OMX_U32)((*(ptr)) <<24);
}

/* BigEndianRead16Bits() gets a 16-bit value into a pointer variable  */
void SkTIJPEGImageDecoder::BigEndianRead16Bits(OMX_U16 *pVal,OMX_U8* ptr)
{
    *pVal = *ptr;
    *pVal <<=8;
    *pVal |= *(ptr+1);
}

OMX_S32 SkTIJPEGImageDecoder::ParseJpegHeader (SkStream* stream, JPEG_HEADER_INFO* JpgHdrInfo)
    {
    LOG_FUNCTION_NAME

    OMX_U8 a = 0;
    OMX_S32 lSize = 0;
    lSize = stream->getLength();
    stream->rewind();

    OMX_U8 *Data = NULL;
    size_t bytesRead=0;
    JpgHdrInfo->nProgressive = 0; /*Default value is non progressive*/
    OMX_U32 littleEndian = 0, skipFactor = 0, nextIFDOffset = 0, nextOffset = 0, tagValOffset = 0;
    OMX_U32 tagCount, tagValue;
    OMX_U8 *pOffsetRef;
    OMX_U8 *pOffsetVal;
    OMX_U16 MPIndexCount, tagID, tagType;

    a = stream->readU8();bytesRead++;
    if ( a != 0xff || stream->readU8() != M_SOI )
        {
        return 0;
        }
    bytesRead++;

    for ( ;; )
        {
        OMX_U32 itemlen = 0;
        OMX_U32 marker = 0;
        OMX_U32 ll = 0,lh = 0, got = 0;

        for ( a=0;a<15 /* 7 originally */;a++ )
            {
            marker = stream->readU8();
            bytesRead++;
            if ( marker != 0xff )
                {
                break;
                }
            if ( a >= 14 /* 6 originally */)
                {
                LIBSKIAHW_LOGDA("too many padding bytes\n");
                if ( Data != NULL )
                    {
                    free(Data);
                    Data=NULL;
                    }
                return 0;
                }
            }

        if ( marker == 0xff )
            {
            /* 0xff is legal padding, but if we get that many, something's wrong.*/
            LIBSKIAHW_LOGEA("too many padding bytes!");
            }

        /* Read the length of the section.*/
        lh = stream->readU8();
        ll = stream->readU8();
        bytesRead+=2;
        itemlen = (lh << 8) | ll;

        if ( itemlen < 2 )
            {
            LIBSKIAHW_LOGEA("invalid marker");
            }

        Data = (OMX_U8 *)malloc(itemlen);
        if ( Data == NULL )
            {
            LIBSKIAHW_LOGEA("Could not allocate memory");
            break;
            }

        /* Store first two pre-read bytes. */
        Data[0] = (OMX_U8)lh;
        Data[1] = (OMX_U8)ll;

        got = stream->read(Data+2, itemlen-2); /* Read the whole section.*/
        bytesRead +=got;

        if ( got != itemlen-2 )
            {
            LIBSKIAHW_LOGEA("Premature end of file?");
            if ( Data != NULL )
                {
                free(Data);
                Data=NULL;
                }
            return 0;
            }

        switch ( marker )
            {
            case M_SOS:
                    {
                        if ( Data != NULL )
                        {
                        free(Data);
                        Data=NULL;
                        }
                    return lSize;
                }

            case M_EOI:
                    {
                    LIBSKIAHW_LOGDA("No image in jpeg!\n");
                    if ( Data != NULL )
                        {
                        free(Data);
                        Data=NULL;
                        }
                    return 0;
                }

            case M_COM: /* Comment section  */
                    {
                        break;
                }

            case M_JFIF:
                    {
                        break;
                }

            case M_EXIF:
                    {
                        break;
                }

            case M_SOF2:
                    {
                    LIBSKIAHW_LOGDA("nProgressive IMAGE!\n");
                    JpgHdrInfo->nProgressive=1;
                    break;
                }

            case M_SOF0:
            case M_SOF1:
            case M_SOF3:
            case M_SOF5:
            case M_SOF6:
            case M_SOF7:
            case M_SOF9:
            case M_SOF10:
            case M_SOF11:
            case M_SOF13:
            case M_SOF14:
            case M_SOF15:
                    {
                        JpgHdrInfo->nHeight = Get16m(Data+3);
                        JpgHdrInfo->nWidth = Get16m(Data+5);
                        JpgHdrInfo->nFormat = GetYUVformat(Data);
                        switch (JpgHdrInfo->nFormat) {
                        case OMX_COLOR_FormatYUV420Planar:
                            LIBSKIAHW_LOGDA("Image chroma format is OMX_COLOR_FormatYUV420Planar\n");
                            break;
                        case OMX_COLOR_FormatYUV411Planar:
                            LIBSKIAHW_LOGDA("Image chroma format is OMX_COLOR_FormatYUV411Planar\n");
                            break;
                        case OMX_COLOR_FormatCbYCrY:
                            LIBSKIAHW_LOGDA("Image chroma format is OMX_COLOR_FormatYUV422Interleaved\n");
                            break;
                        case OMX_COLOR_FormatYUV444Interleaved:
                            LIBSKIAHW_LOGDA("Image chroma format is OMX_COLOR_FormatYUV444Interleaved\n");
                            break;
                        case OMX_COLOR_FormatL8:
                            LIBSKIAHW_LOGDA("Image chroma format is OMX_COLOR_FormatL8 \n");
                            break;
                        default:
                            LIBSKIAHW_LOGDA("Cannot find Image chroma format \n");
                            JpgHdrInfo->nFormat = OMX_COLOR_FormatUnused;
                            break;
                        }

                        LIBSKIAHW_LOGDB("Image Width x Height = %u * %u\n", Get16m(Data+5), Get16m(Data+3)  );

                        if ( Data != NULL ) {
                            free(Data);
                            Data=NULL;
                        }
                        break;
                }
            case M_JPS:
                    {
                        if((*(Data+2) == 0x5F && *(Data+3) == 0x4A && *(Data+4) == 0x50 && *(Data+5) == 0x53 && *(Data+6) == 0x4A && *(Data+7) == 0x50 && *(Data+8) == 0x53 && *(Data+9) == 0x5F)
                            && (0x01 == (OMX_U8) *(Data+JPS_TYPE_OFFSET)))
                        {
                            LIBSKIAHW_LOGDA("Valid Stereo JPS type file \n");
                             fileType = TYPE_JPS;

                            JpgHdrInfo->s3dDesc.nType = (OMX_U8) *(Data+JPS_TYPE_OFFSET);
                            JpgHdrInfo->s3dDesc.nLayout = (OMX_U8) *(Data+JPS_LAYOUT_OFFSET);

                            // Setting the frame order
                            if((OMX_U8) *(Data+JPS_MISCF_OFFSET) & JPS_MISCF_FO_MASK)
                                JpgHdrInfo->s3dDesc.nFrameOrder = S3D_ORDER_LF;
                            else
                                JpgHdrInfo->s3dDesc.nFrameOrder = S3D_ORDER_RF;

                            switch((OMX_U8) *(Data+JPS_MISCF_OFFSET) & JPS_MISCF_SS_MASK)
                            {
                                    case S3D_SS_NONE:
                                            //LIBSKIAHW_LOGDA("No SubSampling \n");
                                            JpgHdrInfo->s3dDesc.nSubSampling = S3D_SS_NONE;
                                            break;
                                    case S3D_SS_VERT:
                                            //LIBSKIAHW_LOGDA("VERT SubSampling \n");
                                            JpgHdrInfo->s3dDesc.nSubSampling = S3D_SS_VERT;
                                            break;
                                    case S3D_SS_HORZ:
                                            //LIBSKIAHW_LOGDA("HORZ SubSampling \n");
                                            JpgHdrInfo->s3dDesc.nSubSampling = S3D_SS_HORZ;
                                            break;
                                    case S3D_SS_BOTH:
                                           // LIBSKIAHW_LOGDA("HORZ and VERT SubSampling \n");
                                            JpgHdrInfo->s3dDesc.nSubSampling = S3D_SS_BOTH;
                                            break;
                            }

                            // Separation field , read it even if Don't care
                            JpgHdrInfo->s3dDesc.nSeparation = (OMX_U8) *(Data+JPS_SEP_OFFSET);

                            //LIBSKIAHW_LOGDA("ntype=0x%x, layout=0x%x, miscflag=0x%x, separation=0x%x \n", (OMX_U8) *(Data+JPS_TYPE_OFFSET), (OMX_U8) *(Data+JPS_LAYOUT_OFFSET) , (OMX_U8) *(Data+JPS_MISCF_OFFSET) , (OMX_U8) *(Data+JPS_SEP_OFFSET));
                       }
                       else
                            {
                                LIBSKIAHW_LOGDA("Non-Supported or invalid JPS file  \n");
                                if ( Data != NULL ) {
                                    free(Data);
                                    Data=NULL;
                                }
                                return 0;
                            }
                    break;
                    }
            case M_MPO:
                    {
                        if(*(Data+2) == 0x4D && *(Data+3) == 0x50 && *(Data+4) == 0x46 && *(Data+5) == 0x00 )
                        {
                            LIBSKIAHW_LOGDA("Valid MPO type file \n");
                            fileType = TYPE_MPO;
                            pOffsetRef = Data + MP_START_OF_OFFSET_REF; // Start of reference
                            finalBytesRead  =bytesRead + 4 - got;// reset and add MP Format ID

                            if(*(pOffsetRef) == 0x49 && *(pOffsetRef+1) == 0x49 /*&& *(pOffsetRef+2) == 0x2A && *(pOffsetRef+3) == 0x00 */)
                            {
                                //LIBSKIAHW_LOGDA("Little Endian \n");
                                littleEndian = 1;
                            }
                            Data += 0x0A;//Skipping to read the offset
                            // Read 0th IFD offset
                            if(littleEndian)
                                skipFactor = (OMX_U32) (*Data);
                            else
                                BigEndianRead32Bits(&skipFactor,Data);

                            Data = pOffsetRef + skipFactor; // Jump to 0th IFD

                            // Reset the offset value
                            nextIFDOffset = 0;
                            tagValue = 0;

                             //Default parameters for now
                            JpgHdrInfo->s3dDesc.nType = 0x01; // STEROSCOPIC IMAGES
                            JpgHdrInfo->s3dDesc.nLayout = S3D_FORMAT_OVERUNDER;
                            JpgHdrInfo->s3dDesc.nFrameOrder = S3D_ORDER_LF;
                            JpgHdrInfo->s3dDesc.nSubSampling = S3D_SS_NONE;
                            JpgHdrInfo->s3dDesc.nSeparation = NULL;

                            do
                            {
                                Data += nextIFDOffset;
                                //nextIFDOffset = 0;

                                if(littleEndian)
                                {
                                    MPIndexCount = (OMX_U16)((*Data) + (*(Data+1)<<8));
                                }
                                else
                                {
                                    BigEndianRead16Bits(&MPIndexCount,Data);
                                }
                                Data += sizeof(OMX_U16);

                                for(int j=0;j<MPIndexCount;j++)
                                {
                                    if(littleEndian)
                                    {
                                        tagID = (OMX_U16)((*Data) + (*(Data+1)<<8));
                                        Data += sizeof(OMX_U16);

                                        tagType = (OMX_U16)((*Data) + (*(Data+1)<<8));
                                        Data += sizeof(OMX_U16);

                                        tagCount = (OMX_U32)((*Data) + (*(Data+1)<<8) +
                                                              (*(Data+2)<<16) + (*(Data+3)<<24));
                                        Data += sizeof(OMX_U32);

                                        tagValue = (OMX_U32)((*Data) + (*(Data+1)<<8) +
                                                              (*(Data+2)<<16) + (*(Data+3)<<24));
                                        Data += sizeof(OMX_U32);

                                    }
                                    else
                                    {
                                        BigEndianRead16Bits(&tagID,Data);
                                        Data += sizeof(OMX_U16);

                                        BigEndianRead16Bits(&tagType,Data);
                                        Data += sizeof(OMX_U16);

                                        BigEndianRead32Bits(&tagCount,Data);
                                        Data += sizeof(OMX_U32);

                                        BigEndianRead32Bits(&tagValue,Data);
                                        Data += sizeof(OMX_U32);
                                    }

                                    switch (tagID)
                                    {
                                        case TAGID_MPFVERSION://MPFVersion

                                            if((tagType != (OMX_U16)TAG_TYPE_UNDEFINED))
                                                goto EXIT;

                                            //LIBSKIAHW_LOGDA("MPF Version \n");
                                            JpgHdrInfo->MPIndexIFDTags.MPFVersion = tagValue;
                                            //TODO: Passed this structure to upper layers
                                            break;
                                        case TAGID_NIMAGES://Number of Images

                                            if((tagType != (OMX_U16)TAG_TYPE_LONG))
                                                goto EXIT;

                                            JpgHdrInfo->MPIndexIFDTags.numberOfImages = (OMX_U8) tagValue;
                                            //LIBSKIAHW_LOGDA("Number of Images %x \n", JpgHdrInfo->MPIndexIFDTags.numberOfImages);
                                            //TODO: Passed this structure to upper layers
                                            break;
                                        case TAGID_MPENTRY://MPEntry

                                            if((tagType != (OMX_U16)TAG_TYPE_UNDEFINED))
                                                goto EXIT;

                                            //LIBSKIAHW_LOGDA("MPEntry \n");
                                            pOffsetVal = pOffsetRef + tagValue;
                                            JpgHdrInfo->MPIndexIFDTags.MPEntry = new MP_ENTRY[(int)(tagCount/16)];
                                            for(int i=0;i<(int)(tagCount/16);i++)
                                            {
                                                JpgHdrInfo->MPIndexIFDTags.MPEntry[i].imageAttribute =
                                                (OMX_U32)((*pOffsetVal) + (*(pOffsetVal+1)<<8) + (*(pOffsetVal+2)<<16) + (*(pOffsetVal+3)<<24));
                                                pOffsetVal += sizeof(OMX_U32);
                                               // LIBSKIAHW_LOGDA("MPEntryAttribute %x \n", JpgHdrInfo->MPIndexIFDTags.MPEntry[i].imageAttribute);

                                                JpgHdrInfo->MPIndexIFDTags.MPEntry[i].imageSize =
                                                (OMX_U32)((*pOffsetVal) + (*(pOffsetVal+1)<<8) + (*(pOffsetVal+2)<<16) + (*(pOffsetVal+3)<<24));
                                                pOffsetVal += sizeof(OMX_U32);
                                               //  LIBSKIAHW_LOGDA("MPEntry ImageSize %x \n", JpgHdrInfo->MPIndexIFDTags.MPEntry[i].imageSize);

                                                JpgHdrInfo->MPIndexIFDTags.MPEntry[i].dataOffset =
                                                (OMX_U32)((*pOffsetVal) + (*(pOffsetVal+1)<<8) + (*(pOffsetVal+2)<<16) + (*(pOffsetVal+3)<<24));
                                                pOffsetVal += sizeof(OMX_U32);
                                                // LIBSKIAHW_LOGDA("MPEntry dataOffset%x \n", JpgHdrInfo->MPIndexIFDTags.MPEntry[i].dataOffset);

                                                JpgHdrInfo->MPIndexIFDTags.MPEntry[i].dependentImage1=
                                                (OMX_U16)((*pOffsetVal) + (*(pOffsetVal+1)<<8));
                                                pOffsetVal += sizeof(OMX_U16);
                                                // LIBSKIAHW_LOGDA("MPEntrydependentImage1  %x \n", JpgHdrInfo->MPIndexIFDTags.MPEntry[i].dependentImage1);

                                                JpgHdrInfo->MPIndexIFDTags.MPEntry[i].dependentImage2 =
                                                (OMX_U16)((*pOffsetVal) + (*(pOffsetVal+1)<<8));
                                                pOffsetVal += sizeof(OMX_U16);
                                                //  LIBSKIAHW_LOGDA("MPEntry dependentImage2  %x \n", JpgHdrInfo->MPIndexIFDTags.MPEntry[i].dependentImage2);

                                            }

                                            pOffsetVal = NULL;
                                        break;
                                        case TAGID_UIDLIST://Indifidual Image Unique ID list

                                            if((tagType != (OMX_U16)TAG_TYPE_UNDEFINED))
                                                goto EXIT;

                                            //LIBSKIAHW_LOGDA("Image Unique ID list Non-supported\n");
                                            //TODO: Passed this structure to upper layers

                                        break;
                                        case TAGID_TFRAMES://Total number of capture frames

                                            if((tagType != (OMX_U16)TAG_TYPE_LONG))
                                                goto EXIT;

                                            JpgHdrInfo->MPIndexIFDTags.totalFrames = (OMX_U8) tagValue;
                                            //TODO: Passed this structure to upper layers
                                            //LIBSKIAHW_LOGDA("Number of captures frames  %x \n", JpgHdrInfo->MPIndexIFDTags.totalFrames);

                                        break;
                                        case TAGID_MPIMAGENUM: //MP Individual Image Number

                                             if((tagType != (OMX_U16)TAG_TYPE_LONG))
                                                goto EXIT;

                                            JpgHdrInfo->MPIndexIFDTags.MPIndividualNum= tagValue;
                                            //LIBSKIAHW_LOGDA("MP Individual Image Number  %x \n", JpgHdrInfo->MPIndexIFDTags.MPIndividualNum);

                                            if(tagValue == 0x1)
                                                JpgHdrInfo->s3dDesc.nFrameOrder = S3D_ORDER_LF;

                                        break;
                                        case TAGID_PANSCANORIENTATION: // Panorama Scanning Orientation

                                             if((tagType != (OMX_U16)TAG_TYPE_LONG))
                                                goto EXIT;

                                            //LIBSKIAHW_LOGDA("Panorama Scanning Information not yet supported \n");
                                            //TODO: Passed this structure to upper layers

                                        break;
                                        case TAGID_BVPOINTNUM: //Base Viewpoint Number

                                            if((tagType != (OMX_U16)TAG_TYPE_LONG))
                                                goto EXIT;

                                            JpgHdrInfo->MPIndexIFDTags.baseViewpointNum= tagValue;
                                            //LIBSKIAHW_LOGDA("Base Viewpoint Number %x  \n", JpgHdrInfo->MPIndexIFDTags.baseViewpointNum);
                                            //TODO: Passed this structure to upper layers

                                        break;
                                        case TAGID_CONVANG: //Convergence Angle

                                            if((tagType != (OMX_U16)TAG_TYPE_SRATIONAL))
                                                goto EXIT;

                                            JpgHdrInfo->MPIndexIFDTags.convergenceAngle = tagValue;// Need to convert it to a 64 bits value according to standard
                                            //LIBSKIAHW_LOGDA("Convergence Angle  %x \n", JpgHdrInfo->MPIndexIFDTags.convergenceAngle );
                                            //TODO: Passed this structure to upper layers

                                        break;
                                        case TAGID_BASELINELEN: //Base Line Length

                                            if((tagType != (OMX_U16)TAG_TYPE_RATIONAL))
                                                goto EXIT;

                                            JpgHdrInfo->MPIndexIFDTags.baselineLength= tagValue;// Need to convert it to a 64 bits value according to standard
                                            //LIBSKIAHW_LOGDA("Base Line Length  %x \n", JpgHdrInfo->MPIndexIFDTags.baselineLength);
                                            //TODO: Passed this structure to upper layers

                                        break;
                                        default:
                                            LIBSKIAHW_LOGDA("Not Supported \n");
                                        break;
                                    }
                                }
                                //At the end of the IFD, read the nextIFD offset
                                if(littleEndian)
                                {
                                    nextIFDOffset = (OMX_U32)((*Data) + (*(Data+1)<<8) +
                                                          (*(Data+2)<<16) + (*(Data+3)<<24));
                                    //LIBSKIAHW_LOGDA("Resseting offset %x \n", nextIFDOffset);
                                }
                                else
                                {
                                    BigEndianRead32Bits(&nextIFDOffset,Data);
                                }
                                if((nextIFDOffset >(itemlen-6))) //APP2 field length (2bytes) + MPformat ID (4 bytes)
                                    {
                                        //LIBSKIAHW_LOGDA("size of app %d \n", itemlen);
                                        nextIFDOffset =0;
                                    }

                                Data = pOffsetRef; // Reset

                            }while(nextIFDOffset!=NULL);
                            Data = pOffsetRef -MP_START_OF_OFFSET_REF;
                        }
                    }
            default:
                     {
                    /* Skip any other sections.*/
                    break;
                }
            }

            if ( Data != NULL )
            {
                free(Data);
                Data=NULL;
            }
    }

EXIT:
    if ( Data != NULL )
    {
        free(Data);
        Data=NULL;
    }
    LOG_FUNCTION_NAME_EXIT
    return 0;
}

void SkTIJPEGImageDecoder::FillBufferDone(OMX_U8* pBuffer, OMX_U32 nFilledLen)
    {
    LOG_FUNCTION_NAME

#if JPEG_DECODER_DUMP_INPUT_AND_OUTPUT
    char *tmp= (char*)bitmap->getPixels();
    char path[50];
    snprintf(path, sizeof(path), "/JDO_%d_%d_%dx%d.yuv", dOutputCount, bitmap->config(), bitmap->width(), bitmap->height());

    SkFILEWStream tempFile(path);
    if (tempFile.write(pBuffer, nFilledLen) == false)
        {
        LIBSKIAHW_LOGDB("Writing to %s failed\n", path);
        }
    else
        {
        LIBSKIAHW_LOGDB("Writing to %s succeeded\n", path);
        }

    dOutputCount++;

#endif

    LIBSKIAHW_LOGDB("\n THE ADDRESSSS OFFFF BM->getPixels(): %p \n", bitmap->getPixels());
    iLastState = iState;
    iState = STATE_FILL_BUFFER_DONE_CALLED;

    if(pDecodeTime) {
        delete pDecodeTime;
        pDecodeTime = NULL;
    }

    LOG_FUNCTION_NAME_EXIT
    }


void SkTIJPEGImageDecoder::EventHandler(OMX_HANDLETYPE hComponent,
                                            OMX_EVENTTYPE eEvent,
                                            OMX_U32 nData1,
                                            OMX_U32 nData2,
                                            OMX_PTR pEventData)
    {
    LOG_FUNCTION_NAME

    switch ( eEvent )
        {

        case OMX_EventCmdComplete:
                {
                    /* Do not move the common stmts in these conditionals outside. */
                    /* We do not want to apply them in cases when these conditions are not met. */
                    if ((nData1 == OMX_CommandStateSet) && (nData2 == OMX_StateIdle))
                        {
                        LIBSKIAHW_LOGDA("Component State Changed To OMX_StateIdle\n");
                        iLastState = iState;
                        iState = STATE_IDLE;
                        sem_post(semaphore) ;
                        }
                    else if ((nData1 == OMX_CommandStateSet) && (nData2 == OMX_StateExecuting))
                        {
                        LIBSKIAHW_LOGDA("Component State Changed To OMX_StateExecuting\n");
                        iLastState = iState;
                        iState = STATE_EXECUTING;
                        sem_post(semaphore) ;
                        }
                    else if ((nData1 == OMX_CommandStateSet) && (nData2 == OMX_StateLoaded))
                        {
                        LIBSKIAHW_LOGDA("Component State Changed To OMX_StateLoaded\n");
                        iLastState = iState;
                        iState = STATE_LOADED;
                        sem_post(semaphore) ;
                        }
                    break;
            }

        case OMX_EventError:
                {
                    LIBSKIAHW_LOGDA("\n\n\nOMX Component  reported an Error!!!!\n\n\n");
                    iLastState = iState;
                    iState = STATE_ERROR;
                    OMX_SendCommand(hComponent, OMX_CommandStateSet, OMX_StateInvalid, NULL);
                    sem_post(semaphore) ;
                    break;
            }
        default:
            break;
    }
    LOG_FUNCTION_NAME_EXIT

    }


OMX_S32 SkTIJPEGImageDecoder::fill_data(OMX_BUFFERHEADERTYPE *pBuf, SkStream* stream, OMX_S32 bufferSize)
    {
    LOG_FUNCTION_NAME

    pBuf->nFilledLen = stream->read(pBuf->pBuffer, bufferSize);

#if JPEG_DECODER_DUMP_INPUT_AND_OUTPUT
    char path[50];
    snprintf(path, sizeof(path), "/JDI_%d.jpg", dInputCount);

    SkFILEWStream tempFile(path);
    if (tempFile.write(pBuf->pBuffer, pBuf->nFilledLen) == false)
        JPEGD_Trace("Writing to %s failed\n", path);
    else
        JPEGD_Trace("Writing to %s succeeded\n", path);

    dInputCount++;

#endif

    LOG_FUNCTION_NAME_EXIT
    return pBuf->nFilledLen;
    }


///Decision engine method that decides between ARM decoder or SIMCOP decoder
bool SkTIJPEGImageDecoder::IsHwFormat(SkStream* stream)
{
    bool useHw = false;//Use SW for 2D Images backward compatility

    inputFileSize = ParseJpegHeader(stream , &JpegHeaderInfo);

    if((JpegHeaderInfo.nFormat!=OMX_COLOR_FormatYUV420Planar)
            && (JpegHeaderInfo.nFormat!=OMX_COLOR_FormatCbYCrY))
        {
            useHw=false;
        }

    if(JpegHeaderInfo.nProgressive)
        {
        useHw = false;
        }

    return useHw;
}

///LIBSKIA decode top level method - this call further branches into ARM or SIMCOP decode based on decision engine
bool SkTIJPEGImageDecoder::onDecode(SkStream* stream, SkBitmap* bm, Mode mode)
{
    LOG_FUNCTION_NAME
    LIBSKIAHW_LOGEB ("Process %x calling onDecode", getpid());
    if(IsHwFormat(stream) && IsHwAvailable())
        {
        LOG_FUNCTION_NAME_EXIT
        return onDecodeOmx(stream, bm, mode);

        }
    else
        {
        LOG_FUNCTION_NAME_EXIT
        return onDecodeArm(stream, bm, mode);
        }
}

///Method which tries to acquire SIMCOP resource
bool SkTIJPEGImageDecoder::IsHwAvailable()
{
    LOG_FUNCTION_NAME
    char strTIJpegDec[] = "OMX.TI.DUCATI1.IMAGE.JPEGD";
    OMX_ERRORTYPE eError = OMX_ErrorNone;

    AutoTimeMillis atm("Init time: ");

    if(pOMXHandle)
        {
        return true;
        }

    OMX_Init();

    OMX_CALLBACKTYPE JPEGCallBack ={OMX_EventHandler, OMX_EmptyBufferDone, OMX_FillBufferDone};

    eError = OMX_GetHandle(&pOMXHandle, strTIJpegDec, (void *)this, &JPEGCallBack);
    if ( (eError != OMX_ErrorNone) ||  (pOMXHandle == NULL) )
        {
        LIBSKIAHW_LOGEB ("Error in Get Handle function eError %d\n", eError);
        pOMXHandle = NULL;
         OMX_Deinit();
         LOG_FUNCTION_NAME_EXIT
         return false;
        }

    iLastState = STATE_LOADED;
    iState = STATE_LOADED;
    LIBSKIAHW_LOGEB ("SIMCOP Available to process %x", getpid());

    LOG_FUNCTION_NAME_EXIT
    return true;

}

bool SkTIJPEGImageDecoder::isValidStream(SkStream* stream, int nextFileOffset)
{
        OMX_U8 a = 0;

        a = stream->readU8();
        if ( a != 0xff || stream->readU8() != M_SOI )
        {
            stream->rewind();
            return false;
        }
        else
        {
            stream->rewind();
            return true;
        }
}

///Method for decoding using ARM decoder
bool SkTIJPEGImageDecoder::onDecodeArm(SkStream* stream, SkBitmap* bm, Mode mode)
{
    if(!pARMHandle)
        {
        pARMHandle = SkNEW(SkJPEGImageDecoder);
        if(!pARMHandle)
            {
            return false;
            }
        }
    bool ret=false;
    int nextFileOffset;
    stream->rewind();

    pARMHandle->setSampleSize(this->getSampleSize());
    pARMHandle->setDitherImage(this->getDitherImage());
    pARMHandle->SetDeviceConfig(this->GetDeviceConfig());
    int scaleFactor = this->getSampleSize();

    if(fileType== TYPE_MPO)
    {
        //LIBSKIAHW_LOGDA("ARM decoder for MPO file thread Id %x , scaleFactor=%d, mode=%d, config=%d \n", pthread_self(), scaleFactor, mode, this->GetDeviceConfig());
        bm->setConfig(this->GetDeviceConfig(), (JpegHeaderInfo.nWidth/scaleFactor),(JpegHeaderInfo.nHeight/scaleFactor), JpegHeaderInfo.s3dDesc);
        if(SkImageDecoder::kDecodeBounds_Mode == mode )
            return pARMHandle->decode(stream, bm, mode);

        // For now we are assuming same resolutions files and we put in TOP/BOTTOM
        // configuration, there is no need to set any other layout
        // as our driver and algo better handle T/B.
        S3DAllocator.config(fileType, bm->width(), (bm->height() *JpegHeaderInfo.MPIndexIFDTags.numberOfImages), JpegHeaderInfo.MPIndexIFDTags.numberOfImages);
        pARMHandle->setAllocator(&S3DAllocator);

        for(int i=0;i < JpegHeaderInfo.MPIndexIFDTags.numberOfImages;i++)
        {
            // The offset of the first image is 0, so skip from next image
            if(i)
            {
                nextFileOffset = JpegHeaderInfo.MPIndexIFDTags.MPEntry[i].dataOffset + finalBytesRead;

                stream->rewind();
                // Skipping to offset
                for(int j=0;j<nextFileOffset;j++)
                    stream->read(NULL, 1);
                /*int realSize = stream->skip((size_t) (myoffset));
                LIBSKIAHW_LOGDA("Skipping by %d bytes \n", realSize);*/

                if(!isValidStream(stream, nextFileOffset))
                    return true;
            }
            ret = pARMHandle->decode(stream, bm, mode);
        }
        S3DAllocator.reset(bm);
        return ret;
    }
    else if(fileType==TYPE_JPS)
    {
         //LIBSKIAHW_LOGDA("JPS ARM decoder \n");
         bm->setConfig(this->GetDeviceConfig(), JpegHeaderInfo.nWidth/scaleFactor, JpegHeaderInfo.nHeight/scaleFactor, JpegHeaderInfo.s3dDesc);
    }
    else
    {
         //LIBSKIAHW_LOGDA("ARM decoder \n");
         bm->setConfig(this->GetDeviceConfig(), JpegHeaderInfo.nWidth/scaleFactor, JpegHeaderInfo.nHeight/scaleFactor);
      }

    return pARMHandle->decode(stream, bm, mode);
}

///Method for decoding using SIMCOP decoder
bool SkTIJPEGImageDecoder::onDecodeOmx(SkStream* stream, SkBitmap* bm, Mode mode)
    {
    LOG_FUNCTION_NAME

    android::gTIJpegDecMutex.lock();
    /* Critical section */

#ifdef TIME_DECODE
    AutoTimeMillis atm("TI JPEG Decode");
#endif
{

    AutoTimeMillis atm("Configuration Decode");

    int nRetval;
    int nIndex1;
    int nIndex2;
    int scaleFactor;
    int bitsPerPixel;
    void *p_out=NULL;
    MemAllocBlock *MemReqDescTiler;
    OMX_S32 nCompId = 100;
    OMX_ERRORTYPE eError = OMX_ErrorNone;
    OMX_PORT_PARAM_TYPE PortType;

    OMX_JPEG_PARAM_UNCOMPRESSEDMODETYPE tUncompressedMode;
    OMX_IMAGE_PARAM_DECODE_SUBREGION pSubRegionDecode;

    LIBSKIAHW_LOGDA("\nUsing TI Image Decoder.\n");

    bitmap = bm;
    inStream = stream;
    SkBitmap::Config config = this->getPrefConfig(k32Bit_SrcDepth, false);
    scaleFactor = this->getSampleSize();

    LIBSKIAHW_LOGDB("Config = %d\n", config);
    LIBSKIAHW_LOGDB("mode = %d\n", mode);
    LIBSKIAHW_LOGDB("scaleFactor = %d ", scaleFactor);


#ifdef TIME_DECODE
    atm.setResolution(JpegHeaderInfo.nWidth , JpegHeaderInfo.nHeight);
#endif

    if (inputFileSize == 0) {
        LIBSKIAHW_LOGEA("The file size is 0. Maybe the format of the file is not correct\n");
        goto EXIT;
    }

    // if no user preference, see what the device recommends
    if (config == SkBitmap::kNo_Config)
        config = SkImageDecoder::GetDeviceConfig();

    bm->setConfig(config, JpegHeaderInfo.nWidth/scaleFactor, JpegHeaderInfo.nHeight/scaleFactor);
    bm->setIsOpaque(true);
    LIBSKIAHW_LOGDB("bm->width() = %d\n", bm->width());
    LIBSKIAHW_LOGDB("bm->height() = %d\n", bm->height());
    LIBSKIAHW_LOGDB("bm->config() = %d\n", bm->config());
    LIBSKIAHW_LOGDB("bm->getSize() = %d\n", bm->getSize());

    if (SkImageDecoder::kDecodeBounds_Mode == mode)
        {
        android::gTIJpegDecMutex.unlock();
        LIBSKIAHW_LOGDA("Leaving Critical Section 1 \n");
        return true;
        }

    PortType.nSize = sizeof(OMX_PORT_PARAM_TYPE);
    PortType.nVersion.s.nVersionMajor = 0x1;
    PortType.nVersion.s.nVersionMinor = 0x1;
    PortType.nVersion.s.nRevision = 0x0;
    PortType.nVersion.s.nStep = 0x0;
    PortType.nPorts = 2;
    PortType.nStartPortNumber = 0;

    eError = OMX_SetParameter(pOMXHandle, OMX_IndexParamImageInit, &PortType);
    if ( eError != OMX_ErrorNone )
        {
        LIBSKIAHW_LOGEA ("Error in OMX_SetParameter function\n");
        iState = STATE_ERROR;
        goto EXIT;
        }

    /**********************************************************************/
    /* Set the component's OMX_PARAM_PORTDEFINITIONTYPE structure (INPUT) */
    /**********************************************************************/

    InPortDef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
    InPortDef.nVersion.s.nVersionMajor = 0x1;
    InPortDef.nVersion.s.nVersionMinor = 0x1;
    InPortDef.nVersion.s.nRevision = 0x0;
    InPortDef.nVersion.s.nStep = 0x0;
    InPortDef.nPortIndex = OMX_JPEGD_TEST_INPUT_PORT;
    InPortDef.eDir = OMX_DirInput;
    InPortDef.nBufferCountActual =1;
    InPortDef.nBufferCountMin = 1;
    InPortDef.nBufferSize = inputFileSize;
    InPortDef.bEnabled = OMX_TRUE;
    InPortDef.bPopulated = OMX_FALSE;
    InPortDef.eDomain = OMX_PortDomainImage;
    InPortDef.format.image.cMIMEType = (OMX_STRING)"OMXJPEGD";
    InPortDef.format.image.pNativeRender = 0;
    InPortDef.format.image.nFrameWidth = inputFileSize;
    InPortDef.format.image.nFrameHeight = 1;
    InPortDef.format.image.nStride = 1;
    InPortDef.format.image.nSliceHeight = 1;
    InPortDef.format.image.bFlagErrorConcealment = OMX_FALSE;
    InPortDef.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;
    InPortDef.format.image.pNativeWindow = 0x0;
    InPortDef.bBuffersContiguous = OMX_FALSE;
    InPortDef.nBufferAlignment = 0;

    if (JpegHeaderInfo.nFormat == OMX_COLOR_FormatYCbYCr)
        {
        InPortDef.format.image.eColorFormat = OMX_COLOR_FormatCbYCrY;
        }
    else
        {
        ///@todo Check this input format
        InPortDef.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
        }

    eError = OMX_SetParameter (pOMXHandle, OMX_IndexParamPortDefinition, &InPortDef);
    if ( eError != OMX_ErrorNone )
        {
        LIBSKIAHW_LOGEA ("Error in OMX_SetParameter function\n");
        eError = OMX_ErrorBadParameter;
        iState = STATE_ERROR;
        goto EXIT;
        }

    /***********************************************************************/
    /* Set the component's OMX_PARAM_PORTDEFINITIONTYPE structure (OUTPUT) */
    /***********************************************************************/

    OutPortDef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
    OutPortDef.nVersion.s.nVersionMajor = 0x1;
    OutPortDef.nVersion.s.nVersionMinor = 0x1;
    OutPortDef.nVersion.s.nRevision = 0x0;
    OutPortDef.nVersion.s.nStep = 0x0;

    OutPortDef.nPortIndex = OMX_JPEGD_TEST_OUTPUT_PORT;
    OutPortDef.eDir = OMX_DirOutput;
    OutPortDef.nBufferCountActual = 1;
    OutPortDef.nBufferCountMin = 1;
    OutPortDef.bEnabled = OMX_TRUE;
    OutPortDef.bPopulated = OMX_FALSE;
    OutPortDef.eDomain = OMX_PortDomainImage;
    OutPortDef.format.image.cMIMEType = (OMX_STRING)"OMXJPEGD";
    OutPortDef.format.image.pNativeRender = 0;
    OutPortDef.format.image.nFrameWidth = JpegHeaderInfo.nWidth/scaleFactor;
    LIBSKIAHW_LOGDB("\nOutPortDef.format.image.nFrameWidth = %d\n", (int)OutPortDef.format.image.nFrameWidth);

    OutPortDef.format.image.nFrameHeight = JpegHeaderInfo.nHeight/scaleFactor;
    LIBSKIAHW_LOGDB("\nOutPortDef.format.image.nFrameHeight = %d\n", (int)OutPortDef.format.image.nFrameHeight);

    OutPortDef.format.image.nStride = JpegHeaderInfo.nWidth/scaleFactor;
    LIBSKIAHW_LOGDB("\n OutPortDef.format.image.nStride = %d\n", (int)OutPortDef.format.image.nStride);

    OutPortDef.format.image.nSliceHeight = 0;
    OutPortDef.format.image.bFlagErrorConcealment = OMX_FALSE;
    OutPortDef.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
    OutPortDef.format.image.pNativeWindow = 0x0;
    OutPortDef.bBuffersContiguous = OMX_FALSE;
    OutPortDef.nBufferAlignment = 0; //64

    if (config == SkBitmap::kA8_Config)
        { /* 8-bits per pixel, with only alpha specified (0 is transparent, 0xFF is opaque) */
        OutPortDef.format.image.eColorFormat = OMX_COLOR_FormatL8;
        bitsPerPixel = 1;
        }
    else if (config == SkBitmap::kRGB_565_Config)
        { /* 16-bits per pixel*/
        OutPortDef.format.image.eColorFormat = OMX_COLOR_Format16bitRGB565;
        LIBSKIAHW_LOGDA("Color format is OMX_COLOR_Format16bitRGB565\n");
        bitsPerPixel = 2;
        }
    else if (config == SkBitmap::kARGB_8888_Config)
        { /* 32-bits per pixel */
        OutPortDef.format.image.eColorFormat = OMX_COLOR_Format32bitARGB8888;
        LIBSKIAHW_LOGDA("Color format is OMX_COLOR_Format32bitARGB8888\n");
        bitsPerPixel = 4;
        }
    else
        { /* Set DEFAULT color format*/
        ///@todo extend the bitmap supported formats to YUV422, YUV420 etc
        OutPortDef.format.image.eColorFormat = OMX_COLOR_FormatCbYCrY;
        bitsPerPixel = 2;
        }

    OutPortDef.nBufferSize = (JpegHeaderInfo.nWidth / scaleFactor ) * (JpegHeaderInfo.nHeight / scaleFactor) * bitsPerPixel ;
    LIBSKIAHW_LOGDB("Output buffer size is %ld\n", OutPortDef.nBufferSize);


    eError = OMX_SetParameter (pOMXHandle, OMX_IndexParamPortDefinition, &OutPortDef);
    if ( eError != OMX_ErrorNone )
        {
        eError = OMX_ErrorBadParameter;
        iState = STATE_ERROR;
        goto EXIT;
        }

    tUncompressedMode.nSize = sizeof(OMX_JPEG_PARAM_UNCOMPRESSEDMODETYPE);
    tUncompressedMode.nVersion.s.nVersionMajor = 0x1;
    tUncompressedMode.nVersion.s.nVersionMinor = 0x1;
    tUncompressedMode.nVersion.s.nRevision = 0x0;
    tUncompressedMode.nVersion.s.nStep = 0x0;

    /* Test Set Parameteres OMX_JPEG_PARAM_UNCOMPRESSEDMODETYPE */
    tUncompressedMode.nPortIndex = OMX_JPEGD_TEST_INPUT_PORT;
    tUncompressedMode.eUncompressedImageMode = OMX_JPEG_UncompressedModeFrame;

    eError = OMX_SetParameter (pOMXHandle, (OMX_INDEXTYPE)OMX_TI_IndexParamJPEGUncompressedMode,&tUncompressedMode);
    if ( eError != OMX_ErrorNone )
        {
        LIBSKIAHW_LOGEB ("JPEGDec test:: error= %x\n", eError);
        iState = STATE_ERROR;
        goto EXIT;
        }

    LIBSKIAHW_LOGDA("OMX_SetParameter input image uncompressed mode done successfully");

    pSubRegionDecode.nSize = sizeof(OMX_IMAGE_PARAM_DECODE_SUBREGION);
    pSubRegionDecode.nVersion.s.nVersionMajor = 0x1;
    pSubRegionDecode.nVersion.s.nVersionMinor = 0x1;
    pSubRegionDecode.nVersion.s.nRevision = 0x0;
    pSubRegionDecode.nVersion.s.nStep = 0x0;

    pSubRegionDecode.nXOrg = 0;
    pSubRegionDecode.nYOrg = 0;
    pSubRegionDecode.nXLength = 0;
    pSubRegionDecode.nYLength = 0;

    /* Set Parameteres OMX_IMAGE_PARAM_DECODE_SUBREGION   */
    eError = OMX_SetParameter(pOMXHandle,(OMX_INDEXTYPE)OMX_TI_IndexParamDecodeSubregion,&pSubRegionDecode);
    if ( eError != OMX_ErrorNone )
        {
        LIBSKIAHW_LOGEB ("JPEGDec test:: %d:error= %x\n", __LINE__, eError);
        iState = STATE_ERROR;
        goto EXIT;
        }
    LIBSKIAHW_LOGDA("OMX_SetParameter sub region decode done successfully");

   eError = OMX_SendCommand(pOMXHandle, OMX_CommandStateSet, OMX_StateIdle ,NULL);
    if ( eError != OMX_ErrorNone )
        {
        LIBSKIAHW_LOGEA ("Error from SendCommand-Idle(Init) State function\n");
        iState = STATE_ERROR;
        goto EXIT;
        }

    if (!bm->allocPixels(&allocator, NULL))
        {
        LIBSKIAHW_LOGEA("xxxxxxxxxxxxxxxxxxxx allocPixels failed\n");
        iState = STATE_ERROR;
        goto EXIT;
        }

    eError = OMX_AllocateBuffer(pOMXHandle, &pInBuffHead,  InPortDef.nPortIndex, (void *)&nCompId, InPortDef.nBufferSize);
    if ( eError != OMX_ErrorNone )
        {
        LIBSKIAHW_LOGEB ("JPEGDec test:: %d:error= %x\n", __LINE__, eError);
        iState = STATE_ERROR;
        goto EXIT;
        }

    eError = OMX_UseBuffer(pOMXHandle, &pOutBuffHead,  OutPortDef.nPortIndex, (void *)&nCompId, OutPortDef.nBufferSize, (OMX_U8*)bm->getPixels());
    if ( eError != OMX_ErrorNone )
        {
        LIBSKIAHW_LOGEB ("JPEGDec test:: %d:error= %x\n", __LINE__, eError);
        iState = STATE_ERROR;
        goto EXIT;
        }

    LIBSKIAHW_LOGDB(" \n THE ADDRESSSS OFFFF BM->getPixels(): %p  \n", bm->getPixels());
}
    android::gTIJpegDecMutex.unlock();
    pBeforeDecodeTime=new AutoTimeMillis("Before_BufferDecode Time");
    Run();

    return true;

EXIT:
    if (iState == STATE_ERROR) {
        sem_post(semaphore);
        Run();
    }
    android::gTIJpegDecMutex.unlock();
    LIBSKIAHW_LOGDA("Leaving Critical Section 3 \n");

    LOG_FUNCTION_NAME_EXIT
    return false;
    }

void SkTIJPEGImageDecoder::PrintState()
    {
    LOG_FUNCTION_NAME
    switch(iState)
        {
        case STATE_LOADED:
            LIBSKIAHW_LOGDA("Current State is STATE_LOADED.\n");
            break;

        case STATE_IDLE:
            LIBSKIAHW_LOGDA("Current State is STATE_IDLE.\n");
            break;

        case STATE_EXECUTING:
            LIBSKIAHW_LOGDA("Current State is STATE_EXECUTING.\n");
            break;

        case STATE_EMPTY_BUFFER_DONE_CALLED:
            LIBSKIAHW_LOGDA("Current State is STATE_EMPTY_BUFFER_DONE_CALLED.\n");
            break;

        case STATE_FILL_BUFFER_DONE_CALLED:
            LIBSKIAHW_LOGDA("Current State is STATE_FILL_BUFFER_DONE_CALLED.\n");
            break;

        case STATE_ERROR:
            LIBSKIAHW_LOGDA("Current State is STATE_ERROR.\n");
            break;

        case STATE_EXIT:
            LIBSKIAHW_LOGDA("Current State is STATE_EXIT.\n");
            break;

        default:
            LIBSKIAHW_LOGDA("Current State is Invalid.\n");
            break;

        }
    LOG_FUNCTION_NAME_EXIT
    }

void SkTIJPEGImageDecoder::Run()
    {
    LOG_FUNCTION_NAME

    int nRead;
    OMX_ERRORTYPE eError = OMX_ErrorNone;

    while(1)
        {

        sem_wait(semaphore) ;
        LIBSKIAHW_LOGDA("-Run Sem Signalled\n");

        PrintState();
        switch(iState)
        {
            case STATE_IDLE:
                    {
                        if (iLastState == STATE_LOADED)
                            {
                            LIBSKIAHW_LOGDA("Entered STATE_IDLE in switch - iLastState = STATE_LOADED\n\n");
                            eError = OMX_SendCommand(pOMXHandle,OMX_CommandStateSet, OMX_StateExecuting, NULL);
                            if ( eError != OMX_ErrorNone )
                                {
                                LIBSKIAHW_LOGEA("eError from SendCommand-Executing State function\n");
                                iState = STATE_ERROR;
                                break;
                                }
                            }
                        else if ((iLastState == STATE_EMPTY_BUFFER_DONE_CALLED) || (iLastState==STATE_FILL_BUFFER_DONE_CALLED))
                            {
                            LIBSKIAHW_LOGDA("Entered STATE_IDLE in switch - iLastState = STATE_EMPTY_BUFFER_DONE_CALLED\n\n");

                            eError = OMX_SendCommand(pOMXHandle, OMX_CommandPortDisable, 0x0, NULL);
                            if ( eError != OMX_ErrorNone )
                                {
                                LIBSKIAHW_LOGEA("Error from SendCommand-PortDisable function. Input port.\n");
                                iState = STATE_ERROR;
                                break;
                                }

                            eError = OMX_SendCommand(pOMXHandle, OMX_CommandPortDisable, 0x1, NULL);
                            if ( eError != OMX_ErrorNone )
                                {
                                LIBSKIAHW_LOGEA("Error from SendCommand-PortDisable function. Output port.\n");
                                iState = STATE_ERROR;
                                break;
                                }

                            /* Free buffers */
                            eError = OMX_FreeBuffer(pOMXHandle, InPortDef.nPortIndex, pInBuffHead);
                            if ( eError != OMX_ErrorNone )
                                {
                                LIBSKIAHW_LOGEA("Error from OMX_FreeBuffer. Input port.\n");
                                iState = STATE_ERROR;
                                break;
                                }

                            eError = OMX_FreeBuffer(pOMXHandle, OutPortDef.nPortIndex, pOutBuffHead);
                            if ( eError != OMX_ErrorNone )
                                {
                                LIBSKIAHW_LOGEA("Error from OMX_FreeBuffer. Output port.\n");
                                iState = STATE_ERROR;
                                break;
                                }

                         eError = OMX_SendCommand(pOMXHandle,OMX_CommandStateSet, OMX_StateLoaded, NULL);
                            if ( eError != OMX_ErrorNone )
                                {
                                LIBSKIAHW_LOGEA("Error from SendCommand-Idle State function\n");
                                iState = STATE_ERROR;
                                break;
                                }
                            break;
                            }
                        else
                            {
                            LIBSKIAHW_LOGDB("Entered STATE_IDLE in switch - iLastState = %d\n\n", iLastState);
                            iState = STATE_ERROR;
                            }
                        break;
                    }

            case STATE_EXECUTING:
                    {
                        LIBSKIAHW_LOGDA("\nOMX Component in executing state, queuing the input and output buffers\n");
                        TIMM_OSAL_Memset (pInBuffHead->pBuffer, 0, InPortDef.nBufferSize);
                        TIMM_OSAL_Memset (pOutBuffHead->pBuffer, 0XFF, OutPortDef.nBufferSize);

                        inStream->rewind();
                        nRead = fill_data(pInBuffHead, inStream, pInBuffHead->nAllocLen);

                        pInBuffHead->nFlags = OMX_BUFFERFLAG_EOS;
                        pInBuffHead->nInputPortIndex = OMX_JPEGD_TEST_INPUT_PORT;
                        pInBuffHead->nAllocLen = pInBuffHead->nFilledLen;
                        pInBuffHead->nOffset = 0;

                        if(pBeforeDecodeTime) {
                            delete pBeforeDecodeTime;
                            pBeforeDecodeTime = NULL;
                        }

                        pDecodeTime=new AutoTimeMillis("BufferDecode Time");
                        OMX_EmptyThisBuffer(pOMXHandle, pInBuffHead);

                        pOutBuffHead->nOutputPortIndex = OMX_JPEGD_TEST_OUTPUT_PORT;
                        OMX_FillThisBuffer(pOMXHandle, pOutBuffHead);
                        LIBSKIAHW_LOGDA("\nQueued input and output buffers to the component\n");
                        break;
                }

            case STATE_EMPTY_BUFFER_DONE_CALLED:
            case STATE_FILL_BUFFER_DONE_CALLED:
                    {

                        pAfterDecodeTime=new AutoTimeMillis("After_BufferDecode Time");

                        ///@todo make this logic cleaner - should be waiting for both EBD and FBD to arrive
                        ///        Put this function under mutex so that state variables are not raced
                        LIBSKIAHW_LOGDA("\nProcessing empty buffer done or fill buffer done state, sending component to idle state\n");
                        eError = OMX_SendCommand(pOMXHandle,OMX_CommandStateSet, OMX_StateIdle, NULL);
                        if ( eError != OMX_ErrorNone ) {
                            LIBSKIAHW_LOGEA("Error from SendCommand-Idle(nStop) State function\n");
                            iState = STATE_ERROR;
                        }
                        break;
                }

            case STATE_LOADED:
            case STATE_ERROR:
                    {
                        /*### Assume that the inStream will be closed by some upper layer */
                        /*### Assume that the Bitmap object/file need not be closed. */
                        /*### More needs to be done here */
                        /*### Do different things based on iLastState */
            if ( iState == STATE_ERROR ) {
                if (pInBuffHead != NULL) {
                    /* Free buffers if it got allocated */
                    eError = OMX_FreeBuffer(pOMXHandle, InPortDef.nPortIndex, pInBuffHead);
                    if ( eError != OMX_ErrorNone ) {
                        LIBSKIAHW_LOGEA("Error from OMX_FreeBuffer. Input port.\n");
                    }
                }
                if (pOutBuffHead != NULL) {
                    eError = OMX_FreeBuffer(pOMXHandle, OutPortDef.nPortIndex, pOutBuffHead);
                    if ( eError != OMX_ErrorNone ) {
                        LIBSKIAHW_LOGEA("Error from OMX_FreeBuffer. Output port.\n");
                    }
                }
            }

                        iState = STATE_EXIT;
                        sem_post(semaphore);

                        if(pAfterDecodeTime) {
                            delete pAfterDecodeTime;
                            pAfterDecodeTime = NULL;
                        }

                        break;
                }

            default:
                break;
        }

        if (iState == STATE_EXIT)
            {
            break;
            }
        }
    LOG_FUNCTION_NAME_EXIT
    }

///@optimization Use one global instance of decoder so that we don't have to call OMX_GetHandle, OMX_FreeHandle every time
static SkTIJPEGImageDecoder gJpegDecoder;

///Wrapper class which will be created every time by the factory method.
///This class wraps the handle to the global singleton jpeg decoder instance
class SkJPEGTIImageDecoderWrapper : public SkImageDecoder {
public:
    SkJPEGTIImageDecoderWrapper(SkImageDecoder* ptr)
        {
        mPtr = ptr;
        }
    virtual Format getFormat() const {
        return kJPEG_Format;
    }

protected:
    virtual bool onDecode(SkStream* stream, SkBitmap* bm,
                          Mode mode)
        {
            mPtr->setSampleSize(this->getSampleSize());
            mPtr->setDitherImage(this->getDitherImage());
            mPtr->SetDeviceConfig(this->GetDeviceConfig());
            return mPtr->decode(stream, bm, mode);
        }
public:
    SkImageDecoder *mPtr;
};

///LIBSKIAHW Factory method
extern "C" SkImageDecoder* SkImageDecoder_HWJPEG_Factory() {
    return new SkJPEGTIImageDecoderWrapper(&gJpegDecoder);
}


