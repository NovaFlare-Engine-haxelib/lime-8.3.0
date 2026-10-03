package lime._internal.compat;
/** Accepts both NF's function syntax and CNE/Origin's .call syntax. */
@:callable abstract Callable<T>(T) from T {
    public var call(get,never):T;
    private inline function get_call():T return this;
}
