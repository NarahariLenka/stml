# Scalable Thread Management Library (STML)

A single-file implementation of a C++ Thread Management Library designed for high performance and ease of use.

## Features
- **Synchronization Primitives**: Mutex, SpinLock, Semaphore, ConditionVariable.
- **Thread Safety**: Robust Task Queue for concurrent access.
- **Scalability**: Thread Pool implementation for efficient task management.
- **Examples**: Built-in test suite and usage examples.

## Compilation

To compile the library and examples, use a C++17 compliant compiler with pthread support:

```bash
g++ -o stml main.cpp -std=c++17 -pthread
```

## Usage

Run the executable to see the demos:

```bash
./stml
```

You will be prompted to enter the number of threads for the pool. 
The program will run several tests:
1. Semaphore synchronization capability.
2. Basic Thread Pool execution.
3. Stress testing with 10,000 tasks.

## License
[MIT](LICENSE)
