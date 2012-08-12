/* Audiotap shared library: a higher-level interface to TAP shared library
 *
 * TAP->audio: feeds TAP data to TAP library, gets audio data from it and feeds
 * it either to audiofile library (for writing WAV files) or to pablio library
 * (for playing)
 * audio->TAP: gets audio data from audiofile library (for WAV and other audio
 * file formats) or from pablio library (sound card's line in), feeds it to
 * TAP library and gets TAP data from it
 *
 * Audiotap shared library can work without audiofile or without pablio, but
 * it is useless without both.
 *
 * Copyright (c) Fabrizio Gennari, 2003
 *
 * The program is distributed under the GNU Lesser General Public License.
 * See file LESSER-LICENSE.TXT for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "audiofile.h"
#include "portaudio.h"
#include "tapencoder.h"
#include "tapdecoder.h"
#include "audiotap.h"

struct audio2tap_functions {
  enum audiotap_status(*get_pulse)(struct audiotap *audiotap, uint32_t *pulse, uint32_t *raw_pulse);
  enum audiotap_status(*set_buffer)(void *priv, int32_t *buffer, uint32_t bufsize, uint32_t *numframes);
  int (*get_total_len)(struct audiotap *audiotap);
  int (*get_current_pos)(struct audiotap *audiotap);
  int (*is_eof)(struct audiotap *audiotap);
  void (*invert)(struct audiotap *audiotap);
  void (*close)(void *priv);
};

struct tap2audio_functions {
  void                (*set_pulse)(struct audiotap *audiotap, uint32_t pulse);
  uint32_t            (*get_buffer)(struct audiotap *audiotap);
  enum audiotap_status(*dump_buffer)(uint8_t *buffer, uint32_t bufroom, void *priv);
  void                (*close)(void *priv);
};

struct tap_handle {
  FILE *file;
  union{
    struct{
      unsigned char version;
      uint32_t next_pulse;
      uint8_t exhausted;
    };
    struct{
      uint32_t overflow_value;
      uint8_t bits_per_sample;
      uint8_t get_full_waves_in_v2;
      uint8_t temp_get_half_waves_in_v2;
      uint8_t last_was_0;
    };
  };
};

static const char c64_tap_header[] = "C64-TAPE-RAW";
static const char c16_tap_header[] = "C16-TAPE-RAW";

struct audiotap {
  struct tap_enc_t *tapenc;
  struct tap_dec_t *tapdec;
  uint8_t *buffer, bufstart[2048];
  uint32_t bufroom;
  int terminated;
  int has_flushed;
  float factor;
  uint32_t accumulated_samples;
  const struct tap2audio_functions *tap2audio_functions;
  const struct audio2tap_functions *audio2tap_functions;
  void *priv;
};

extern struct audiotap_init_status status;

static const float tap_clocks[TAP_MACHINE_MAX+1][TAP_VIDEOTYPE_MAX+1]={
  {985248,1022727}, /* C64 */
  {1108405,1022727}, /* VIC */
  {886724,894886}  /* C16 */
};

static uint32_t convert_samples(struct audiotap *audiotap, uint32_t raw_samples){
  audiotap->accumulated_samples += raw_samples;
  return (uint32_t)(raw_samples * audiotap->factor);
}

static enum audiotap_status audio2tap_open_common(struct audiotap **audiotap,
                                                  struct tap_enc_t *tapenc,
                                                  uint32_t freq,
                                                  uint8_t machine,
                                                  uint8_t videotype,
                                                  const struct audio2tap_functions *audio2tap_functions,
                                                  void *priv){
  struct audiotap *obj = NULL;
  enum audiotap_status error = AUDIOTAP_WRONG_ARGUMENTS;
  do{
	  if (machine > TAP_MACHINE_MAX || videotype > TAP_VIDEOTYPE_MAX)
      break;
    error = AUDIOTAP_NO_MEMORY;
    obj = calloc(1, sizeof(struct audiotap));	 
    if (obj == NULL)	 
      break;
    obj->priv = priv;
    obj->audio2tap_functions = audio2tap_functions;
    obj->bufroom = 0;
    obj->factor = tap_clocks[machine][videotype] / freq;	 
    obj->tapenc = tapenc;
    error = AUDIOTAP_OK;
  }while(0);
 if (error == AUDIOTAP_OK)
   *audiotap = obj;
 else	 
   audio2tap_close(obj);  *audiotap = obj;
  return error;
}

