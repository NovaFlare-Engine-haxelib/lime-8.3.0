package lime.media;

import lime.app.Event;
import lime.media.openal.AL;
import lime.media.openal.ALSource;
import lime.math.Vector4;

#if !lime_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
/**
	The `AudioSource` class provides a way to control audio playback in a Lime application. 
	It allows for playing, pausing, and stopping audio, as well as controlling various 
	audio properties such as gain, pitch, and looping.

	Depending on the platform, the audio backend may vary, but the API remains consistent.

	@see lime.media.AudioBuffer
**/
class AudioSource
{
	/**
		An event that is dispatched when the audio playback is complete.
	**/
	public var onComplete = new Event<Void->Void>();
	
	public static var activeSources(default, null):Array<AudioSource> = [];

	/**
		The `AudioBuffer` associated with this `AudioSource`.
	**/
	public var buffer:AudioBuffer;

	/**
		The current playback position of the audio, in milliseconds.
	**/
	public var currentTime(get, set):Float;

	/**
		The gain (volume) of the audio. A value of `1.0` represents the default volume.
	**/
	public var gain(get, set):Float;

	/**
		The length of the audio, in milliseconds.
	**/
	public var length(get, set):Int;

	/**
		The number of times the audio will loop. A value of `0` means the audio will not loop.
	**/
	public var loops(get, set):Int;

	/**
		The pitch of the audio. A value of `1.0` represents the default pitch.
	**/
	public var pitch(get, set):Float;

	/**
		The offset within the audio buffer to start playback, in samples.
	**/
	public var offset:Int;

	/**
		The 3D position of the audio source, represented as a `Vector4`.
	**/
	public var position(get, set):Vector4;

	/**
		Whether the audio source is currently playing.
	**/
	public var playing(get, never):Bool;

	@:noCompletion private var __backend:AudioSourceBackend;

	/**
		Creates a new `AudioSource` instance.
		@param buffer The `AudioBuffer` to associate with this `AudioSource`.
		@param offset The starting offset within the audio buffer, in samples.
		@param length The length of the audio to play, in milliseconds. If `null`, the full buffer is used.
		@param loops The number of times to loop the audio. `0` means no looping.
	**/
	public function new(buffer:AudioBuffer = null, offset:Int = 0, length:Null<Int> = null, loops:Int = 0)
	{
		this.buffer = buffer;
		this.offset = offset;

		__backend = new AudioSourceBackend(this);

		if (length != null && length != 0)
		{
			this.length = length;
		}

		this.loops = loops;

		activeSources.push(this);

		if (buffer != null)
		{
			init();
		}
	}

	/**
		Releases any resources used by this `AudioSource`.
	**/
	public function dispose():Void
	{
		activeSources.remove(this);
		__backend.dispose();
	}

	@:noCompletion private function init():Void
	{
		__backend.init();
	}

	/**
		Starts or resumes audio playback.
	**/
	public function play():Void
	{
		__backend.play();
	}

	/**
		Pauses audio playback.
	**/
	public function pause():Void
	{
		__backend.pause();
	}

	/**
		Stops audio playback and resets the playback position to the beginning.
	**/
	public function stop():Void
	{
		__backend.stop();
	}

	// Get & Set Methods
	@:noCompletion private function get_currentTime():Float
	{
		return __backend.getCurrentTime();
	}

	@:noCompletion private function set_currentTime(value:Float):Float
	{
		return __backend.setCurrentTime(Std.int(value));
	}

	@:noCompletion private function get_gain():Float
	{
		return __backend.getGain();
	}

	@:noCompletion private function set_gain(value:Float):Float
	{
		return __backend.setGain(value);
	}

	@:noCompletion private function get_length():Int
	{
		return __backend.getLength();
	}

	@:noCompletion private function set_length(value:Int):Int
	{
		return __backend.setLength(value);
	}

	@:noCompletion private function get_loops():Int
	{
		return __backend.getLoops();
	}

	@:noCompletion private function set_loops(value:Int):Int
	{
		return __backend.setLoops(value);
	}

	@:noCompletion private function get_pitch():Float
	{
		return __backend.getPitch();
	}

	@:noCompletion private function set_pitch(value:Float):Float
	{
		return __backend.setPitch(value);
	}

	@:noCompletion private function get_position():Vector4
	{
		return __backend.getPosition();
	}

	@:noCompletion private function set_position(value:Vector4):Vector4
	{
		return __backend.setPosition(value);
	}

	@:noCompletion private function get_playing():Bool
	{
		return __backend.getPlaying();
	}


