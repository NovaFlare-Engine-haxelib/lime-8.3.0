package lime._internal.compat;
#if !macro
import lime.app.Event;
import lime.utils.DroppedFile;
#end
#if macro
import haxe.macro.Context;
import haxe.macro.Expr;
import haxe.macro.Type;
#end

/** One event supports NF's String callback and CNE/Origin's DroppedFile callback.
    All listeners share priorities, cancellation and once-only semantics. */
@:forward()
abstract DropFileEvent(DropFileEventData) from DropFileEventData to DropFileEventData {
    #if !macro
    public function new(?event:Event<String->Void>) {this=new DropFileEventData(event);}
    @:from public static function fromLegacy(event:Event<String->Void>):DropFileEvent return new DropFileEvent(event);
    @:to public function toLegacy():Event<String->Void> return @:privateAccess this.event;
    #end
    public macro function add(self:Expr,listener:Expr,?once:Expr,?priority:Expr):Expr {
        var detailed=false;
        switch(Context.follow(Context.typeof(listener))) {
            case TFun(args,_):
                if(args.length!=1 && args.length!=4)Context.error("A dropped-file callback takes 1 or 4 arguments",listener.pos);
                detailed=args.length==4;
            default:Context.error("Expected a dropped-file callback",listener.pos);
        }
        switch(once) {case null | {expr:EConst(CIdent("null"))}:once=macro false;default:}
        switch(priority) {case null | {expr:EConst(CIdent("null"))}:priority=macro 0;default:}
        return macro $self.__add($listener,$once,$priority,$v{detailed});
    }
}
#if !macro
private class DropFileEventData {
    private var event:Event<String->Void>;
    @:noCompletion public var __listeners(get,set):Array<String->Void>;
    @:noCompletion public var __repeat(get,set):Array<Bool>;
    private function get___listeners():Array<String->Void> return event.__listeners;
    private function set___listeners(value:Array<String->Void>):Array<String->Void> return event.__listeners=value;
    private function get___repeat():Array<Bool> return event.__repeat;
    private function set___repeat(value:Array<Bool>):Array<Bool> return event.__repeat=value;
    private var adapters:Array<{listener:Dynamic,wrapper:String->Void}> = [];
    private var source:String;
    private var x:Float=0;
    private var y:Float=0;
    public var canceled(get,never):Bool;
    private function get_canceled():Bool return event.canceled;
    public function new(?event:Event<String->Void>) {this.event=event==null?new Event<String->Void>():event;}

    // Keep event.add available to runtime scripts as well as Haxe's typed macro.
    @:keep public function add(listener:Dynamic,once:Bool=false,priority:Int=0):Void {
        var detailed=false;
        #if cpp
        if(listener!=null)detailed=untyped __cpp__("{0}->__ArgCount() == 4",listener);
        #elseif js
        detailed=Reflect.field(listener,"length")==4;
        #end
        __add(listener,once,priority,detailed);
    }

    @:noCompletion public function __add(listener:Dynamic,once:Bool,priority:Int,detailed:Bool):Void {
        var wrapper:String->Void;
        if(detailed) wrapper=path->Reflect.callMethod(null,listener,[@:privateAccess new DroppedFile(path #if (js && html5) ,null #end),source,x,y]);
        else wrapper=cast listener;
        adapters.push({listener:listener,wrapper:wrapper});event.add(wrapper,once,priority);
    }
    public function dispatch(file:Dynamic,source:String=null,x:Float=0,y:Float=0):Void {
        this.source=source;this.x=x;this.y=y;
        var path:String=Std.isOfType(file,DroppedFile)?(cast file:DroppedFile).path:cast file;
        event.dispatch(path);prune();
    }
    public function cancel():Void event.cancel();
    public function has(listener:Dynamic):Bool {
        prune();if(event.has(cast listener))return true;for(adapter in adapters)if(Reflect.compareMethods(adapter.listener,listener))return true;return false;
    }
    public function remove(listener:Dynamic):Void {
        event.remove(cast listener);
        for(adapter in adapters.copy())if(Reflect.compareMethods(adapter.listener,listener)) {event.remove(adapter.wrapper);adapters.remove(adapter);}
    }
    public function removeAll():Void {event.removeAll();adapters=[];}
    private function prune():Void {for(adapter in adapters.copy())if(!event.has(adapter.wrapper))adapters.remove(adapter);}
}

#else
private typedef DropFileEventData = Dynamic;
#end
