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

#include <stdlib.h>
#ifndef _WIN32
#include <dlfcn.h>
#else
#include <windows.h>
#endif
#include <sys/types.h>
#include "audiofile.h"
#include "pablio.h"
#include "tap.h"
#include "audiotap.h"

#define AUDIOTAP_BUFSIZE 512

struct audiotap {
  PaStream *pablio;
  AFfilehandle file;
  struct tap_t *tap;
  int32_t *buffer, *bufstart;
  u_int32_t bufroom;
  int terminated;
};

static struct audiotap_init_status status = {
  LIBRARY_UNINIT,
  LIBRARY_UNINIT
};

static enum library_status audiofile_init(){
#if defined(WIN32)
  HINSTANCE handle;
#else
  void *handle;
#endif

static const char* audiofile_library_name =
#if (defined _MSC_VER)
#ifdef _DEBUG
"audiofiled.dll"
#else //_MSC_VER and not DEBUG
"audiofile.dll"
#endif //_MSC_VER,DEBUG
#elif (defined _WIN32 || defined __CYGWIN__)
#ifdef AUDIOFILE_COMPILED_WITH_CYGWIN_SHELL
"cygaudiofile-0.dll"
#else //not AUDIOFILE_COMPILED_WITH_CYGWIN_SHELL
"libaudiofile-0.dll"
#endif //_WIN32 or __CYGWIN__,AUDIOFILE_COMPILED_WITH_CYGWIN_SHELL
#else //not _MSC_VER, not _WIN32, not __CYGWIN__
"libaudiofile.so.0"
#endif//_MSC_VER,_WIN32 or __CYGWIN__
;

#if defined(WIN32)
  handle=LoadLibraryA(audiofile_library_name);
#else
  handle=dlopen(audiofile_library_name, RTLD_LAZY);
#endif
  if (!handle) {
    return LIBRARY_MISSING;
  };

#if defined(WIN32)
  afOpenFile = (void*)GetProcAddress(handle, "afOpenFile");
#else
  afOpenFile = dlsym(handle, "afOpenFile");
#endif
  if (!afOpenFile) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afCloseFile = (void*)GetProcAddress(handle, "afCloseFile");
#else
  afCloseFile = dlsym(handle, "afCloseFile");
#endif
  if (!afCloseFile) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afReadFrames = (void*)GetProcAddress(handle, "afReadFrames");
#else
  afReadFrames = dlsym(handle, "afReadFrames");
#endif
  if (!afReadFrames) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afWriteFrames = (void*)GetProcAddress(handle, "afWriteFrames");
#else
  afWriteFrames = dlsym(handle, "afWriteFrames");
#endif
  if (!afWriteFrames) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afSetVirtualChannels = (void*)GetProcAddress(handle, "afSetVirtualChannels");
#else
  afSetVirtualChannels = dlsym(handle, "afSetVirtualChannels");
#endif
  if (!afSetVirtualChannels) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afGetSampleFormat = (void*)GetProcAddress(handle, "afGetSampleFormat");
#else
  afGetSampleFormat = dlsym(handle, "afGetSampleFormat");
#endif
  if (!afGetSampleFormat) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afSetVirtualSampleFormat = (void*)GetProcAddress(handle, "afSetVirtualSampleFormat");
#else
  afSetVirtualSampleFormat = dlsym(handle, "afSetVirtualSampleFormat");
#endif
  if (!afSetVirtualSampleFormat) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afGetVirtualFrameSize = (void*)GetProcAddress(handle, "afGetVirtualFrameSize");
#else
  afGetVirtualFrameSize = dlsym(handle, "afGetVirtualFrameSize");
#endif
  if (!afGetVirtualFrameSize) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afGetFrameCount = (void*)GetProcAddress(handle, "afGetFrameCount");
#else
  afGetFrameCount = dlsym(handle, "afGetFrameCount");
#endif
  if (!afGetFrameCount) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afTellFrame = (void*)GetProcAddress(handle, "afTellFrame");
#else
  afTellFrame = dlsym(handle, "afTellFrame");
#endif
  if (!afTellFrame) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afInitFileFormat = (void*)GetProcAddress(handle, "afInitFileFormat");
#else
  afInitFileFormat = dlsym(handle, "afInitFileFormat");
#endif
  if (!afInitFileFormat) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afInitSampleFormat = (void*)GetProcAddress(handle, "afInitSampleFormat");
#else
  afInitSampleFormat = dlsym(handle, "afInitSampleFormat");
#endif
  if (!afInitSampleFormat) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afInitChannels = (void*)GetProcAddress(handle, "afInitChannels");
#else
  afInitChannels = dlsym(handle, "afInitChannels");
#endif
  if (!afInitChannels) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afInitRate = (void*)GetProcAddress(handle, "afInitRate");
#else
  afInitRate = dlsym(handle, "afInitRate");
#endif
  if (!afInitRate) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afGetRate = (void*)GetProcAddress(handle, "afGetRate");
#else
  afGetRate = dlsym(handle, "afGetRate");
#endif
  if (!afGetRate) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afNewFileSetup = (void*)GetProcAddress(handle, "afNewFileSetup");
#else
  afNewFileSetup = dlsym(handle, "afNewFileSetup");
#endif
  if (!afNewFileSetup) {
    return LIBRARY_SYMBOLS_MISSING;
  };

#if defined(WIN32)
  afFreeFileSetup = (void*)GetProcAddress(handle, "afFreeFileSetup");
#else
  afFreeFileSetup = dlsym(handle, "afFreeFileSetup");
#endif
  if (!afFreeFileSetup) {
    return LIBRARY_SYMBOLS_MISSING;
  };