    private function __addCompatEffect(index:Int):Void {
        #if !(flash || (js && html5))
        __backend.addEffect(index);
        #end
    }
    private function __updateCompatEffect(index:Int):Void {
        #if !(flash || (js && html5))
        __backend.updateEffect(index);
        #end
    }
    private function __removeCompatEffect(index:Int):Void {
        #if !(flash || (js && html5))
        __backend.removeEffect(index);
        #end
    }
    private var __effects:Array<AudioEffect> = [];
    public var latency(get,never):Float;
    public var loopTime:Float = 0;
    public var pan(get,set):Float;
    public var peaks(get,never):Array<Float>;
    private function get_latency():Float {
        #if (flash || (js && html5))
        return 0;
        #else
        return __backend.getLatency();
        #end
    }
    private function get_pan():Float return position.x;
    private function set_pan(value:Float):Float {position=new Vector4(value,0,0);return value;}
    private function get_peaks():Array<Float> return [for(_ in 0...(buffer==null?0:buffer.channels)) 0.0];
    public function load():Void {if(buffer!=null) {buffer.load();init();}}
    public function unload():Void {stop();__backend.dispose();}
    public function prepare(minTime:Float=0):Void {if(buffer!=null)buffer.load();}
    public static function pauseSources(sources:Array<AudioSource>):Void {for(source in sources)if(source!=null)source.pause();}
    public static function playSources(sources:Array<AudioSource>):Void {for(source in sources)if(source!=null)source.play();}
    public static function stopSources(sources:Array<AudioSource>):Void {for(source in sources)if(source!=null)source.stop();}

    public function getFloatTimeDomainData(array:lime.utils.Float32Array, size:Int, channel:Int=-1, offset:Int=0):Int {
        if(array==null||buffer==null||buffer.data==null||size<=0)return 0;
        var bytesPerSample=Std.int(buffer.bitsPerSample/8);
        if(bytesPerSample!=1&&bytesPerSample!=2)return 0;
        var step=channel<0?1:buffer.channels;
        var start=(Std.int(currentTime*buffer.sampleRate/1000)+offset)*buffer.channels+(channel<0?0:channel);
        var count=0;
        while(count<size && count<array.length && start>=0 && (start+count*step+1)*bytesPerSample<=buffer.data.length) {
            var index=(start+count*step)*bytesPerSample;
            var sample=bytesPerSample==1?(buffer.data[index]-128)*256:(buffer.data[index]|(buffer.data[index+1]<<8));
            if(sample>=32768)sample-=65536;
            array[count++]=sample/32768.0;
        }
        return count;
    }

    public function getByteTimeDomainData(array:lime.utils.UInt8Array, size:Int, channel:Int=-1, offset:Int=0):Int {
        if(array==null||size<=0)return 0;
        var samples=new lime.utils.Float32Array(Std.int(Math.min(size,array.length)));
        var count=getFloatTimeDomainData(samples,size,channel,offset);
        for(i in 0...count)array[i]=Std.int((samples[i]+1)*127.5);
        return count;
    }



	public function addEffect(effect:AudioEffect):Bool
	{
		if (__effects == null) __effects = [];

		var index = __effects.indexOf(effect);
		if (index == -1)
		{
			if (__effects.length > 6) return false;

			index = __effects.indexOf(null);
			if (index == -1)
			{
				index = __effects.length;
				__effects.push(effect);
			}
			else
			{
				__effects[index] = effect;
			}

			effect.__appliedSources.push(this);
			if (!effect.bypass) __addCompatEffect(index);
		}
		else if (!effect.bypass)
		{
			__updateCompatEffect(index);
		}

		return true;
	}

	public function removeEffect(effect:AudioEffect):Void
	{
		if (__effects != null)
		{
			var index = __effects.indexOf(effect);
			if (index != -1)
			{
				if (!effect.bypass) __removeCompatEffect(index);
				__effects[index] = null;
				//while (__effects[__effects.length - 1] == null) __effects.pop();

				effect.__appliedSources.remove(this);
				if (effect.autoDispose) effect.dispose();
			}
		}
	}

	public function clearEffects():Void
	{
		if (__effects != null)
		{
			var index = __effects.length, effect:AudioEffect;
			while (index-- > 0)
			{
				effect = __effects[index];
				if (effect == null) continue;

				if (!effect.bypass) __removeCompatEffect(index);
				if (effect.autoDispose) effect.dispose();
			}

			__effects = null;
		}
	}

	public function getEffectAt(index:Int):AudioEffect
	{
		return __effects[index];
	}

	public function getEffectIndex(effect:AudioEffect):Int
	{
		return __effects.indexOf(effect);
	}
}

#if flash
@:noCompletion private typedef AudioSourceBackend = lime._internal.backend.flash.FlashAudioSource;
#elseif (js && html5)
@:noCompletion private typedef AudioSourceBackend = lime._internal.backend.html5.HTML5AudioSource;
#else
@:noCompletion private typedef AudioSourceBackend = lime._internal.backend.native.NativeAudioSource;
#end
