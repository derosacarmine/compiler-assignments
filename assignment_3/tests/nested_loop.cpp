void fun(int n, int m){

    int b=3;
    int c=4;
    int e;

    int y;

    for(int i=0;i<n;i++){
        int a = b+c;

        if(a<=10){
            e = 2;
        }
        else {
            e = 3;
        }

        //we check that the instructions are moved out of the loop they're invariant in and not just every loop
        //x should be moved outside both fors and y only outside the second for
        for(int j=0; j < m; ++j) {
            int x = a + 3;
            y = i + x;

            int z = j + 2;
        }
        
        int d = a+1;
        int f = e+2;
    }
}