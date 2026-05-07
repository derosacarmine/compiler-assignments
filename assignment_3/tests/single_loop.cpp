int fun(int n, int x, int y, int* array) {
    n =2;
    int invariant_calc = 0;
    int sum = 0;
    int b = 3;

    int i = 0;

    do {

        int a = b+1; //loop invariant

        if (array[i] < 0) {
            continue; 
        }

        if (x > 10) {
            invariant_calc = x + y; //loop invariant
        }

        sum += array[i];

        i++;
    } while (i < n);

    return sum + invariant_calc;
}