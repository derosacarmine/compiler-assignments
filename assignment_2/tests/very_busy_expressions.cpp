#include <cstdio>

/**
 * Test per Very Busy Expressions (Backward Analysis)
 *
 * CFG atteso:
 *   entry -> if.then / if.else -> exit
 *
 * Espressioni binarie:
 *   - (a - b) calcolata in ENTRAMBI i rami -> very busy in entry
 *   - (b - a) calcolata in ENTRAMBI i rami -> very busy in entry
 *
 * Risultato atteso:
 *   IN[entry]   = { (a-b), (b-a) }
 *   IN[if.then] = { (a-b), (b-a) }
 *   IN[if.else] = { (a-b), (b-a) }
 *   OUT[if.then] = OUT[if.else] = {} (exit successors)
 */
void test_very_busy(int cond, int a, int b) {
    int x, y;

    if (cond > 0) {
        // Ramo then: calcola a-b e b-a
        x = a - b;
        y = b - a;
        std::printf("then: x=%d, y=%d\n", x, y);
    } else {
        // Ramo else: calcola le stesse espressioni
        x = a - b;
        y = b - a;
        std::printf("else: x=%d, y=%d\n", x, y);
    }
}

// int main(int argc, char** argv) {
//     test_very_busy(argc - 1, 10, 3);
//     return 0;
// }