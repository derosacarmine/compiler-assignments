#include <iostream>
#include <cstdio>

/**
 * Questo programma riflette la struttura del CFG della Constant Propagation:
 * - k viene inizializzato a 2 (BB2)
 * - a viene inizializzato a 4 in entrambi i rami (BB4, BB6)
 * - x riceve valori diversi (5 o 8) a seconda del ramo (BB5, BB7)
 * - BB8 è il punto di "Meet" dove x perde la costanza, ma a e k la mantengono.
 */

void test_constant_propagation(int cond) {
    int k, a, x, b, y;

    // BB2
    k = 2;

    // BB3 (Branch)
    if (cond > 0) {
        // BB4
        a = 4;
        // BB5
        x = 5;
    } else {
        // BB6
        a = 4;
        // BB7
        x = 8;
    }

    // BB8: Punto di Meet
    // Qui l'analisi dovrebbe dire: 
    // IN[BB8] = { <k, 2>, <a, 4> } -> x è "Varying" perché 5 != 8
    
    // BB9 & BB10
    if (a == 4) {
        b = 2; // BB10
    }

    // BB11 & BB12
    y = a * b; // 4 * 2 = 8 (BB12)

    // BB13
    k = 5; 

    // BB14 & BB15
    std::printf("Risultati: k=%d, a=%d, x=%d, y=%d\n", k, a, x, y);
}

// int main(int argc, char** argv) {
//     int input = (argc > 1) ? 1 : 0;
//     test_constant_propagation(input);
//     return 0;
// }