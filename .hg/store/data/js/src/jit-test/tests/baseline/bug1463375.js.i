         [   Z     j���������c@ެbw��e7 3�ޤM3            u// |jit-test| allow-overrecursed
function f() {
   f.apply(null, new Array(20000));
}
f()
