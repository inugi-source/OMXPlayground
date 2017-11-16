//
//  omxJPEGEnc.c
//  OpenMAX
//
//  Created by Michael Kwasnicki on 29.08.17.
//  Copyright © 2017 Michael Kwasnicki. All rights reserved.
//

// inspired by https://github.com/hopkinskong/rpi-omx-jpeg-encode


#include "omxJPEGEnc.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#define OMX_SKIP64BIT
#include <IL/OMX_Core.h>
#include <interface/vcos/vcos.h>

#include "cHelper.h"
#include "omxHelper.h"



typedef struct {
    OMX_HANDLETYPE handle;

    OMX_U32 inputPortIndex;
    OMX_BUFFERHEADERTYPE *inputBuffer;
    bool inputReady;

    OMX_BUFFERHEADERTYPE *outputBuffer;
    OMX_U32 outputPortIndex;
    bool outputReady;
} OMXImageEncode_s;



struct OMXContext_s {
    OMXImageEncode_s imageEncode;

    VCOS_SEMAPHORE_T handler_lock;
};




static OMX_ERRORTYPE omxEventHandler(
                                     OMX_IN OMX_HANDLETYPE hComponent,
                                     OMX_IN OMX_PTR pAppData,
                                     OMX_IN OMX_EVENTTYPE eEvent,
                                     OMX_IN OMX_U32 nData1,
                                     OMX_IN OMX_U32 nData2,
                                     OMX_IN OMX_PTR pEventData) {

    printf("eEvent: %s,  ", omxEventTypeEnum(eEvent));
    //printf("eEvent: %s,  nData1: %x,  nData2: %x\n", omxEventTypeEnum(eEvent), nData1, nData2);
    OMXContext_s *ctx = (OMXContext_s *)pAppData;

    switch(eEvent) {
        case OMX_EventCmdComplete:
            printf("Command: %s,  ", omxCommandTypeEnum(nData1));

            switch (nData1) {
                case OMX_CommandStateSet:
                    printf("State: %s\n", omxStateTypeEnum(nData2));
                    break;

                case OMX_CommandPortDisable:
                case OMX_CommandPortEnable:
                    printf("Port: %d\n", nData2);
                    break;

                default:
                    printf("nData2: 0x%x\n", nData2);
            }

            break;

        case OMX_EventError:
            printf(COLOR_RED "ErrorType: %s,  nData2: %x\n" COLOR_NC, omxErrorTypeEnum(nData1), nData2);

            if (nData1 != OMX_ErrorStreamCorrupt) {
                assert(NULL);
            }
            break;

        default:
            printf("nData1: %x,  nData2: %x\n", nData1, nData2);
            printf("unhandeled event %x: %x %x\n", eEvent, nData1, nData2);
            break;
    }

    return OMX_ErrorNone;
}



static OMX_ERRORTYPE omxEmptyBufferDone(
                                        OMX_IN OMX_HANDLETYPE hComponent,
                                        OMX_IN OMX_PTR pAppData,
                                        OMX_IN OMX_BUFFERHEADERTYPE *pBuffer) {
    OMXContext_s *ctx = (OMXContext_s*)pAppData;
//    vcos_semaphore_wait(&ctx->handler_lock);
    ctx->imageEncode.inputReady = true;
    vcos_semaphore_post(&ctx->handler_lock);
    return OMX_ErrorNone;
}



static OMX_ERRORTYPE omxFillBufferDone(
                                       OMX_OUT OMX_HANDLETYPE hComponent,
                                       OMX_OUT OMX_PTR pAppData,
                                       OMX_OUT OMX_BUFFERHEADERTYPE *pBuffer) {
    OMXContext_s *ctx = (OMXContext_s*)pAppData;
//    vcos_semaphore_wait(&ctx->handler_lock);
    ctx->imageEncode.outputReady = true;
    vcos_semaphore_post(&ctx->handler_lock);
    return OMX_ErrorNone;
}