static enum audiotap_status audio2tap_audio_open_common(struct audiotap **audiotap,
                                                        uint32_t freq,
                                                        struct tapenc_params *tapenc_params,
                                                        uint8_t machine,
                                                        uint8_t videotype,
                                                        uint8_t halfwaves,
                                                        const struct audio2tap_functions *audio2tap_functions,
                                                        void *priv){
  enum audiotap_status error = AUDIOTAP_WRONG_ARGUMENTS;
  struct tap_enc_t *tapenc;

  do{
    if (tapenc_params == NULL)
      break;

    error = AUDIOTAP_NO_MEMORY;

    if (
        (tapenc=tapencoder_init(tapenc_params->min_duration,
                                tapenc_params->sensitivity,
                                tapenc_params->initial_threshold,
                                tapenc_params->inverted,
                                halfwaves
                               )
        )==NULL
       )
      break;
    error = AUDIOTAP_OK;
  }while(0);

  if (error != AUDIOTAP_OK){
    audio2tap_functions->close(priv);
    return error;
  }
  return audio2tap_open_common(audiotap, tapenc, freq, machine, videotype, audio2tap_functions, priv);
}

static enum audiotap_status tapfile_get_pulse(struct audiotap *audiotap, uint32_t *pulse, uint32_t *raw_pulse){
  struct tap_handle *handle = (struct tap_handle *)audiotap->priv;
  uint8_t byte, threebytes[3];

  *pulse = 0;

  while(1){
    if (audiotap->terminated)
      return AUDIOTAP_INTERRUPTED;
    if (fread(&byte, 1, 1, handle->file) != 1)
      return AUDIOTAP_EOF;
    if (byte != 0){
      *raw_pulse = byte;
      *pulse += byte * 8;
      handle->last_was_0 = 0;
      return AUDIOTAP_OK;
    }
    if (handle->version == 0){
      if (handle->last_was_0)
        continue;
      *raw_pulse = 0;
      *pulse = 1000000;
      handle->last_was_0 = 1;
      return AUDIOTAP_OK;
    }
    if (fread(threebytes, 3, 1, handle->file) != 1)
      return AUDIOTAP_EOF;
    *raw_pulse = threebytes[0]        +
                (threebytes[1] <<  8) +
                (threebytes[2] << 16);
    *pulse += *raw_pulse;
    if (*raw_pulse < 0xFFFFFF)
      return AUDIOTAP_OK;
  }
}

static enum audiotap_status tapfile_get_wave(struct audiotap *audiotap, uint32_t *pulse, uint32_t *raw_pulse){
  struct tap_handle *handle = (struct tap_handle *)audiotap->priv;
  enum audiotap_status ret = tapfile_get_pulse(audiotap, pulse, raw_pulse);

  if (ret != AUDIOTAP_OK)
    return ret;

  if (handle->temp_get_half_waves_in_v2)
    handle->temp_get_half_waves_in_v2 = 0;
  else if (handle->get_full_waves_in_v2){
    uint32_t second_half_wave;
    ret = tapfile_get_pulse(audiotap, &second_half_wave, raw_pulse);
    *pulse += second_half_wave;
  }
  return ret;
}

static int tapfile_get_total_len(struct audiotap *audiotap){
  struct tap_handle *handle = (struct tap_handle *)audiotap->priv;
  struct stat stats;

  if (fstat(fileno(handle->file), &stats) == -1)
    return -1;
  return stats.st_size;
}

static int tapfile_get_current_pos(struct audiotap *audiotap){
  struct tap_handle *handle = (struct tap_handle *)audiotap->priv;
  long res;

  if ((res = ftell(handle->file)) == -1)
    return -1;
  return (int)res;
}

static void tapfile_close(void *priv){
  struct tap_handle *handle = (struct tap_handle *)priv;

  fclose(handle->file);
  free(handle);
}

static int tapfile_is_eof(struct audiotap *audiotap){
  struct tap_handle *handle = (struct tap_handle *)audiotap->priv;

  return feof(handle->file);
}

static void tapfile_invert(struct audiotap *audiotap)
{
  struct tap_handle *handle = (struct tap_handle *)audiotap->priv;
  handle->temp_get_half_waves_in_v2 = 1;
}

static const struct audio2tap_functions tapfile_read_functions = {
  tapfile_get_wave,
  NULL,
  tapfile_get_total_len,
  tapfile_get_current_pos,
  tapfile_is_eof,
  tapfile_invert,
  tapfile_close
};

