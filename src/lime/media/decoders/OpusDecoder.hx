package lime.media.decoders;
import haxe.io.Bytes;
import lime.media.AudioCodec;
import lime.media.AudioDecoder;
@:access(lime.media.AudioDecoder)
class OpusDecoder extends AudioDecoder {
    private function new(handle:Dynamic) {super(handle,AudioCodec.OPUS);}
    public static function fromBytes(bytes:Bytes):OpusDecoder {
        var result=AudioDecoder.fromBytes(bytes,AudioCodec.OPUS);if(result==null)return null;
        var decoder=new OpusDecoder(result.handle);decoder.bytes=bytes;return decoder;
    }
    public static function fromFile(path:String):OpusDecoder {
        var result=AudioDecoder.fromFile(path,AudioCodec.OPUS);if(result==null)return null;
        var decoder=new OpusDecoder(result.handle);decoder.path=path;return decoder;
    }
    override public function clone():OpusDecoder return path!=null?fromFile(path):fromBytes(bytes);
}
