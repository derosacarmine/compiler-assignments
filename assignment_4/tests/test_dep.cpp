int fun(int n) {
    int c=10;
    for(int i=0;i<n;i++){
        c = i;
    }
    for(int i=0;i<n;i++){
        int d = i+c;
    }
    return 0;
}