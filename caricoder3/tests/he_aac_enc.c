/*
 * caricoder3 test helper: 16-bit stereo 48 kHz PCM on stdin -> HE-AAC
 * (ADTS) on stdout, through libfdk-aac. The ffmpeg in most distributions
 * cannot make HE-AAC, and the tests need a stream of it to transcode from.
 *
 *   cc -o he_aac_enc he_aac_enc.c -lfdk-aac
 *   he_aac_enc [aot]      aot 5 = HE-AAC (default), 29 = HE-AAC v2
 */
#include <stdio.h>
#include <stdlib.h>
#include <fdk-aac/aacenc_lib.h>

int main(int argc, char **argv)
{
    HANDLE_AACENCODER h;
    AACENC_InfoStruct info;
    int aot = argc > 1 ? atoi(argv[1]) : 5;
    static short pcm[2 * 4096];
    static unsigned char out[16384];

    if (aacEncOpen(&h, 0, 2) != AACENC_OK ||
        aacEncoder_SetParam(h, AACENC_AOT, (UINT)aot) != AACENC_OK ||
        aacEncoder_SetParam(h, AACENC_SAMPLERATE, 48000) != AACENC_OK ||
        aacEncoder_SetParam(h, AACENC_CHANNELMODE, MODE_2) != AACENC_OK ||
        aacEncoder_SetParam(h, AACENC_CHANNELORDER, 1) != AACENC_OK ||
        aacEncoder_SetParam(h, AACENC_BITRATE, aot == 29 ? 32000 : 64000) != AACENC_OK ||
        aacEncoder_SetParam(h, AACENC_TRANSMUX, TT_MP4_ADTS) != AACENC_OK ||
        aacEncEncode(h, NULL, NULL, NULL, NULL) != AACENC_OK ||
        aacEncInfo(h, &info) != AACENC_OK) {
        fprintf(stderr, "encoder setup failed\n");
        return 1;
    }
    for (;;) {
        AACENC_BufDesc ib = { 0 }, ob = { 0 };
        AACENC_InArgs ia = { 0 };
        AACENC_OutArgs oa = { 0 };
        void *ip = pcm, *op = out;
        INT iid = IN_AUDIO_DATA, oid = OUT_BITSTREAM_DATA;
        INT isz, iel = 2, osz = sizeof(out), oel = 1;
        size_t n = fread(pcm, 4, info.frameLength, stdin);

        if (n == 0)
            break;
        for (size_t i = n * 2; i < 2u * info.frameLength; i++)
            pcm[i] = 0;
        isz = (INT)(info.frameLength * 4);
        ib.numBufs = 1; ib.bufs = &ip; ib.bufferIdentifiers = &iid;
        ib.bufSizes = &isz; ib.bufElSizes = &iel;
        ob.numBufs = 1; ob.bufs = &op; ob.bufferIdentifiers = &oid;
        ob.bufSizes = &osz; ob.bufElSizes = &oel;
        ia.numInSamples = (INT)(info.frameLength * 2);
        if (aacEncEncode(h, &ib, &ob, &ia, &oa) != AACENC_OK)
            return 1;
        fwrite(out, 1, (size_t)oa.numOutBytes, stdout);
    }
    aacEncClose(&h);
    return 0;
}