static void getImageEncodePorts(OMXImageEncode_s *component) {
    OMX_ERRORTYPE omxErr = OMX_ErrorNone;
    OMX_PORT_PARAM_TYPE ports;
    OMX_INIT_STRUCTURE(ports);
    omxErr = OMX_GetParameter(component->handle, OMX_IndexParamImageInit, &ports);
    omxAssert(omxErr);
    const OMX_U32 pEnd = ports.nStartPortNumber + ports.nPorts;

    for (OMX_U32 p = ports.nStartPortNumber; p < pEnd; p++) {
        OMX_PARAM_PORTDEFINITIONTYPE portDefinition;
        OMX_INIT_STRUCTURE(portDefinition);
        portDefinition.nPortIndex = p;
        omxErr = OMX_GetParameter(component->handle, OMX_IndexParamPortDefinition, &portDefinition);
        omxAssert(omxErr);

        if (portDefinition.eDir == OMX_DirInput) {
            assert(component->inputPortIndex == 0);
            component->inputPortIndex = p;
        }

        if (portDefinition.eDir == OMX_DirOutput) {
            assert(component->outputPortIndex == 0);
            component->outputPortIndex = p;
        }
    }
}



static bool setupImageEncodeInputPort(OMXImageEncode_s *component, OMX_U32 nFrameWidth, OMX_U32 nFrameHeight, OMX_U32 nSliceHeight, OMX_COLOR_FORMATTYPE eColorFormat) {
    assert((nSliceHeight == 16) || (nSliceHeight == nFrameHeight));
    assert(omxAssertImagePortFormatSupported(component->handle, component->inputPortIndex, eColorFormat));
    // supports also OMX_COLOR_Format8bitPalette

    OMX_ERRORTYPE omxErr = OMX_ErrorNone;
    OMX_PARAM_PORTDEFINITIONTYPE portDefinition;
    OMX_INIT_STRUCTURE(portDefinition);
    portDefinition.nPortIndex = component->inputPortIndex;
    omxErr = OMX_GetParameter(component->handle, OMX_IndexParamPortDefinition, &portDefinition);
    omxAssert(omxErr);
    portDefinition.format.image.nFrameWidth = nFrameWidth;
    portDefinition.format.image.nFrameHeight = nFrameHeight;
    portDefinition.format.image.nSliceHeight = nSliceHeight; // 16 | nFrameHeight
    portDefinition.format.image.nStride = 0;
    portDefinition.format.image.bFlagErrorConcealment = OMX_FALSE;
    portDefinition.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
    portDefinition.format.image.eColorFormat = eColorFormat;
    omxErr = OMX_SetParameter(component->handle, OMX_IndexParamPortDefinition, &portDefinition);

    if (omxErr != OMX_ErrorNone) {
        puts(COLOR_RED "Failed" COLOR_NC );
        return false;
    }

    omxAssert(omxErr);
    omxErr = OMX_GetParameter(component->handle, OMX_IndexParamPortDefinition, &portDefinition);
    omxAssert(omxErr);

    omxPrintPort(component->handle, component->inputPortIndex);
    printf("%d %d (%d)\n", nFrameWidth, nSliceHeight, portDefinition.nBufferSize);

    omxEnablePort(component->handle, component->inputPortIndex, OMX_TRUE);

    omxErr = OMX_AllocateBuffer(component->handle, &component->inputBuffer, component->inputPortIndex, NULL, portDefinition.nBufferSize);
    omxAssert(omxErr);
    return true;
}



