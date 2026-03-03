// vorbis_encoder.h
#ifndef vorbis_encoder_h
#define vorbis_encoder_h

#include "vorbis/codec.h"
#include "vorbis/vorbisenc.h"


int encode_to_ogg(const float* samples, int num_samples, int sample_rate, const char* output_path);

#endif
