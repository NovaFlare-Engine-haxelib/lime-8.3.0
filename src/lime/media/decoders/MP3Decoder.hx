package lime.media.decoders;
import haxe.io.Bytes;
import lime.media.AudioCodec;
import lime.media.AudioDecoder;
@:access(lime.media.AudioDecoder)
class MP3Decoder extends AudioDecoder {
    private function new(handle:Dynamic) {super(handle,AudioCodec.MPEG);}
    public static function fromBytes(bytes:Bytes):MP3Decoder {
        var result=AudioDecoder.fromBytes(bytes,AudioCodec.MPEG);if(result==null)return null;
        var decoder=new MP3Decoder(result.handle);decoder.bytes=bytes;return decoder;
    }
    public static function fromFile(path:String):MP3Decoder {
        var result=AudioDecoder.fromFile(path,AudioCodec.MPEG);if(result==null)return null;
        var decoder=new MP3Decoder(result.handle);decoder.path=path;return decoder;
    }
    override public function clone():MP3Decoder return path!=null?fromFile(path):fromBytes(bytes);
}