static enum audiotap_status tapfile_init(struct audiotap **audiotap,
                                         const char *file,
                                         uint8_t *machine,
                                         uint8_t *videotype,
                                         uint8_t *halfwaves){
  struct tap_handle *handle;
  enum audiotap_status err;

  handle = malloc(sizeof(struct tap_handle));
  if (handle == NULL)
    return AUDIOTAP_NO_MEMORY;


  do {
    char file_header[12];
    handle->file = fopen(file, "rb");
    if (handle->file == NULL){
      err = errno == ENOENT ? AUDIOTAP_NO_FILE : AUDIOTAP_LIBRARY_ERROR;
      break;
    }
    err = AUDIOTAP_LIBRARY_ERROR;
    if (fread(file_header, sizeof(file_header), 1, handle->file) < 1)
      break;
    err = AUDIOTAP_WRONG_FILETYPE;
    if (
        memcmp(c64_tap_header, file_header, sizeof(file_header))
     && memcmp(c16_tap_header, file_header, sizeof(file_header))
       )
      break;
    if (fread(&handle->version, 1, 1, handle->file) < 1)
      break;
    if (handle->version > 2)
      break;
    if (fread(machine, 1, 1, handle->file) < 1)
      break;
    if (fread(videotype, 1, 1, handle->file) < 1)
      break;
    if (fseek(handle->file, 20, SEEK_SET) != 0)
      break;
    err = AUDIOTAP_OK;
  } while (0);
  if (err == AUDIOTAP_OK){
    handle->get_full_waves_in_v2 = handle->version == 2 && *halfwaves == 0;
    handle->temp_get_half_waves_in_v2 = 0;
    handle->last_was_0 = 0;
    *halfwaves = handle->version == 2;
    return audio2tap_open_common(audiotap,
                                 NULL,
                                 0, /*unused*/
                                 *machine,
                                 *videotype,
                                 &tapfile_read_functions,
                                 handle);
  }
  if (handle->file != NULL)
    fclose(handle->file);
  free(handle);
  return err;
}

static enum audiotap_status audio_get_pulse(struct audiotap *audiotap, uint32_t *pulse, uint32_t *raw_pulse){
  while(!audiotap->terminated && !audiotap->has_flushed){
    uint32_t done_now;
    enum audiotap_status error;
    uint32_t numframes;

    done_now=tapenc_get_pulse(audiotap->tapenc, (int32_t*)audiotap->buffer, audiotap->bufroom, raw_pulse);
    audiotap->buffer += done_now * sizeof(int32_t);
    audiotap->bufroom -= done_now;
    if(*raw_pulse > 0){
      *pulse = convert_samples(audiotap, *raw_pulse);
      return AUDIOTAP_OK;
    }
    
    error = audiotap->audio2tap_functions->set_buffer(audiotap->priv, (int32_t*)audiotap->bufstart, sizeof(audiotap->bufstart) / sizeof(int32_t), &numframes);
    if (error != AUDIOTAP_OK)
      return error;
    if (numframes == 0){
      *raw_pulse = tapenc_flush(audiotap->tapenc);
      *pulse = convert_samples(audiotap, *raw_pulse);
      audiotap->has_flushed=1;
      return AUDIOTAP_OK;
    }
    audiotap->buffer = audiotap->bufstart;
    audiotap->bufroom = numframes;
  }
  return audiotap->terminated ? AUDIOTAP_INTERRUPTED : AUDIOTAP_EOF;
}

static void audio_invert(struct audiotap *audiotap){
  tapenc_invert(audiotap->tapenc);
}

static enum audiotap_status audiofile_set_buffer(void *priv, int32_t *buffer, uint32_t bufsize, uint32_t *numframes) {
  *numframes=afReadFrames((AFfilehandle)priv, AF_DEFAULT_TRACK, buffer, bufsize);
  return *numframes == -1 ? AUDIOTAP_LIBRARY_ERROR : AUDIOTAP_OK;
}

static void audiofile_close(void *priv){
  afCloseFile((AFfilehandle)priv);
}

static int audiofile_get_total_len(struct audiotap *audiotap){
  return (int)(afGetFrameCount((AFfilehandle)audiotap->priv, AF_DEFAULT_TRACK));
}

static int audiofile_get_current_pos(struct audiotap *audiotap){
   return audiotap->accumulated_samples;
}

static int audiofile_is_eof(struct audiotap *audiotap){
  return audiotap->has_flushed;
}

