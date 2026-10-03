package lime.media.decoders;
import haxe.io.Bytes;
import lime.media.AudioCodec;
import lime.media.AudioDecoder;
@:access(lime.media.AudioDecoder)
class VorbisDecoder extends AudioDecoder {
    private function new(handle:Dynamic) {super(handle,AudioCodec.VORBIS);}
    public static function fromBytes(bytes:Bytes):VorbisDecoder {
        var result=AudioDecoder.fromBytes(bytes,AudioCodec.VORBIS);if(result==null)return null;
        var decoder=new VorbisDecoder(result.handle);decoder.bytes=bytes;return decoder;
    }
    public static function fromFile(path:String):VorbisDecoder {
        var result=AudioDecoder.fromFile(path,AudioCodec.VORBIS);if(result==null)return null;
        var decoder=new VorbisDecoder(result.handle);decoder.path=path;return decoder;
    }
    override public function clone():VorbisDecoder return path!=null?fromFile(path):fromBytes(bytes);


    public var version:Int = 0;
    public static function fromVorbisFile(file:lime.media.vorbis.VorbisFile):VorbisDecoder {
        if(file==null)return null;
        var info=file.info();if(info==null)return null;
        var total=file.pcmTotal();if(total.high!=0||total.low<0)return null;
        var previous=file.pcmTell();file.pcmSeek(0);
        var pcm=haxe.io.Bytes.alloc(total.low*info.channels*2);var count=0;
        while(count<pcm.length) {var read=file.read(pcm,count,pcm.length-count);if(read<=0)break;count+=read;}
        file.pcmSeek(previous);
        var wave=haxe.io.Bytes.alloc(44+count);wave.blit(0,haxe.io.Bytes.ofString("RIFF"),0,4);wave.setInt32(4,36+count);
        wave.blit(8,haxe.io.Bytes.ofString("WAVEfmt "),0,8);wave.setInt32(16,16);wave.setUInt16(20,1);wave.setUInt16(22,info.channels);
        wave.setInt32(24,info.rate);wave.setInt32(28,info.rate*info.channels*2);wave.setUInt16(32,info.channels*2);wave.setUInt16(34,16);
        wave.blit(36,haxe.io.Bytes.ofString("data"),0,4);wave.setInt32(40,count);wave.blit(44,pcm,0,count);
        var result=AudioDecoder.fromBytes(wave,AudioCodec.WAVE);if(result==null)return null;
        var decoder=new VorbisDecoder(result.handle);decoder.bytes=wave;decoder.codec=AudioCodec.WAVE;decoder.version=info.version;return decoder;
    }

}
