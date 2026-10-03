// CNE and Origin API adapters. The backend remains SDL2.
// Unsupported optional codecs return null/false; no SDL3 runtime is required.
#if defined(HX_WINDOWS) || defined(HX_MACOS) || defined(HX_LINUX)
#define NEKO_COMPATIBLE
#endif
#include <system/CFFI.h>
#include <media/AudioBuffer.h>
#include <utils/Bytes.h>
#include "../graphics/opengl/OpenGL.h"
#include <graphics/ImageBuffer.h>
#include <SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
namespace lime {
value lime_vorbis_file_read(value file,value buffer,int position,int length,bool bigEndian,int word,bool signedData);
void lime_gl_clear_depthf(float depth);
value lime_audio_load_bytes(value data, value buffer);
value lime_audio_load_file(value data, value buffer);
value lime_file_dialog_open_directory(HxString title, HxString filter, HxString defaultPath);
value lime_file_dialog_open_file(HxString title, HxString filter, HxString defaultPath);
value lime_file_dialog_open_files(HxString title, HxString filter, HxString defaultPath);
value lime_file_dialog_save_file(HxString title, HxString filter, HxString defaultPath);
int lime_application_alert(value application, int type, HxString message, HxString title, value buttons);

static value compatInt64(int64_t number) {
    value result=alloc_empty_object();
    alloc_field(result,val_id("low"),alloc_int((int)(uint32_t)number));
    alloc_field(result,val_id("high"),alloc_int((int)(number>>32)));
    return result;
}
static value compatOpenAudio(value data, bool file, int codec) {
    // Use NF's existing OGG/WAV decoder. Other codecs are explicitly unsupported.
    if (codec != 0 && codec != 4) return alloc_null();
    value buffer=alloc_empty_object();
    value pcm=alloc_empty_object();
    alloc_field(pcm,val_id("buffer"),alloc_empty_object());
    alloc_field(buffer,val_id("data"),pcm);
    value decoded=file ? lime_audio_load_file(data,buffer) : lime_audio_load_bytes(data,buffer);
    if (val_is_null(decoded)) return alloc_null();
    value handle=alloc_empty_object();
    alloc_field(handle,val_id("buffer"),decoded);
    alloc_field(handle,val_id("position"),alloc_float(0));
    return handle;
}
static int64_t compatAudioTotal(value handle) {
    if (val_is_null(handle)) return 0;
    AudioBuffer buffer(val_field(handle,val_id("buffer")));
    int size=buffer.channels*(buffer.bitsPerSample/8);
    return buffer.data && size>0 ? buffer.data->byteLength/size : 0;
}
static int compatDecodeAudio(value handle, unsigned char* dest, int capacity, int frames, bool floating) {
    if (val_is_null(handle) || !dest || capacity<=0 || frames<=0) return 0;
    AudioBuffer buffer(val_field(handle,val_id("buffer")));
    int inputSize=buffer.bitsPerSample/8;
    if (!buffer.data || !buffer.data->buffer || buffer.channels<=0 || (inputSize!=1 && inputSize!=2)) return 0;
    int outputSize=floating ? 4 : 2;
    int64_t position=(int64_t)val_number(val_field(handle,val_id("position")));
    int count=(int)std::min<int64_t>(frames,std::max<int64_t>(0,compatAudioTotal(handle)-position));
    count=std::min(count,capacity/(outputSize*buffer.channels));
    const unsigned char* src=buffer.data->buffer->b+position*inputSize*buffer.channels;
    for (int i=0;i<count*buffer.channels;i++) {
        int16_t sample=inputSize==1 ? (int16_t)((int(src[i])-128)*256) : (int16_t)(src[i*2]|(src[i*2+1]<<8));
        if (floating) { float number=sample/32768.0f; std::memcpy(dest+i*4,&number,4); }
        else { dest[i*2]=(unsigned char)sample;dest[i*2+1]=(unsigned char)(sample>>8); }
    }
    alloc_field(handle,val_id("position"),alloc_float((double)(position+count)));
    return count*buffer.channels*outputSize;
}

value lime_animation_decoder_get_frame(value a0, value a1) {
    return alloc_null();
}
DEFINE_PRIME2(lime_animation_decoder_get_frame);

int lime_animation_decoder_get_status(value a0) {
    return -1;
}
DEFINE_PRIME1(lime_animation_decoder_get_status);

value lime_animation_decoder_open_bytes(value a0, value a1) {
    return alloc_null();
}
DEFINE_PRIME2(lime_animation_decoder_open_bytes);

value lime_animation_decoder_open_file(value a0, value a1) {
    return alloc_null();
}
DEFINE_PRIME2(lime_animation_decoder_open_file);

bool lime_animation_decoder_reset(value a0) {
    return false;
}
DEFINE_PRIME1(lime_animation_decoder_reset);

bool lime_audio_decoder_can_seek(value a0) {
    return !val_is_null(a0);
}
DEFINE_PRIME1(lime_audio_decoder_can_seek);

value lime_audio_decoder_decode(value a0, value a1, int a2, int a3) {
    Bytes data(a1); int size=compatDecodeAudio(a0,data.b,data.length,a2,a3==1);
    if(size==0) {alloc_field(a1,val_id("length"),alloc_int(0));alloc_field(a1,val_id("b"),buffer_val(alloc_buffer_len(0)));return a1;}
    data.Resize(size);return data.Value(a1);
}
DEFINE_PRIME4(lime_audio_decoder_decode);

value lime_audio_decoder_info(value a0) {
    if (val_is_null(a0)) return alloc_null(); AudioBuffer buffer(val_field(a0,val_id("buffer"))); value info=alloc_empty_object(); alloc_field(info,val_id("channels"),alloc_int(buffer.channels));alloc_field(info,val_id("sampleRate"),alloc_int(buffer.sampleRate));alloc_field(info,val_id("bitsPerSample"),alloc_int(buffer.bitsPerSample));return info;
}
DEFINE_PRIME1(lime_audio_decoder_info);

value lime_audio_decoder_open_bytes(value a0, int a1) {
    return compatOpenAudio(a0,false,a1);
}
DEFINE_PRIME2(lime_audio_decoder_open_bytes);

value lime_audio_decoder_open_file(value a0, int a1) {
    return compatOpenAudio(a0,true,a1);
}
DEFINE_PRIME2(lime_audio_decoder_open_file);

bool lime_audio_decoder_rewind(value a0) {
    if (val_is_null(a0)) return false;alloc_field(a0,val_id("position"),alloc_float(0));return true;
}
DEFINE_PRIME1(lime_audio_decoder_rewind);

bool lime_audio_decoder_seek(value a0, int a1, int a2) {
    int64_t frame=(int64_t)(((uint64_t)(uint32_t)a2<<32)|(uint32_t)a1);if(val_is_null(a0)||frame<0||frame>compatAudioTotal(a0))return false;alloc_field(a0,val_id("position"),alloc_float((double)frame));return true;
}
DEFINE_PRIME3(lime_audio_decoder_seek);

value lime_audio_decoder_tell(value a0) {
    return compatInt64(val_is_null(a0)?0:(int64_t)val_number(val_field(a0,val_id("position"))));
}
DEFINE_PRIME1(lime_audio_decoder_tell);

value lime_audio_decoder_total(value a0) {
    return compatInt64(compatAudioTotal(a0));
}
DEFINE_PRIME1(lime_audio_decoder_total);

value lime_bmp_decode_bytes(value a0, value a1) {
    Bytes bytes(a0);if(!bytes.b || bytes.length<=0)return alloc_null();
    SDL_Surface* input=SDL_LoadBMP_RW(SDL_RWFromConstMem(bytes.b,bytes.length),1);
    if(!input)return alloc_null();
    SDL_Surface* rgba=SDL_ConvertSurfaceFormat(input,SDL_PIXELFORMAT_ABGR8888,0);SDL_FreeSurface(input);
    if(!rgba)return alloc_null();
    ImageBuffer output(a1);output.format=RGBA32;output.premultiplied=false;output.transparent=true;
    output.Resize(rgba->w,rgba->h,32);
    for(int y=0;y<rgba->h;y++)std::memcpy(output.data->buffer->b+y*rgba->w*4,(unsigned char*)rgba->pixels+y*rgba->pitch,rgba->w*4);
    SDL_FreeSurface(rgba);return output.Value(a1);
}
DEFINE_PRIME2(lime_bmp_decode_bytes);

value lime_bmp_decode_file(HxString a0, value a1) {
    const char* path=hxs_utf8(a0,nullptr);if(!path)return alloc_null();Bytes bytes;bytes.ReadFile(path);
    return lime_bmp_decode_bytes(bytes.Value(alloc_empty_object()),a1);
}
DEFINE_PRIME2(lime_bmp_decode_file);

void lime_drlibs_flac_close(value a0) {
    
}
DEFINE_PRIME1v(lime_drlibs_flac_close);

int lime_drlibs_flac_decode(value a0, value a1, int a2, int a3, int a4) {
    Bytes data(a1);if(a2<0||a3<0||a2>data.length)return 0;int size=std::min(a3,data.length-a2);int channels=val_is_null(a0)?0:val_int(val_field(val_field(a0,val_id("buffer")),val_id("channels")));return channels>0?compatDecodeAudio(a0,data.b+a2,size,size/(channels*(a4==4?4:2)),a4==4):0;
}
DEFINE_PRIME5(lime_drlibs_flac_decode);

value lime_drlibs_flac_from_bytes(value a0) {
    return compatOpenAudio(a0,false,2);
}
DEFINE_PRIME1(lime_drlibs_flac_from_bytes);

value lime_drlibs_flac_from_file(HxString a0) {
    return compatOpenAudio(alloc_string(hxs_utf8(a0,nullptr)),true,2);
}
DEFINE_PRIME1(lime_drlibs_flac_from_file);

value lime_drlibs_flac_info(value a0) {
    if (val_is_null(a0)) return alloc_null(); AudioBuffer buffer(val_field(a0,val_id("buffer"))); value info=alloc_empty_object(); alloc_field(info,val_id("channels"),alloc_int(buffer.channels));alloc_field(info,val_id("sampleRate"),alloc_int(buffer.sampleRate));alloc_field(info,val_id("bitsPerSample"),alloc_int(buffer.bitsPerSample));return info;
}
DEFINE_PRIME1(lime_drlibs_flac_info);

int lime_drlibs_flac_seek(value a0, value a1, value a2) {
    int64_t frame=(int64_t)(((uint64_t)(uint32_t)val_int(a2)<<32)|(uint32_t)val_int(a1));if(val_is_null(a0)||frame<0||frame>compatAudioTotal(a0))return 0;alloc_field(a0,val_id("position"),alloc_float((double)frame));return 1;
}
DEFINE_PRIME3(lime_drlibs_flac_seek);

value lime_drlibs_flac_tell(value a0) {
    return compatInt64(val_is_null(a0)?0:(int64_t)val_number(val_field(a0,val_id("position"))));
}
DEFINE_PRIME1(lime_drlibs_flac_tell);

value lime_drlibs_flac_total(value a0) {
    return compatInt64(compatAudioTotal(a0));
}
DEFINE_PRIME1(lime_drlibs_flac_total);

int lime_drlibs_mp3_decode(value a0, value a1, int a2, int a3) {
    Bytes data(a1);if(a2<0||a3<0||a2>data.length)return 0;int size=std::min(a3,data.length-a2);int channels=val_is_null(a0)?0:val_int(val_field(val_field(a0,val_id("buffer")),val_id("channels")));return channels>0?compatDecodeAudio(a0,data.b+a2,size,size/(channels*4),true):0;
}
DEFINE_PRIME4(lime_drlibs_mp3_decode);

value lime_drlibs_mp3_from_bytes(value a0) {
    return compatOpenAudio(a0,false,3);
}
DEFINE_PRIME1(lime_drlibs_mp3_from_bytes);

value lime_drlibs_mp3_from_file(HxString a0) {
    return compatOpenAudio(alloc_string(hxs_utf8(a0,nullptr)),true,3);
}
DEFINE_PRIME1(lime_drlibs_mp3_from_file);

value lime_drlibs_mp3_info(value a0) {
    if (val_is_null(a0)) return alloc_null(); AudioBuffer buffer(val_field(a0,val_id("buffer"))); value info=alloc_empty_object(); alloc_field(info,val_id("channels"),alloc_int(buffer.channels));alloc_field(info,val_id("sampleRate"),alloc_int(buffer.sampleRate));alloc_field(info,val_id("bitsPerSample"),alloc_int(buffer.bitsPerSample));return info;
}
DEFINE_PRIME1(lime_drlibs_mp3_info);

int lime_drlibs_mp3_seek(value a0, value a1, value a2) {
    int64_t frame=(int64_t)(((uint64_t)(uint32_t)val_int(a2)<<32)|(uint32_t)val_int(a1));if(val_is_null(a0)||frame<0||frame>compatAudioTotal(a0))return 0;alloc_field(a0,val_id("position"),alloc_float((double)frame));return 1;
}
DEFINE_PRIME3(lime_drlibs_mp3_seek);

value lime_drlibs_mp3_tell(value a0) {
    return compatInt64(val_is_null(a0)?0:(int64_t)val_number(val_field(a0,val_id("position"))));
}
DEFINE_PRIME1(lime_drlibs_mp3_tell);

value lime_drlibs_mp3_total(value a0) {
    return compatInt64(compatAudioTotal(a0));
}
DEFINE_PRIME1(lime_drlibs_mp3_total);

void lime_drlibs_mp3_uninit(value a0) {
    
}
DEFINE_PRIME1v(lime_drlibs_mp3_uninit);

int lime_drlibs_wav_decode(value a0, value a1, int a2, int a3, int a4) {
    Bytes data(a1);if(a2<0||a3<0||a2>data.length)return 0;int size=std::min(a3,data.length-a2);int channels=val_is_null(a0)?0:val_int(val_field(val_field(a0,val_id("buffer")),val_id("channels")));return channels>0?compatDecodeAudio(a0,data.b+a2,size,size/(channels*(a4==4?4:2)),a4==4):0;
}
DEFINE_PRIME5(lime_drlibs_wav_decode);

value lime_drlibs_wav_from_bytes(value a0) {
    return compatOpenAudio(a0,false,4);
}
DEFINE_PRIME1(lime_drlibs_wav_from_bytes);

value lime_drlibs_wav_from_file(HxString a0) {
    return compatOpenAudio(alloc_string(hxs_utf8(a0,nullptr)),true,4);
}
DEFINE_PRIME1(lime_drlibs_wav_from_file);

value lime_drlibs_wav_info(value a0) {
    if (val_is_null(a0)) return alloc_null(); AudioBuffer buffer(val_field(a0,val_id("buffer"))); value info=alloc_empty_object(); alloc_field(info,val_id("channels"),alloc_int(buffer.channels));alloc_field(info,val_id("sampleRate"),alloc_int(buffer.sampleRate));alloc_field(info,val_id("bitsPerSample"),alloc_int(buffer.bitsPerSample));return info;
}
DEFINE_PRIME1(lime_drlibs_wav_info);

int lime_drlibs_wav_seek(value a0, value a1, value a2) {
    int64_t frame=(int64_t)(((uint64_t)(uint32_t)val_int(a2)<<32)|(uint32_t)val_int(a1));if(val_is_null(a0)||frame<0||frame>compatAudioTotal(a0))return 0;alloc_field(a0,val_id("position"),alloc_float((double)frame));return 1;
}
DEFINE_PRIME3(lime_drlibs_wav_seek);

value lime_drlibs_wav_tell(value a0) {
    return compatInt64(val_is_null(a0)?0:(int64_t)val_number(val_field(a0,val_id("position"))));
}
DEFINE_PRIME1(lime_drlibs_wav_tell);

value lime_drlibs_wav_total(value a0) {
    return compatInt64(compatAudioTotal(a0));
}
DEFINE_PRIME1(lime_drlibs_wav_total);

void lime_drlibs_wav_uninit(value a0) {
    
}
DEFINE_PRIME1v(lime_drlibs_wav_uninit);

void lime_file_dialog_open_directory_compat(value a0, HxString a1, value a2, HxString a3, bool a4) {
    value path=lime_file_dialog_open_directory(a1,HxString(""),a3);if (!val_is_null(a2)) {value list=alloc_array(val_is_null(path)?0:1);if(!val_is_null(path))val_array_set_i(list,0,path);val_call1(a2,list);}
}
DEFINE_PRIME5v(lime_file_dialog_open_directory_compat);

void lime_file_dialog_open_file_compat(value a0, HxString a1, value a2, value a3, value a4, int a5, HxString a6, bool a7) {
    HxString filter(""); if(a5>0&&!val_is_null(a4)) filter=HxString(val_string(val_array_i(a4,0)));value list;
        if(a7) list=lime_file_dialog_open_files(a1,filter,a6);
        else {value path=lime_file_dialog_open_file(a1,filter,a6);list=alloc_array(val_is_null(path)?0:1);if(!val_is_null(path))val_array_set_i(list,0,path);}
        if(val_is_null(list))list=alloc_array(0);if(!val_is_null(a2))val_call2(a2,list,alloc_int(a5>0?0:-1));
}
DEFINE_PRIME8v(lime_file_dialog_open_file_compat);

void lime_file_dialog_save_file_compat(value a0, HxString a1, value a2, value a3, value a4, int a5, HxString a6) {
    HxString filter("");if(a5>0&&!val_is_null(a4))filter=HxString(val_string(val_array_i(a4,0)));value path=lime_file_dialog_save_file(a1,filter,a6);if(!val_is_null(a2))val_call2(a2,path,alloc_int(a5>0?0:-1));
}
DEFINE_PRIME7v(lime_file_dialog_save_file_compat);

value lime_gif_decode_bytes(value a0, value a1) {
    return alloc_null();
}
DEFINE_PRIME2(lime_gif_decode_bytes);

value lime_gif_decode_file(HxString a0, value a1) {
    return alloc_null();
}
DEFINE_PRIME2(lime_gif_decode_file);

void lime_gl_blend_barrier() {
    #if defined(LIME_OPENGL) || defined(LIME_OPENGLES)
    if(SDL_GL_GetCurrentContext() && SDL_GL_ExtensionSupported("GL_KHR_blend_equation_advanced")) {
        #ifdef _WIN32
        typedef void (__stdcall *Barrier)();
        #else
        typedef void (*Barrier)();
        #endif
        Barrier barrier=(Barrier)SDL_GL_GetProcAddress("glBlendBarrierKHR");
        if(barrier)barrier();
    }
    #endif
}
DEFINE_PRIME0v(lime_gl_blend_barrier);

void lime_gl_clear_depth(float a0) {
    #if defined(LIME_OPENGL) || defined(LIME_OPENGLES)
        lime_gl_clear_depthf(a0);
        #endif
}
DEFINE_PRIME1v(lime_gl_clear_depth);

int lime_opus_file_channel_count(value a0) {
    return 0;
}
DEFINE_PRIME1(lime_opus_file_channel_count);

int lime_opus_file_decode(value a0, value a1, int a2, int a3) {
    return 0;
}
DEFINE_PRIME4(lime_opus_file_decode);

void lime_opus_file_free(value a0) {
    
}
DEFINE_PRIME1v(lime_opus_file_free);

value lime_opus_file_from_bytes(value a0) {
    return alloc_null();
}
DEFINE_PRIME1(lime_opus_file_from_bytes);

value lime_opus_file_from_file(HxString a0) {
    return alloc_null();
}
DEFINE_PRIME1(lime_opus_file_from_file);

int lime_opus_file_seek(value a0, value a1, value a2) {
    return 0;
}
DEFINE_PRIME3(lime_opus_file_seek);

bool lime_opus_file_seekable(value a0) {
    return false;
}
DEFINE_PRIME1(lime_opus_file_seekable);

value lime_opus_file_tell(value a0) {
    return alloc_null();
}
DEFINE_PRIME1(lime_opus_file_tell);

value lime_opus_file_total(value a0) {
    return alloc_null();
}
DEFINE_PRIME1(lime_opus_file_total);

value lime_svg_decode_bytes(value a0, value a1) {
    return alloc_null();
}
DEFINE_PRIME2(lime_svg_decode_bytes);

value lime_svg_decode_file(HxString a0, value a1) {
    return alloc_null();
}
DEFINE_PRIME2(lime_svg_decode_file);

value lime_svg_decode_sized_bytes(value a0, int a1, int a2, value a3) {
    return alloc_null();
}
DEFINE_PRIME4(lime_svg_decode_sized_bytes);

value lime_svg_decode_sized_file(HxString a0, int a1, int a2, value a3) {
    return alloc_null();
}
DEFINE_PRIME4(lime_svg_decode_sized_file);

int lime_vorbis_file_decode(value a0, value a1, int a2, int a3, int a4) {
    if(val_is_null(a0)||val_is_null(a1)||a2<0||a3<=0||a4!=2)return 0;
    Bytes bytes(a1);if(a2>bytes.length)return 0;int limit=std::min(a3,bytes.length-a2),count=0;
    #ifdef LIME_VORBIS
    while(count<limit) {
        value result=lime_vorbis_file_read(a0,a1,a2+count,limit-count,false,2,true);
        if(val_is_null(result))break;
        int read=val_int(val_field(result,val_id("returnValue")));if(read<=0)break;count+=read;
    }
    #endif
    return count;
}
DEFINE_PRIME5(lime_vorbis_file_decode);

value lime_webp_decode_bytes(value a0, value a1) {
    return alloc_null();
}
DEFINE_PRIME2(lime_webp_decode_bytes);

value lime_webp_decode_file(HxString a0, value a1) {
    return alloc_null();
}
DEFINE_PRIME2(lime_webp_decode_file);

int lime_window_alert_compat(value a0, int a1, HxString a2, HxString a3, value a4) {
    return lime_application_alert(alloc_null(),a1,a2,a3,a4);
}
DEFINE_PRIME5(lime_window_alert_compat);

}
