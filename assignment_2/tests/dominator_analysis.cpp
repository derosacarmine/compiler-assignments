#include <cstdio>

/**
 * Test per Dominator Analysis
 *
 * CFG atteso (con -O0):
 *   entry -> if.then / if.else -> if.end -> loop.header -> loop.body / loop.exit
 *
 * Dominatori attesi:
 *   entry       dom: { entry }
 *   if.then     dom: { entry, if.then }
 *   if.else     dom: { entry, if.else }
 *   if.end      dom: { entry, if.end }
 *   loop.header dom: { entry, if.end, loop.header }
 *   loop.body   dom: { entry, if.end, loop.header, loop.body }
 *   loop.exit   dom: { entry, if.end, loop.header, loop.exit }
 */
void test_dominator(int cond, int n) {
    int x;

    // Primo branch: crea if.then e if.else
    if (cond > 0) {
        x = 1;
    } else {
        x = 2;
    }

    // if.end: punto di join, dominato solo da entry e se stesso
    int sum = 0;

    // Loop: loop.header domina loop.body e loop.exit
    for (int i = 0; i < n; i++) {
        sum += x;
    }

    std::printf("sum=%d\n", sum);
}

int main(int argc, char** argv) {
    test_dominator(argc - 1, 5);
    return 0;
}