  return LIBRARY_OK;
}

static enum library_status pablio_init(){
  void *handle;

  static const char* portaudio_library_name =
#if (defined _MSC_VER)
    "portaudio_x86.dll"
#elif (defined _WIN32 || defined __CYGWIN__) //not _MSC_VER
#ifdef PORTAUDIO_COMPILED_WITH_CYGWIN_SHELL
    "cygportaudio-2.dll"
#else //not PORTAUDIO_COMPILED_WITH_CYGWIN_SHELL
    "libportaudio-2.dll"
#endif //_WIN32 or __CYGWIN__,AUDIOFILE_COMPILED_WITH_CYGWIN_SHELL
#else //not _MSC_VER, not _WIN32, not __CYGWIN__
    "libportaudio.so.2"
#endif//_MSC_VER,_WIN32 or __CYGWIN__
    ;


#if defined(WIN32)
  handle=LoadLibraryA(portaudio_library_name);
#else
  handle=dlopen(portaudio_library_name, RTLD_LAZY);
#endif
  if (!handle)
    return LIBRARY_MISSING;

#if defined(WIN32)
  Pa_Initialize = (void*)GetProcAddress(handle, "Pa_Initialize");
#else
  Pa_Initialize = dlsym(handle, "Pa_Initialize");
#endif
   if (!Pa_Initialize) {
    return LIBRARY_SYMBOLS_MISSING;
  }

#if defined(WIN32)
  Pa_Terminate = (void*)GetProcAddress(handle, "Pa_Terminate");
#else
  Pa_Terminate = dlsym(handle, "Pa_Terminate");
#endif
   if (!Pa_Terminate) {
    return LIBRARY_SYMBOLS_MISSING;
  }

#if defined(WIN32)
  Pa_OpenDefaultStream = (void*)GetProcAddress(handle, "Pa_OpenDefaultStream");
#else
  Pa_OpenDefaultStream = dlsym(handle, "Pa_OpenDefaultStream");
#endif
   if (!Pa_OpenDefaultStream) {
    return LIBRARY_SYMBOLS_MISSING;
  }

#if defined(WIN32)
   Pa_CloseStream = (void*)GetProcAddress(handle, "Pa_CloseStream");
#else
   Pa_CloseStream = dlsym(handle, "Pa_CloseStream");
#endif
   if (!Pa_CloseStream) {
     return LIBRARY_SYMBOLS_MISSING;
   }

#if defined(WIN32)
   Pa_StartStream = (void*)GetProcAddress(handle, "Pa_StartStream");
#else
   Pa_StartStream = dlsym(handle, "Pa_StartStream");
#endif
   if (!Pa_StartStream) {
     return LIBRARY_SYMBOLS_MISSING;
   }

#if defined(WIN32)
   Pa_StopStream = (void*)GetProcAddress(handle, "Pa_StopStream");
#else
   Pa_StopStream = dlsym(handle, "Pa_StopStream");
#endif
   if (!Pa_StopStream) {
     return LIBRARY_SYMBOLS_MISSING;
   }

#if defined(WIN32)
   Pa_ReadStream = (void*)GetProcAddress(handle, "Pa_ReadStream");
#else
   Pa_ReadStream = dlsym(handle, "Pa_ReadStream");
#endif
   if (!Pa_ReadStream) {
     return LIBRARY_SYMBOLS_MISSING;
   }

#if defined(WIN32)
   Pa_WriteStream = (void*)GetProcAddress(handle, "Pa_WriteStream");
#else
   Pa_WriteStream = dlsym(handle, "Pa_WriteStream");
#endif
   if (!Pa_WriteStream) {
     return LIBRARY_SYMBOLS_MISSING;
   }