static void setupImageEncodeOutputPort(OMXImageEncode_s *component, OMX_U32 nQFactor) {
    OMX_ERRORTYPE omxErr = OMX_ErrorNone;
    OMX_PARAM_PORTDEFINITIONTYPE portDefinition;
    OMX_INIT_STRUCTURE(portDefinition);
    portDefinition.nPortIndex = component->outputPortIndex;
    omxErr = OMX_GetParameter(component->handle, OMX_IndexParamPortDefinition, &portDefinition);
    omxAssert(omxErr);
    portDefinition.format.image.bFlagErrorConcealment = OMX_FALSE;
    portDefinition.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;
    portDefinition.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
    omxErr = OMX_SetParameter(component->handle, OMX_IndexParamPortDefinition, &portDefinition);
    omxAssert(omxErr);
    omxErr = OMX_GetParameter(component->handle, OMX_IndexParamPortDefinition, &portDefinition);
    omxAssert(omxErr);

    OMX_IMAGE_PARAM_QFACTORTYPE qFactor;
    OMX_INIT_STRUCTURE(qFactor);
    qFactor.nPortIndex = component->outputPortIndex;
    qFactor.nQFactor = nQFactor;
    omxErr = OMX_SetParameter(component->handle, OMX_IndexParamQFactor, &qFactor);
    omxAssert(omxErr);

    omxEnablePort(component->handle, component->outputPortIndex, OMX_TRUE);

    omxErr = OMX_AllocateBuffer(component->handle, &component->outputBuffer, component->outputPortIndex, NULL, portDefinition.nBufferSize);
    omxAssert(omxErr);
}



static void freeImageEncodeBuffers(OMXImageEncode_s *component) {
    OMX_ERRORTYPE omxErr = OMX_ErrorNone;
    omxErr = OMX_FreeBuffer(component->handle, component->inputPortIndex, component->inputBuffer);
    omxAssert(omxErr);
    omxErr = OMX_FreeBuffer(component->handle, component->outputPortIndex, component->outputBuffer);
    omxAssert(omxErr);
}



OMXContext_s * omxJPEGEncInit(uint32_t rawImageWidth, uint32_t rawImageHeight, uint32_t sliceHeight, uint8_t outputQuality, OMX_COLOR_FORMATTYPE colorFormat) {
    OMX_ERRORTYPE omxErr = OMX_ErrorNone;
    VCOS_STATUS_T vcosErr = VCOS_SUCCESS;
    int result;

    OMXContext_s *ctx = malloc(sizeof(OMXContext_s));
    memset(ctx, 0, sizeof(*ctx));

    vcosErr = vcos_semaphore_create(&ctx->handler_lock, "handler_lock", 1);
    assert(vcosErr == VCOS_SUCCESS);

    OMX_STRING omxComponentName = "OMX.broadcom.image_encode";
    OMX_CALLBACKTYPE omxCallbacks;
    omxCallbacks.EventHandler = omxEventHandler;
    omxCallbacks.EmptyBufferDone = omxEmptyBufferDone;
    omxCallbacks.FillBufferDone = omxFillBufferDone;
    omxErr = OMX_GetHandle(&ctx->imageEncode.handle, omxComponentName, ctx, &omxCallbacks);
    omxAssert(omxErr);
    omxAssertState(ctx->imageEncode.handle, OMX_StateLoaded);

    getImageEncodePorts(&ctx->imageEncode);
    omxEnablePort(ctx->imageEncode.handle, ctx->imageEncode.inputPortIndex, OMX_FALSE);
    omxEnablePort(ctx->imageEncode.handle, ctx->imageEncode.outputPortIndex, OMX_FALSE);
    omxSwitchToState(ctx->imageEncode.handle, OMX_StateIdle);

    bool x = setupImageEncodeInputPort(&ctx->imageEncode, rawImageWidth, rawImageHeight, sliceHeight, colorFormat);

    return (OMXContext_s *)x;

//    setupImageEncodeOutputPort(&ctx->imageEncode, outputQuality);
//    omxSwitchToState(ctx->imageEncode.handle, OMX_StateExecuting);

//    return ctx;
}



void omxJPEGEncDeinit(OMXContext_s *ctx) {
    OMX_ERRORTYPE omxErr = OMX_ErrorNone;
    omxSwitchToState(ctx->imageEncode.handle, OMX_StateIdle);
    omxEnablePort(ctx->imageEncode.handle, ctx->imageEncode.inputPortIndex, OMX_FALSE);
    omxEnablePort(ctx->imageEncode.handle, ctx->imageEncode.outputPortIndex, OMX_FALSE);
    freeImageEncodeBuffers(&ctx->imageEncode);
    omxSwitchToState(ctx->imageEncode.handle, OMX_StateLoaded);


    omxErr = OMX_FreeHandle(ctx->imageEncode.handle);
    omxAssert(omxErr);
    free(ctx);
}