static const struct audio2tap_functions audiofile_read_functions = {
  audio_get_pulse,
  audiofile_set_buffer,
  audiofile_get_total_len,
  audiofile_get_current_pos,
  audiofile_is_eof,
  audio_invert,
  audiofile_close
};

static enum audiotap_status audiofile_read_init(struct audiotap **audiotap,
                                                const char *file,
                                                struct tapenc_params *params,
                                                uint8_t machine,
                                                uint8_t videotype,
                                                uint8_t halfwaves){
  uint32_t freq;
  enum audiotap_status error = AUDIOTAP_LIBRARY_ERROR;
  AFfilehandle fh;

  if (status.audiofile_init_status != LIBRARY_OK
   || status.tapencoder_init_status != LIBRARY_OK)
    return AUDIOTAP_LIBRARY_UNAVAILABLE;
  fh=afOpenFile(file,"r", NULL);
  if (fh == AF_NULL_FILEHANDLE)
    return AUDIOTAP_LIBRARY_ERROR;
  do{
    if ( (freq=(uint32_t)afGetRate(fh, AF_DEFAULT_TRACK)) == -1)
      break;
    if (afSetVirtualChannels(fh, AF_DEFAULT_TRACK, 1) == -1)
      break;
    if (afSetVirtualSampleFormat(fh, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, 32) == -1)
      break;
    if (afGetVirtualFrameSize(fh, AF_DEFAULT_TRACK, 0) != 4)
      break;
    error = AUDIOTAP_OK;
  }while(0);
  if(error != AUDIOTAP_OK){
    afCloseFile(fh);
    return error;
  }
  return audio2tap_audio_open_common(audiotap,
                                     freq,
                                     params,
                                     machine,
                                     videotype,
                                     halfwaves,
                                     &audiofile_read_functions,
                                     fh);
}

static enum audiotap_status dmpfile_get_pulse(struct audiotap *audiotap, uint32_t *pulse, uint32_t *raw_pulse){
  struct tap_handle *handle = (struct tap_handle *)audiotap->priv;

  *pulse = 0;
  while(1){
    uint32_t this_pulse = 0;
    int bitshift;
    for (bitshift = 0; bitshift < handle->bits_per_sample; bitshift += 8){
      uint8_t byte;
      if (fread(&byte, 1, 1, handle->file) != 1)
        return AUDIOTAP_EOF;
      this_pulse += (byte<<bitshift);
    }
    *raw_pulse = this_pulse;
    *pulse += this_pulse;
    if (this_pulse < handle->overflow_value){
      *pulse = (uint32_t)(*pulse * audiotap->factor);
      return AUDIOTAP_OK;
    }
  }
}

static const struct audio2tap_functions dmpfile_read_functions = {
  dmpfile_get_pulse,
  NULL,
  tapfile_get_total_len,
  tapfile_get_current_pos,
  tapfile_is_eof,
  tapfile_invert,
  tapfile_close
};


static enum audiotap_status dmpfile_init(struct audiotap **audiotap,
                                         const char *file,
                                         uint8_t *machine,
                                         uint8_t *videotype,
                                         uint8_t *halfwaves){
  uint32_t freq;
  uint8_t version;
  uint8_t freq_on_file[4];
  const char dmp_file_header[] = "DC2N-TAP-RAW";
  struct tap_handle *handle = malloc(sizeof(struct tap_handle));
  enum audiotap_status err;

  if (handle == NULL)
    return AUDIOTAP_NO_MEMORY;

  do {
    char file_header[12];
    handle->file = fopen(file, "rb");
    if (handle->file == NULL){
      err = errno == ENOENT ? AUDIOTAP_NO_FILE : AUDIOTAP_LIBRARY_ERROR;
      break;
    }
    err = AUDIOTAP_LIBRARY_ERROR;
    if (fread(file_header, sizeof(file_header), 1, handle->file) < 1)
      break;
    err = AUDIOTAP_WRONG_FILETYPE;
    if (memcmp(dmp_file_header, file_header, sizeof(file_header)))
      break;
    if (fread(&version, 1, 1, handle->file) < 1)
      break;
    if (version > 1)
      break;
    if (fread(machine, 1, 1, handle->file) < 1)
      break;
    if (version == 1) {
      handle->get_full_waves_in_v2 = (*machine & (1<<5)) && *halfwaves == 0;
      *halfwaves = *machine & (1<<5) != 0;
      *machine = *machine & 0x0f;
    }
    handle->temp_get_half_waves_in_v2 = 0;
    if (*machine > TAP_MACHINE_MAX)
      break;
    if (fread(videotype, 1, 1, handle->file) < 1)
      break;
    if (*videotype > TAP_VIDEOTYPE_MAX)
      break;
    if (fread(&handle->bits_per_sample, 1, 1, handle->file) < 1)
      break;
    handle->overflow_value = (1<<handle->bits_per_sample) - 1;
    if (fread(freq_on_file, sizeof(freq_on_file), 1, handle->file) < 1)
      break;
    freq = freq_on_file[0]
        + (freq_on_file[1]<< 8)
        + (freq_on_file[2]<<16)
        + (freq_on_file[3]<<24);
    err = AUDIOTAP_OK;
  } while (0);
  if (err == AUDIOTAP_OK)
    return audio2tap_open_common(audiotap,
                                 NULL,
                                 freq,
                                 *machine,
                                 *videotype,
                                 &dmpfile_read_functions,
                                 handle);
  if (handle->file != NULL)
    fclose(handle->file);
  free(handle);
  return err;
}

