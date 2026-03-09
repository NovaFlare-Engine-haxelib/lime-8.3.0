package lime.system;

import lime.system.System;

#if !lime_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
class BackendThread
{
    public static function start():Void
    {
        #if (lime_cffi && !macro)
        try {
            lime_backend_thread_start();
        } catch (e:Dynamic) {
            trace("[BackendThread] Error: Native bindings missing. Please run 'lime rebuild windows'. Details: " + e);
        }
        #end
    }

    public static function stop():Void
    {
        #if (lime_cffi && !macro)
        try {
            lime_backend_thread_stop();
        } catch (e:Dynamic) {
            // Ignore if already stopped or missing
        }
        #end
    }

    public static function run(callback:Void->Void):Void
    {
        #if (lime_cffi && !macro)
        try {
            lime_backend_thread_run(callback);
        } catch (e:Dynamic) {
            trace("[BackendThread] Error: Cannot run task. Native library may be outdated.");
        }
        #end
    }

    #if (lime_cffi && !macro)
    private static var lime_backend_thread_start = System.load("lime", "lime_backend_thread_start", 0, true);
    private static var lime_backend_thread_stop = System.load("lime", "lime_backend_thread_stop", 0, true);
    private static var lime_backend_thread_run = System.load("lime", "lime_backend_thread_run", 1, true);
    #end
}