void omxJPEGEncProcess(OMXContext_s *ctx, uint8_t *output, size_t *outputFill, size_t outputSize, uint8_t *rawImage, size_t rawImageSize) {
    OMX_ERRORTYPE omxErr = OMX_ErrorNone;

    int pos = 0;
    uint32_t sliceSize = ctx->imageEncode.inputBuffer->nAllocLen;
    ctx->imageEncode.inputReady = true;
    ctx->imageEncode.outputReady = false;

    omxErr = OMX_FillThisBuffer(ctx->imageEncode.handle, ctx->imageEncode.outputBuffer);
    omxAssert(omxErr);

    // FILE *output = fopen("out.jpg", "wb");

    while (true) {
        if (ctx->imageEncode.outputReady) {
            ctx->imageEncode.outputReady = false;
            //fwrite(ctx->imageEncode.outputBuffer->pBuffer + ctx->imageEncode.outputBuffer->nOffset, sizeof(uint8_t), ctx->imageEncode.outputBuffer->nFilledLen, output);

            if (ctx->imageEncode.outputBuffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
                break;
            }

            omxErr = OMX_FillThisBuffer(ctx->imageEncode.handle, ctx->imageEncode.outputBuffer);
            omxAssert(omxErr);
        }

        if (ctx->imageEncode.inputReady) {
            ctx->imageEncode.inputReady = false;

            if (pos == rawImageSize) {
                continue;
            }

            memcpy(ctx->imageEncode.inputBuffer->pBuffer, &rawImage[pos], sliceSize);
            ctx->imageEncode.inputBuffer->nOffset = 0;
            ctx->imageEncode.inputBuffer->nFilledLen = sliceSize;
            pos += sliceSize;

            if (pos + sliceSize > rawImageSize) {
                sliceSize = rawImageSize - pos;
            }

            omxErr = OMX_EmptyThisBuffer(ctx->imageEncode.handle, ctx->imageEncode.inputBuffer);
            omxAssert(omxErr);
        }

        vcos_semaphore_wait(&ctx->handler_lock);
        puts(".");
        //            vcos_semaphore_post(&ctx->handler_lock);
    }

    //fclose(output);
    puts(COLOR_RED "done" COLOR_NC " blaub " COLOR_GREEN "yes" COLOR_NC);
}







