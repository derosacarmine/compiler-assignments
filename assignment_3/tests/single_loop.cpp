int fun(int n, int x, int y, int* array) {
    n =2;
    int invariant_calc = 0;
    int sum = 0;
    int b = 3;

    int i = 0;

    do {

        int a = b+1;
        
        // Condizione 1: C'è un'uscita anticipata (Early Exit). 
        // Questo significa che il blocco successivo NON domina l'uscita del loop.
        if (array[i] < 0) {
            continue; 
        }

        // Condizione 2: L'istruzione è condizionale.
        if (x > 10) {
            // ISTRUZIONE INVARIANTE: 'x' e 'y' non vengono mai modificati nel loop.
            // In LLVM IR sarà un'istruzione "add" (Safe to Speculate).
            // Entrerà nell'invariantSet.
            invariant_calc = x + y;
        }

        sum += array[i];

        i++;
    } while (i < n);

    // Condizione 3: invariant_calc viene usata FUORI dal loop.
    // Di conseguenza, isDeadAfterLoop() restituirà 'false'.
    return sum + invariant_calc;
}