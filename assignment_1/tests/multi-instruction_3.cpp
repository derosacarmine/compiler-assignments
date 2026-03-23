int fun (int x)
{
    int a = x & 5;
    int b = a & 5;

    int c = x | 5;
    int d = c | 5;

    int e = x ^ 5;
    int f = e ^ 5;

    int w = x ^ 3;
    int y = w ^ 7;
    int z = y ^ 4;

    return 0;
}