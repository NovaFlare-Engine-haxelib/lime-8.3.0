package lime.media.decoders;
import haxe.io.Bytes;
import lime.media.AudioCodec;
import lime.media.AudioDecoder;
@:access(lime.media.AudioDecoder)
class FLACDecoder extends AudioDecoder {
    private function new(handle:Dynamic) {super(handle,AudioCodec.FLAC);}
    public static function fromBytes(bytes:Bytes):FLACDecoder {
        var result=AudioDecoder.fromBytes(bytes,AudioCodec.FLAC);if(result==null)return null;
        var decoder=new FLACDecoder(result.handle);decoder.bytes=bytes;return decoder;
    }
    public static function fromFile(path:String):FLACDecoder {
        var result=AudioDecoder.fromFile(path,AudioCodec.FLAC);if(result==null)return null;
        var decoder=new FLACDecoder(result.handle);decoder.path=path;return decoder;
    }
    override public function clone():FLACDecoder return path!=null?fromFile(path):fromBytes(bytes);
}
