# fib.py: Python source, shown with colours in the Code app.
def fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a

if __name__ == "__main__":
    for i in range(10):
        print(i, fib(i))
