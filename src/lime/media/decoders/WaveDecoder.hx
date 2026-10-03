package lime.media.decoders;
import haxe.io.Bytes;
import lime.media.AudioCodec;
import lime.media.AudioDecoder;
@:access(lime.media.AudioDecoder)
class WaveDecoder extends AudioDecoder {
    private function new(handle:Dynamic) {super(handle,AudioCodec.WAVE);}
    public static function fromBytes(bytes:Bytes):WaveDecoder {
        var result=AudioDecoder.fromBytes(bytes,AudioCodec.WAVE);if(result==null)return null;
        var decoder=new WaveDecoder(result.handle);decoder.bytes=bytes;return decoder;
    }
    public static function fromFile(path:String):WaveDecoder {
        var result=AudioDecoder.fromFile(path,AudioCodec.WAVE);if(result==null)return null;
        var decoder=new WaveDecoder(result.handle);decoder.path=path;return decoder;
    }
    override public function clone():WaveDecoder return path!=null?fromFile(path):fromBytes(bytes);
}
