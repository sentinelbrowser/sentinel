         c   b     ^���������R	m1�ɩ��&C$sKm�S�            u// |jit-test| error:TypeError
function f() {
    with(this) {};
}
(new new Proxy(f, {get: f}))();