void audio2tap_invert(struct audiotap *audiotap);

enum audiotap_status audio2tap_open_from_file3(struct audiotap **audiotap,
                                              const char *file,
                                              struct tapenc_params *params,
                                              uint8_t *machine,
                                              uint8_t *videotype,
                                              uint8_t *halfwaves){
  enum audiotap_status error;

  if (machine == NULL || videotype == NULL || halfwaves == NULL)
    return AUDIOTAP_WRONG_ARGUMENTS;
  error = tapfile_init(audiotap, file, machine, videotype, halfwaves);
  if (error == AUDIOTAP_OK)
    return AUDIOTAP_OK;
  if (error != AUDIOTAP_WRONG_FILETYPE)
    return error;
  error = dmpfile_init(audiotap, file, machine, videotype, halfwaves);
  if (error == AUDIOTAP_OK)
    return AUDIOTAP_OK;
  if (error != AUDIOTAP_WRONG_FILETYPE)
    return error;
  if (params == NULL)
    return AUDIOTAP_WRONG_ARGUMENTS;
  return audiofile_read_init(audiotap,
                        file,
                        params,
                        *machine,
                        *videotype,
                        *halfwaves);
}

static enum audiotap_status portaudio_set_buffer(void *priv, int32_t *buffer, uint32_t bufsize, uint32_t *numframes){
  if (Pa_ReadStream((PaStream*)priv, buffer, bufsize) != paNoError)
    return AUDIOTAP_LIBRARY_ERROR;
  *numframes=bufsize;
  return AUDIOTAP_OK;
}

static void portaudio_close(void *priv){
  Pa_StopStream((PaStream*)priv);
  Pa_CloseStream((PaStream*)priv);
}

static int portaudio_get_total_len(struct audiotap *audiotap){
  return -1;
}

static int portaudio_get_current_pos(struct audiotap *audiotap){
  return -1;
}

static int portaudio_is_eof(struct audiotap *audiotap){
  return 0;
}

static const struct audio2tap_functions portaudio_read_functions = {
  audio_get_pulse,
  portaudio_set_buffer,
  portaudio_get_total_len,
  portaudio_get_current_pos,
  portaudio_is_eof,
  audio_invert,
  portaudio_close
};

enum audiotap_status audio2tap_from_soundcard3(struct audiotap **audiotap,
                                              uint32_t freq,
                                              struct tapenc_params *params,
                                              uint8_t machine,
                                              uint8_t videotype,
                                              uint8_t halfwaves){
  enum audiotap_status error=AUDIOTAP_LIBRARY_ERROR;
  PaStream *pastream;

  if (status.portaudio_init_status != LIBRARY_OK
   || status.tapencoder_init_status != LIBRARY_OK)
    return AUDIOTAP_LIBRARY_UNAVAILABLE;
  if (Pa_OpenDefaultStream(&pastream, 1, 0, paInt32, freq, sizeof((*audiotap)->bufstart) / sizeof(int32_t), NULL, NULL) != paNoError)
    return AUDIOTAP_LIBRARY_ERROR;
  if (Pa_StartStream(pastream) != paNoError){
    Pa_CloseStream(pastream);
    return AUDIOTAP_LIBRARY_ERROR;
  }
  return audio2tap_audio_open_common(audiotap,
                                     freq,
                                     params,
                                     machine,
                                     videotype,
                                     halfwaves,
                                     &portaudio_read_functions,
                                     pastream);
}

