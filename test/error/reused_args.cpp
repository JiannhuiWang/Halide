#include <Halide.h>

using namespace Halide;

int main(int argc, char **argv) {
    Func f;
    Var x;
    f(x, x) = x;

    return 0;
}
