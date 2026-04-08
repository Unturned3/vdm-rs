
#define T double

void swaprow(T* a, T* b, int len)
{
    for (int i = 0; i < len; i++)
        swap(T, a[i], b[i]);
}

T round_to_0(T v, T eps)
{
    if (-eps < v && v < eps)
        return 0;
    return v;
}

// Print a n*m matrix (assuming T is float/double)
void printMat(T* a, int n, int m)
{
    for (int i = 0; i < n; i++) {
        printf("%5.2f", round_to_0(a[i * m], 1e-6));
        for (int j = 1; j < m; j++) {
            printf(", %5.2f", round_to_0(a[i * m + j], 1e-6));
        }
        printf("\n");
    }
}

// array multiply and accumulate: a[] = a[] + b[] * c
void amac(T* a, T* b, T c, int len)
{
    for (int i = 0; i < len; i++)
        a[i] = add(a[i], mul(b[i], c));
}

// array multiply: a[] = b[] * c
void amul(T* a, T* b, T c, int len)
{
    for (int i = 0; i < len; i++)
        a[i] = mul(b[i], c);
}

// Create n*n identity matrix
void identity(T* a, int n)
{
    memset(a, 0, n * n * sizeof(T));
    for (int i = 0; i < n; i++)
        a[i * (n + 1)] = 1;
}

// Original implementation
// Returns negative if matrix is singular
// Otherwise, returns the number of row swaps (non-negative integer)
int invert_mat_naive(T* a, int n)
{
    T b[n * n];
    identity(b, n);
    int ar[n], swapCnt = 0; // ar[i]: physical index of logical row i
    for (int i = 0; i < n; i++)
        ar[i] = i; // Initially, every row is at their original row position.

    for (int i = 0; i < n; i++) { // Gaussian elimination over the rows i
        int j;

        for (j = i; j < n; j++) // Search for pivot starting from current row i
            if (a[ar[j] * n + i] != 0)
                break; // Found a valid pivot at a[j, i]

        if (j >= n) {
            fprintf(stderr, "Error: cannot invert matrix!\n");
            return -1; // Pivot not found
        }
        if (i != j) {
#ifdef debug
            fprintf(stderr, "Info: row swap\n");
#endif
            int t = ar[i];
            ar[i] = ar[j];
            ar[j] = t;
            swapCnt++;
        }
        T ipiv = div(1, a[ar[i] * n + i]);
        amul(a + ar[i] * n, a + ar[i] * n, ipiv, n); // Multiply row ar[i] of a by ipiv
        amul(b + ar[i] * n, b + ar[i] * n, ipiv, n); // Multiply row ar[i] of b by ipiv
// piv = a[ar[i] * n + i];
#ifdef debug
        printMat(a, n, n);
        printf("\n");
#endif
        for (j = 0; j < n; j++) {
            if (j != i) {
                T scale = mul(-1, a[ar[j] * n + i]);
                amac(a + ar[j] * n, a + ar[i] * n, scale, n);
                amac(b + ar[j] * n, b + ar[i] * n, scale, n);
#ifdef debug
                printMat(a, n, n);
                printf("\n");
#endif
            }
        }
    }

    for (int i = 0; i < n; i++) // Actually swap the rows
        // memcpy(a + ar[i] * n, b + i * n, sizeof(T) * n);	// WRONG!
        memcpy(a + i * n, b + ar[i] * n, sizeof(T) * n); // WRONG!

#ifdef debug
    printf("a: \n");
    printMat(a, n, n);
#endif
    return swapCnt;
}

// Interpretation: https://chatgpt.com/c/69aa68de-93a4-8325-a3bd-d8f24e2ff3cb
// Invert matrix in-place
// Returns negative if matrix is singular
// Otherwise, returns the number of row swaps needed (non-negative integer)
int invert_mat(T* a, int n)
{
    int ar[n];
    for (int i = 0; i < n; i++)
        ar[i] = i;

    for (int i = 0; i < n; i++) {
        int j;

        for (j = i; j < n; j++)
            if (a[j * n + i] != 0)
                break; // row j has a valid pivot at the i-th column

        if (j >= n) {
            fprintf(stderr, "Error: cannot invert matrix!\n");
            return -1; // no valid pivot found
        }
        if (i != j) { // row swap
            swap(int, ar[i], ar[j]);
            swaprow(a + i * n, a + j * n, n);
        }
        T ipiv = div(1, a[i * n + i]); // 1 over pivot element
        amul(a + i * n, a + i * n, ipiv, n);
        a[i * n + i] = ipiv;
        for (j = 0; j < n; j++) {
            if (j != i) {
                T scale = mul(-1, a[j * n + i]);
                a[j * n + i] = 0;
                amac(a + j * n, a + i * n, scale, n);
            }
        }
    }

    int swapCnt = 0;

    for (int i = 0; i < n; i++) {
        /*
        // WRONG! This version only works for pairwise swaps.
        // If we have a swap cycle of 3 or more, e.g. a -> b, b-> c, c -> a, this fails.
        if (ar[i] != i) {
                swapCnt++;
                for (int j=0; j<n; j++)	 // Swap columns i, ar[i]
                        swap(T, a[j * n + i], a[j * n + ar[i]]);
                swap(int, ar[ar[i]], ar[i]);
        }
        */
        while (ar[i] != i) {
            swapCnt++;
            for (int j = 0; j < n; j++) // Swap columns i, ar[i]
                swap(T, a[j * n + i], a[j * n + ar[i]]);
            swap(int, ar[ar[i]], ar[i]);
        }
    }

    return swapCnt;
}

// Multiplies two n*n matrices (naive O(n^3) algorithm)
void mat_mul_naive(T* a, T* b, T* c, int n)
{
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            c[i * n + j] = 0;
            for (int k = 0; k < n; k++)
                c[i * n + j] += a[i * n + k] * b[k * n + j];
        }
    }
}

// Cache-optimized naive matmul
void mat_mul(T* a, T* b, T* c, int n)
{
    memset(c, 0, n * n * sizeof(T));
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < n; k++) {
            double aik = a[i * n + k];
            for (int j = 0; j < n; j++) {
                c[i * n + j] += aik * b[k * n + j];
            }
        }
    }
}