enum audiotap_status audio2tap_get_pulses(struct audiotap *audiotap, uint32_t *pulse, uint32_t *raw_pulse){
  return audiotap->audio2tap_functions->get_pulse(audiotap, pulse, raw_pulse);
}

int audio2tap_get_total_len(struct audiotap *audiotap){
  return audiotap->audio2tap_functions->get_total_len(audiotap);
}

int audio2tap_get_current_pos(struct audiotap *audiotap){
  return audiotap->audio2tap_functions->get_current_pos(audiotap);
}

int audio2tap_is_eof(struct audiotap *audiotap){
  return audiotap->terminated == 1 || audiotap->audio2tap_functions->is_eof(audiotap);
}

int32_t audio2tap_get_current_sound_level(struct audiotap *audiotap){
  if (!audiotap->tapenc)
    return -1;
  return tapenc_get_max(audiotap->tapenc);
}

void audio2tap_invert(struct audiotap *audiotap)
{
  audiotap->audio2tap_functions->invert(audiotap);
}

void audiotap_terminate(struct audiotap *audiotap){
  audiotap->terminated = 1;
}

int audiotap_is_terminated(struct audiotap *audiotap){
  return audiotap->terminated;
}

void audio2tap_close(struct audiotap *audiotap){
  if (audiotap){
    audiotap->audio2tap_functions->close(audiotap->priv);
    tapencoder_exit(audiotap->tapenc);
  }
  free(audiotap);
}

/* ----------------- TAP2AUDIO ----------------- */

static void tapfile_set_pulse(struct audiotap *audiotap, uint32_t pulse){
  struct tap_handle *handle = (struct tap_handle *)audiotap->priv;
  handle->next_pulse = pulse;
  handle->exhausted = 0;
}

static uint32_t tapfile_get_buffer(struct audiotap *audiotap){
  struct tap_handle *handle = (struct tap_handle *)audiotap->priv;
  uint8_t *buffer = audiotap->bufstart;
  uint32_t bufroom = sizeof(audiotap->bufstart);
  uint8_t not_enough_bufroom = 0;

  if(handle->version > 0){
    while(handle->next_pulse >= 0xFFFFFF){
      if(bufroom < 4){
        not_enough_bufroom = 1;
        break;
      }
      *buffer++ = 0;
      bufroom--;
      *buffer++ = 0xFF;
      bufroom--;
      *buffer++ = 0xFF;
      bufroom--;
      *buffer++ = 0xFF;
      bufroom--;
      handle->next_pulse -= 0xFFFFFF;
      if (handle->next_pulse == 0)
        handle->exhausted = 1;
    }
  }

  if(!not_enough_bufroom){
    if(handle->version > 0 && (handle->next_pulse >= 0x800 || handle->exhausted)){
      if (bufroom >= 4){
        *buffer++ = 0;
        bufroom--;
        *buffer++ = (handle->next_pulse      )&0xFF;
        bufroom--;
        *buffer++ = (handle->next_pulse >>  8)&0xFF;
        bufroom--;
        *buffer++ = (handle->next_pulse >> 16)&0xFF;
        bufroom--;
        handle->next_pulse = 0;
        handle->exhausted = 0;
      }
    }
    else if (bufroom > 0 && handle->next_pulse > 0){
      if (handle->next_pulse >= 0x800 && handle->version == 0)
        handle->next_pulse = 0;
      else if (handle->next_pulse > 0x7F8)
        handle->next_pulse = 0x7F8;
      *buffer++ = (handle->next_pulse + 7) / 8;
      bufroom--;
      handle->next_pulse = 0;
    }
  }
  return sizeof(audiotap->bufstart) - bufroom;
}

static enum audiotap_status tapfile_dump_buffer(uint8_t *buffer, uint32_t bufsize, void *priv){
  return fwrite(buffer, bufsize, 1, ((struct tap_handle *)priv)->file) == 1
   ? AUDIOTAP_OK
   : AUDIOTAP_LIBRARY_ERROR;
}