void omxJPEGEnc() {
    OMX_U32 outputQuality = 20;      // [1, 100] as in libJPEG 9

    uint32_t rawImageWidth = 640;
    uint32_t rawImageHeight = 480;
    OMX_S32 sliceHeight = rawImageHeight;
    uint8_t rawImageChannels = 3;
    size_t rawImageSize = rawImageWidth * rawImageHeight * rawImageChannels;
    uint8_t *rawImage = (uint8_t *)malloc(rawImageSize);

    for (uint32_t y = 0; y < rawImageHeight; y++) {
        for (uint32_t x = 0; x < rawImageWidth; x++) {
            ssize_t index = (x + rawImageWidth * y) * rawImageChannels;
            rawImage[index + 0] = x % 256;
            rawImage[index + 1] = y % 256;
            rawImage[index + 2] = (x + y) % 256;
        }
    }


    OMX_COLOR_FORMATTYPE test[] = {
        OMX_COLOR_FormatUnused,
        OMX_COLOR_Format16bitRGB565,
        OMX_COLOR_Format24bitRGB888,
        OMX_COLOR_Format24bitBGR888,
        OMX_COLOR_Format32bitARGB8888,
        OMX_COLOR_FormatYUV420PackedPlanar,
        OMX_COLOR_FormatYUV422PackedPlanar,
        OMX_COLOR_FormatYCbYCr,
        OMX_COLOR_FormatYCrYCb,
        OMX_COLOR_FormatCbYCrY,
        OMX_COLOR_FormatCrYCbY,
        OMX_COLOR_Format32bitABGR8888,
        OMX_COLOR_Format8bitPalette,




        OMX_COLOR_FormatUnused,
        OMX_COLOR_FormatMonochrome,
        OMX_COLOR_Format8bitRGB332,
        OMX_COLOR_Format12bitRGB444,
        OMX_COLOR_Format16bitARGB4444,
        OMX_COLOR_Format16bitARGB1555,
        OMX_COLOR_Format16bitRGB565,
        OMX_COLOR_Format16bitBGR565,
        OMX_COLOR_Format18bitRGB666,
        OMX_COLOR_Format18bitARGB1665,
        OMX_COLOR_Format19bitARGB1666,
        OMX_COLOR_Format24bitRGB888,
        OMX_COLOR_Format24bitBGR888,
        OMX_COLOR_Format24bitARGB1887,
        OMX_COLOR_Format25bitARGB1888,
        OMX_COLOR_Format32bitBGRA8888,
        OMX_COLOR_Format32bitARGB8888,
        OMX_COLOR_FormatYUV411Planar,
        OMX_COLOR_FormatYUV411PackedPlanar,
        OMX_COLOR_FormatYUV420Planar,
        OMX_COLOR_FormatYUV420PackedPlanar,
        OMX_COLOR_FormatYUV420SemiPlanar,
        OMX_COLOR_FormatYUV422Planar,
        OMX_COLOR_FormatYUV422PackedPlanar,
        OMX_COLOR_FormatYUV422SemiPlanar,
        OMX_COLOR_FormatYCbYCr,
        OMX_COLOR_FormatYCrYCb,
        OMX_COLOR_FormatCbYCrY,
        OMX_COLOR_FormatCrYCbY,
        OMX_COLOR_FormatYUV444Interleaved,
        OMX_COLOR_FormatRawBayer8bit,
        OMX_COLOR_FormatRawBayer10bit,
        OMX_COLOR_FormatRawBayer8bitcompressed,
        OMX_COLOR_FormatL2,
        OMX_COLOR_FormatL4,
        OMX_COLOR_FormatL8,
        OMX_COLOR_FormatL16,
        OMX_COLOR_FormatL24,
        OMX_COLOR_FormatL32,
        OMX_COLOR_FormatYUV420PackedSemiPlanar,
        OMX_COLOR_FormatYUV422PackedSemiPlanar,
        OMX_COLOR_Format18BitBGR666,
        OMX_COLOR_Format24BitARGB6666,
        OMX_COLOR_Format24BitABGR6666,
        OMX_COLOR_Format32bitABGR8888,
        OMX_COLOR_Format8bitPalette,
        OMX_COLOR_FormatYUVUV128,
        OMX_COLOR_FormatRawBayer12bit,
        OMX_COLOR_FormatBRCMEGL,
        OMX_COLOR_FormatBRCMOpaque,
        OMX_COLOR_FormatYVU420PackedPlanar,
        OMX_COLOR_FormatYVU420PackedSemiPlanar,
        OMX_COLOR_FormatRawBayer16bit,
        OMX_COLOR_FormatYUV420_16PackedPlanar,
        OMX_COLOR_FormatYUVUV64_16,
    };
    int numTest = sizeof(test)/sizeof(test[0]);

    for (int i = 0; i < numTest; i++) {
        //size_t outputSize = rawImageWidth * rawImageHeight * sizeof(uint8_t);
        //size_t outputFill = 0;
        //uint8_t *output = malloc(outputSize);


        printf(COLOR_YELLOW "--> %d:\n" COLOR_NC, i);
        OMXContext_s *ctx = omxJPEGEncInit(rawImageWidth, rawImageHeight, sliceHeight, outputQuality, test[i]);


        if (ctx) {
            puts(omxColorFormatTypeEnum(test[i]));
            puts(COLOR_GREEN "WIN" COLOR_NC);
        }

        // for (int i = 0; i < 100; i++) {
        //    omxJPEGEncProcess(ctx, output, &outputFill, outputSize, rawImage, rawImageSize);
        // }
        
        
        //omxJPEGEncDeinit(ctx);
        //free(output);
    }
}
