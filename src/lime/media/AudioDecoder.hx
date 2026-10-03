package lime.media;
import haxe.Int64;
import haxe.io.Bytes;
import lime.app.Future;
import lime.utils.ArrayBuffer;
import lime._internal.backend.native.NativeCFFI;

/** OGG/WAV compatibility decoder backed by NF's existing native codecs.
    CNE's decode(buffer, offset, length, word) and Origin's decode(frames, format)
    are both accepted. Unsupported optional codecs return null from factories. */
@:access(lime._internal.backend.native.NativeCFFI)
class AudioDecoder {
    public var bitsPerSample:Int = 16;
    public var channels:Int = 0;
    public var sampleRate:Int = 0;
    public var eof:Bool = false;
    private var handle:Dynamic;
    private var path:String;
    private var bytes:Bytes;
    private var codec:AudioCodec;
    private function new(handle:Dynamic, codec:AudioCodec) {
        this.handle=handle; this.codec=codec;
        #if (lime_cffi && !macro)
        var info=NativeCFFI.lime_audio_decoder_info(handle);
        if(info!=null) {channels=info.channels;sampleRate=info.sampleRate;}
        #end
    }
    public static function fromFile(path:String, ?codec:AudioCodec):AudioDecoder {
        if(path==null)return null;
        #if (lime_cffi && !macro)
        for(kind in (codec==null ? [AudioCodec.VORBIS,AudioCodec.WAVE] : [codec])) {
            var handle=NativeCFFI.lime_audio_decoder_open_file(path,kind.toNative());
            if(handle!=null) {var result=new AudioDecoder(handle,kind);result.path=path;return result;}
        }
        #end
        return null;
    }
    public static function fromBytes(bytes:Bytes, ?codec:AudioCodec):AudioDecoder {
        if(bytes==null)return null;
        #if (lime_cffi && !macro)
        for(kind in (codec==null ? [AudioCodec.VORBIS,AudioCodec.WAVE] : [codec])) {
            var handle=NativeCFFI.lime_audio_decoder_open_bytes(bytes,kind.toNative());
            if(handle!=null) {var result=new AudioDecoder(handle,kind);result.bytes=bytes;return result;}
        }
        #end
        return null;
    }
    public static function fromFiles(paths:Array<String>):AudioDecoder {
        if(paths!=null)for(path in paths) {var result=fromFile(path);if(result!=null)return result;}
        return null;
    }
    public static function fromBase64(data:String):AudioDecoder {
        if(data==null)return null;
        var comma=data.indexOf(",");return fromBytes(haxe.crypto.Base64.decode(comma<0?data:data.substr(comma+1)));
    }
    public static function loadFromFile(path:String):Future<AudioDecoder> return new Future(function() {
        var result=fromFile(path);if(result==null)throw "Unsupported or invalid audio: "+path;return result;
    },true);
    public static function loadFromFiles(paths:Array<String>):Future<AudioDecoder> return new Future(function() {
        var result=fromFiles(paths);if(result==null)throw "Unsupported or invalid audio";return result;
    },true);
    public function clone():AudioDecoder return path!=null?fromFile(path,codec):fromBytes(bytes,codec);
    public function dispose():Void {handle=null;bytes=null;path=null;eof=true;}
    public function decode(bufferOrFrames:Dynamic, positionOrFormat:Dynamic=0, ?length:Int, word:Int=2):Dynamic {
        var frameMode=length==null;
        var format:Int=frameMode ? cast positionOrFormat : (word==4?1:0);
        var size=format==1?4:2;
        if(channels<=0 || handle==null)return frameMode?null:0;
        var frames:Int=frameMode ? cast bufferOrFrames : Std.int(length/(size*channels));
        if(frames<0)return frameMode?null:0;
        var output=Bytes.alloc(frames*channels*size);
        #if (lime_cffi && !macro)
        output=cast NativeCFFI.lime_audio_decoder_decode(handle,output,frames,format);
        #end
        if(output==null)return frameMode?null:0;
        eof=output.length<frames*channels*size;
        if(frameMode)return output;
        var destination:Bytes=cast bufferOrFrames;
        var position:Int=cast positionOrFormat;
        if(destination==null||position<0||position>destination.length)return 0;
        var count=Std.int(Math.min(output.length,destination.length-position));
        destination.blit(position,output,0,count);return count;
    }
    public function rewind():Bool return seek(Int64.make(0,0));
    public function seek(frame:Int64):Bool {
        #if (lime_cffi && !macro)
        if(handle!=null && NativeCFFI.lime_audio_decoder_seek(handle,frame.low,frame.high)) {eof=false;return true;}
        #end
        return false;
    }
    public function canSeek():Bool return handle!=null;
    public function seekable():Bool return canSeek();
    public function tell():Int64 {
        #if (lime_cffi && !macro)
        if(handle!=null) {var result=NativeCFFI.lime_audio_decoder_tell(handle);return Int64.make(result.high,result.low);}
        #end
        return 0;
    }
    public function total():Int64 {
        #if (lime_cffi && !macro)
        if(handle!=null) {var result=NativeCFFI.lime_audio_decoder_total(handle);return Int64.make(result.high,result.low);}
        #end
        return 0;
    }
}