static void tapfile_write_close(void *file){
  struct tap_handle *handle = file;
  long size;
  unsigned char size_header[4];

  do{
    if ((fseek(handle->file, 0, SEEK_END)) != 0)
      break;
    if ((size = ftell(handle->file)) == -1)
      break;
    size -= 20;
    if (size < 0)
      break;
    size_header[0] = (unsigned char) (size & 0xFF);
    size_header[1] = (unsigned char) ((size >> 8) & 0xFF);
    size_header[2] = (unsigned char) ((size >> 16) & 0xFF);
    size_header[3] = (unsigned char) ((size >> 24) & 0xFF);
    if ((fseek(handle->file, 16, SEEK_SET)) != 0)
      break;
    fwrite(size_header, 4, 1, handle->file);
  }while(0);
  fclose(handle->file);
  free(handle);
}

static const struct tap2audio_functions tapfile_write_functions = {
  tapfile_set_pulse,
  tapfile_get_buffer,
  tapfile_dump_buffer,
  tapfile_write_close,
};

static void audio_set_pulse(struct audiotap *audiotap, uint32_t pulse){
  tapdec_set_pulse(audiotap->tapdec, (uint32_t)(pulse / audiotap->factor));
}

static uint32_t audio_get_buffer(struct audiotap *audiotap){
  return tapdec_get_buffer(audiotap->tapdec, (int32_t*)audiotap->bufstart, (uint32_t)(sizeof(audiotap->bufstart) / sizeof(uint32_t)) );
}

static enum audiotap_status audiofile_dump_buffer(uint8_t *buffer, uint32_t bufsize, void *priv){
  return (afWriteFrames((AFfilehandle)priv, AF_DEFAULT_TRACK, buffer, (int)bufsize) == (int)bufsize) ? AUDIOTAP_OK : AUDIOTAP_LIBRARY_ERROR;
}

static const struct tap2audio_functions audiofile_write_functions = {
  audio_set_pulse,
  audio_get_buffer,
  audiofile_dump_buffer,
  audiofile_close,
};

static enum audiotap_status portaudio_dump_buffer(uint8_t *buffer, uint32_t bufsize, void *priv){
  return Pa_WriteStream((PaStream*)priv, buffer, bufsize) == paNoError ? AUDIOTAP_OK : AUDIOTAP_LIBRARY_ERROR;
}

static const struct tap2audio_functions portaudio_write_functions = {
  audio_set_pulse,
  audio_get_buffer,
  portaudio_dump_buffer,
  portaudio_close,
};

static enum audiotap_status tap2audio_open_common(struct audiotap **audiotap
                                                 ,struct tapdec_params *params
                                                 ,uint32_t freq
                                                 ,uint8_t machine
                                                 ,uint8_t videotype
                                                 ,const struct tap2audio_functions *functions
                                                 ,void *priv){
  struct audiotap *obj = NULL;
  enum audiotap_status error = AUDIOTAP_WRONG_ARGUMENTS;
  if (machine <= TAP_MACHINE_MAX && videotype <= TAP_VIDEOTYPE_MAX){
    error = AUDIOTAP_NO_MEMORY;
    obj=calloc(1, sizeof(struct audiotap));
    if (obj != NULL){
      obj->factor = tap_clocks[machine][videotype] / freq;
      obj->priv = priv;
      obj->tap2audio_functions = functions;
      if (params == NULL)
        error = AUDIOTAP_OK;
      else{
        enum tapdec_waveform waveform =
          params->waveform == AUDIOTAP_WAVE_TRIANGLE ? TAPDEC_TRIANGLE :
          params->waveform == AUDIOTAP_WAVE_SQUARE   ? TAPDEC_SQUARE   :
                                                       TAPDEC_SINE;
        if(
           (obj->tapdec = tapdecoder_init(params->volume,
                                          params->inverted,
                                          params->halfwaves,
                                          waveform))
            != NULL
          )
          error = AUDIOTAP_OK;
      }
    }
  }

  if (error != AUDIOTAP_OK) {
    functions->close(priv);
    free(obj);
    obj = NULL;
  }

  *audiotap = obj;
  return error;
}

enum audiotap_status tap2audio_open_to_soundcard3(struct audiotap **audiotap
                                                 ,struct tapdec_params *params
                                                 ,uint32_t freq
                                                 ,uint8_t machine
                                                 ,uint8_t videotype){
  PaStream *pastream;

  if (status.portaudio_init_status != LIBRARY_OK
   || status.tapdecoder_init_status != LIBRARY_OK)
    return AUDIOTAP_LIBRARY_UNAVAILABLE;
  if (Pa_OpenDefaultStream(&pastream, 0, 1, paInt32, freq, sizeof(((struct audiotap*)NULL)->bufstart), NULL, NULL) != paNoError)
    return AUDIOTAP_LIBRARY_ERROR;
  if (Pa_StartStream(pastream) != paNoError){
    Pa_CloseStream(pastream);
    return AUDIOTAP_LIBRARY_ERROR;
  }

  return tap2audio_open_common(audiotap
                              ,params
                              ,freq
                              ,machine
                              ,videotype
                              ,&portaudio_write_functions
                              ,pastream);
}