  if (Pa_Initialize() != paNoError)
  {
    return LIBRARY_INIT_FAILED;
  }

  return LIBRARY_OK;
}

struct audiotap_init_status audiotap_initialize(void){
  status.audiofile_init_status = audiofile_init();
  status.pablio_init_status = pablio_init();
  return status;
}

enum audiotap_status audio2tap_open(struct audiotap **audiotap,
				    char *file,
					u_int32_t freq,
				    u_int32_t min_duration,
				    u_int32_t min_height,
				    int inverted){
  struct audiotap *obj;
  enum audiotap_status error;

  obj=calloc(1, sizeof(struct audiotap));
  if (obj == NULL) return AUDIOTAP_NO_MEMORY;

  if (file == NULL){
    if (status.pablio_init_status != LIBRARY_OK){
      free(obj);
      return AUDIOTAP_LIBRARY_UNAVAILABLE;
    }
    error=AUDIOTAP_LIBRARY_ERROR;
    if (Pa_OpenDefaultStream(&obj->pablio, 1, 0, paInt32, freq, AUDIOTAP_BUFSIZE, NULL, NULL) != paNoError)
      goto err;
    if (Pa_StartStream(obj->pablio) != paNoError)
    {
      Pa_CloseStream(obj->pablio);
      goto err;
    }
  }
  else{
    if (status.audiofile_init_status != LIBRARY_OK){
      free(obj);
      return AUDIOTAP_LIBRARY_UNAVAILABLE;
    }
    error=AUDIOTAP_NO_FILE;
    obj->file=afOpenFile(file,"r", NULL);
    if (obj->file == AF_NULL_FILEHANDLE)
      goto err;
    error=AUDIOTAP_LIBRARY_ERROR;
    if ( (freq=afGetRate(obj->file, AF_DEFAULT_TRACK)) == -1) /* freq passed by the user is discarded */{
      afCloseFile(obj->file);
      goto err;
    }
    if (afSetVirtualChannels(obj->file, AF_DEFAULT_TRACK, 1) == -1){
      afCloseFile(obj->file);
      goto err;
    }
    if (afSetVirtualSampleFormat(obj->file, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, 32) == -1){
      afCloseFile(obj->file);
      goto err;
    }
    if (afGetVirtualFrameSize(obj->file, AF_DEFAULT_TRACK, 0) != 4){
      afCloseFile(obj->file);
      goto err;
    }
  }
  error=AUDIOTAP_NO_MEMORY;
  if ((obj->tap=tap_fromaudio_init(freq, min_duration, min_height, inverted))==NULL)
    goto err;
  *audiotap = obj;
  return AUDIOTAP_OK;

 err:
  free(obj);
  return error;
}

