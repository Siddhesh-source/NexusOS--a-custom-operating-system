/* hello.c: a small C program to read in the Code app.
 * BharatOS runs shell scripts (.sh) from Code today; C programs run as
 * apps once user mode arrives. */
#include <stdio.h>

static int square(int x)
{
    return x * x;
}

int main(void)
{
    const char *name = "BharatOS";
    for (int i = 1; i <= 5; i++)
        printf("%d squared is %d\n", i, square(i));
    printf("Hello from %s!\n", name);
    return 0;
}