enum audiotap_status tap2audio_open_to_wavfile3(struct audiotap **audiotap
                                               ,const char *file
                                               ,struct tapdec_params *params
                                               ,uint32_t freq
                                               ,uint8_t machine
                                               ,uint8_t videotype){
  AFfilehandle fh;
  AFfilesetup setup;

  if (status.audiofile_init_status != LIBRARY_OK
   || status.tapdecoder_init_status != LIBRARY_OK)
    return AUDIOTAP_LIBRARY_UNAVAILABLE;
  setup=afNewFileSetup();
  if (setup == AF_NULL_FILESETUP)
    return AUDIOTAP_NO_MEMORY;
  afInitRate(setup, AF_DEFAULT_TRACK, freq);
  afInitChannels(setup, AF_DEFAULT_TRACK, 1);
  afInitFileFormat(setup, AF_FILE_WAVE);
  afInitSampleFormat(setup, AF_DEFAULT_TRACK, AF_SAMPFMT_UNSIGNED, 8);
  fh=afOpenFile(file,"w", setup);
  afFreeFileSetup(setup);
  if (fh == AF_NULL_FILEHANDLE)
    return AUDIOTAP_LIBRARY_ERROR;
  if (afSetVirtualSampleFormat(fh, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, 32) == -1
   || afGetVirtualFrameSize(fh, AF_DEFAULT_TRACK, 0) != 4){
    afCloseFile(fh);
    return AUDIOTAP_LIBRARY_ERROR;
  }

  return tap2audio_open_common(audiotap
                              ,params
                              ,freq
                              ,machine
                              ,videotype
                              ,&audiofile_write_functions
                              ,fh);
}

enum audiotap_status tap2audio_open_to_tapfile2(struct audiotap **audiotap
                                               ,const char *name
                                               ,uint8_t version
                                               ,uint8_t machine
                                               ,uint8_t videotype){
  struct tap_handle *handle;
  const char *tap_header = (machine == TAP_MACHINE_C16 ? c16_tap_header : c64_tap_header);
  enum audiotap_status error = AUDIOTAP_LIBRARY_ERROR;

  if (version > 2)
    return AUDIOTAP_WRONG_ARGUMENTS;
  if((handle = malloc(sizeof(struct tap_handle))) == NULL)
    return AUDIOTAP_NO_MEMORY;

  handle->file = fopen(name, "wb");
  if (handle->file == NULL) {
    free(handle);
    return AUDIOTAP_NO_FILE;
  }

  handle->version = version;

  do{
    if (fwrite(tap_header, strlen(tap_header), 1, handle->file) != 1)
      break;
    if (fwrite(&version, 1, 1, handle->file) != 1)
      break;
    if (fwrite(&machine, 1, 1, handle->file) != 1)
      break;
    if (fwrite(&videotype, 1, 1, handle->file) != 1)
      break;
    if (fseek(handle->file, 20, SEEK_SET) != 0)
      break;
    error = AUDIOTAP_OK;
  }while(0);

  if(error != AUDIOTAP_OK){
    fclose(handle->file);
    free(handle);
    return error;
  }
  return tap2audio_open_common(audiotap
                              ,NULL
                              ,0 /* unused */
                              ,machine
                              ,videotype
                              ,&tapfile_write_functions
                              ,handle);
}

enum audiotap_status tap2audio_set_pulse(struct audiotap *audiotap, uint32_t pulse){
  uint32_t numframes;
  enum audiotap_status error = AUDIOTAP_OK;

  audiotap->tap2audio_functions->set_pulse(audiotap, pulse);

  while(error == AUDIOTAP_OK && (numframes = audiotap->tap2audio_functions->get_buffer(audiotap)) > 0){
    error = audiotap->terminated ? AUDIOTAP_INTERRUPTED :
    audiotap->tap2audio_functions->dump_buffer(audiotap->bufstart, numframes, audiotap->priv);
  }

  return error;
}

void tap2audio_close(struct audiotap *audiotap){
  audiotap->tap2audio_functions->close(audiotap->priv);
  tapdecoder_exit(audiotap->tapdec);
  free(audiotap);
}