enum audiotap_status audio2tap_set_machine(struct audiotap *audiotap, u_int8_t machine, u_int8_t videotype){
  if (tap_set_machine(audiotap->tap,machine,videotype)!=TAP_OK)
    return AUDIOTAP_LIBRARY_ERROR;
  return AUDIOTAP_OK;
}

enum audiotap_status audio2tap_get_pulse(struct audiotap *audiotap, u_int32_t *pulse){
  int numframes;

  while(1){
    if(audiotap->terminated)
      return AUDIOTAP_INTERRUPTED;

    *pulse=tap_get_pulse(audiotap->tap);
    if(*pulse < TAP_NO_MORE_SAMPLES)
      return AUDIOTAP_OK;

    if (tap_is_flushing(audiotap->tap))
      return AUDIOTAP_EOF;
    
    if (audiotap->buffer != NULL)
      free(audiotap->buffer);
    audiotap->buffer=malloc(AUDIOTAP_BUFSIZE*sizeof(int32_t));
    if (audiotap->buffer == NULL) return AUDIOTAP_NO_MEMORY;

    if (audiotap->file != NULL){
      numframes=afReadFrames(audiotap->file, AF_DEFAULT_TRACK, audiotap->buffer, AUDIOTAP_BUFSIZE);
      if (numframes == -1) return AUDIOTAP_LIBRARY_ERROR;
      if (numframes == 0)
      {
        tap_flush(audiotap->tap);
        continue;
      }
    }
    else{
      if (Pa_ReadStream(audiotap->pablio, audiotap->buffer, AUDIOTAP_BUFSIZE) != paNoError)
        return AUDIOTAP_LIBRARY_ERROR;
      numframes=AUDIOTAP_BUFSIZE;
    }
    tap_set_buffer(audiotap->tap, audiotap->buffer, numframes);
  }
}

int audio2tap_get_total_len(struct audiotap *audiotap){
  if (audiotap->file == 0) return -1;
  return (int)(afGetFrameCount(audiotap->file, AF_DEFAULT_TRACK));
}

int audio2tap_get_current_pos(struct audiotap *audiotap){
   if (audiotap->file == 0 || audiotap->tap == 0) return -1;
   return tap_get_pos(audiotap->tap);
}

int32_t audio2tap_get_current_sound_level(struct audiotap *audiotap){
	if (!audiotap->tap)
		return -1;
	return tap_get_max(audiotap->tap);
};

void audiotap_terminate(struct audiotap *audiotap){
  audiotap->terminated = 1;
}

void audio2tap_close(struct audiotap *audiotap){
  if(audiotap->file != 0)
    afCloseFile(audiotap->file);
  else if(audiotap->pablio != NULL){
    Pa_StopStream(audiotap->pablio);
    Pa_CloseStream(audiotap->pablio);
  }
  tap_exit(audiotap->tap);
  if (audiotap->buffer)
	  free(audiotap->buffer);
  free(audiotap);
}

enum audiotap_status tap2audio_open(struct audiotap **audiotap, char *file, int32_t volume, u_int32_t freq, int inverted, u_int8_t machine, u_int8_t videotype){
  struct audiotap *obj;
  enum audiotap_status error;
  AFfilesetup setup;

  obj=calloc(1, sizeof(struct audiotap));
  if (obj == NULL) return AUDIOTAP_NO_MEMORY;

  obj->bufstart=obj->buffer = malloc(AUDIOTAP_BUFSIZE*sizeof(int32_t));
  if (obj->buffer == NULL) {
	  free(obj);
	  return AUDIOTAP_NO_MEMORY;
  }
  obj->bufroom=AUDIOTAP_BUFSIZE;

