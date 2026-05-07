void fun(int n, int m){

    int b=3;
    int c=4;
    int e;

    int y;

    for(int i=0;i<n;i++){
        int a = b+c; //loop invariant

        if(a<=10){
            e = 2;
        }
        else {
            e = 3;
        }
        for(int j=0; j < m; ++j) {
            int x = a + 3; //outer loop invariant
            y = i + x; //inner loop invariant

            int z = j + 2;
        }
        
        int d = a+1;
        int f = e+2;
    }
}