  if (file == NULL){
    if (status.pablio_init_status != LIBRARY_OK){
      error = AUDIOTAP_LIBRARY_UNAVAILABLE;
      goto err;
    }
    error=AUDIOTAP_LIBRARY_ERROR;
    if (Pa_OpenDefaultStream(&obj->pablio, 0, 1, paInt32, freq, AUDIOTAP_BUFSIZE, NULL, NULL) != paNoError)
      goto err;
    if (Pa_StartStream(obj->pablio) != paNoError)
    {
      Pa_CloseStream(obj->pablio);
      goto err;
    }
  }
  else{
    if (status.audiofile_init_status != LIBRARY_OK){
	error = AUDIOTAP_LIBRARY_UNAVAILABLE;
	goto err;
    }
    error=AUDIOTAP_NO_MEMORY;
    setup=afNewFileSetup();
    if (setup == AF_NULL_FILESETUP)
      goto err;
    afInitRate(setup, AF_DEFAULT_TRACK, freq);
    afInitChannels(setup, AF_DEFAULT_TRACK, 1);
    afInitFileFormat(setup, AF_FILE_WAVE);
    afInitSampleFormat(setup, AF_DEFAULT_TRACK, AF_SAMPFMT_UNSIGNED, 8);
    obj->file=afOpenFile(file,"w", setup);
    afFreeFileSetup(setup);
    if (obj->file == AF_NULL_FILEHANDLE)
      goto err;
    error=AUDIOTAP_LIBRARY_ERROR;
    if (afSetVirtualSampleFormat(obj->file, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, 32) == -1)
      goto err;
    if (afGetVirtualFrameSize(obj->file, AF_DEFAULT_TRACK, 0) != 4)
      goto err;
  }
  error=AUDIOTAP_NO_MEMORY;
  if ((obj->tap=tap_toaudio_init(freq, volume, inverted))==NULL)
    goto err;
  error=AUDIOTAP_LIBRARY_ERROR;
  if (tap_set_machine(obj->tap,machine,videotype)!=TAP_OK){
    tap_exit(obj->tap);
    goto err;
  }

  *audiotap = obj;
  return AUDIOTAP_OK;

 err:
  free(obj->buffer);
  free(obj);
  return error;
}

enum audiotap_status tap2audio_set_pulse(struct audiotap *audiotap, u_int32_t pulse){
  int numframes, numwritten;

  tap_set_pulse(audiotap->tap, pulse);

  while(1){
    if(audiotap->terminated)
      return AUDIOTAP_INTERRUPTED;

    numframes=tap_get_buffer(audiotap->tap, audiotap->buffer, audiotap->bufroom);
    audiotap->buffer += numframes;
    audiotap->bufroom -= numframes;

    if (audiotap->bufroom != 0)
      return AUDIOTAP_OK;

    if (audiotap->file != NULL){
      numwritten=afWriteFrames(audiotap->file, AF_DEFAULT_TRACK, audiotap->bufstart, audiotap->buffer - audiotap->bufstart);
      if (numwritten == -1 || audiotap->buffer - audiotap->bufstart != numwritten) return AUDIOTAP_LIBRARY_ERROR;
    }
    else {
      if(Pa_WriteStream(audiotap->pablio, audiotap->bufstart, audiotap->buffer - audiotap->bufstart) != paNoError)
        return AUDIOTAP_LIBRARY_ERROR;
    }

    free(audiotap->bufstart);
    audiotap->bufstart=audiotap->buffer=malloc(AUDIOTAP_BUFSIZE*sizeof(int32_t));
    if (audiotap->buffer == NULL) return AUDIOTAP_NO_MEMORY;
    audiotap->bufroom = AUDIOTAP_BUFSIZE;
  }
}

void tap2audio_close(struct audiotap *audiotap){
  if(audiotap->file != 0) {
	afWriteFrames(audiotap->file, AF_DEFAULT_TRACK, audiotap->bufstart, audiotap->buffer - audiotap->bufstart);
	afCloseFile(audiotap->file);
  }
  else if(audiotap->pablio != NULL){
    Pa_WriteStream(audiotap->pablio, audiotap->bufstart, audiotap->buffer - audiotap->bufstart);
    Pa_StopStream(audiotap->pablio);
    Pa_CloseStream(audiotap->pablio);
  }
  tap_exit(audiotap->tap);
  free(audiotap->bufstart);
  free(audiotap);
}
