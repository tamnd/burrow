# Cryptography

burrow's `crypto` packages are Go's, with the same algorithms, the same answers and the same panics. The hashes are covered in [hashing.md](hashing.md). This page covers the rest, one package at a time as they land.

## crypto/subtle

`burrow/crypto/subtle.h` is what the other packages build on to keep secrets out of timing. Code that stops at the first wrong byte of a MAC, or branches on a bit of a key, takes a different time depending on the secret, and that difference can be measured from across a network. The functions here take the same time whatever the bytes are.

Checking a tag is the one most programs need:

<!-- example: ../examples/crypto/subtle.c#verify -->
```c
static bool tag_ok(Slice got, Slice want) {
    return subtle_constant_time_compare(got, want) == 1;
}
```

The time depends on the length and nothing else. Slices of different lengths compare unequal at once, which is fine because the length of a tag is not a secret.

The answers are `Int`s that are 1 or 0, as in Go, so one can be passed straight on as the choice in a select or a copy:

<!-- example: ../examples/crypto/subtle.c#select -->
```c
Int is_y = subtle_constant_time_byte_eq(secret, 'y');
Int n = subtle_constant_time_select(is_y, 10, 20);
```

<!-- example: ../examples/crypto/subtle.c#copy -->
```c
Byte key[4] = {1, 2, 3, 4};
Byte fallback[4] = {9, 9, 9, 9};
subtle_constant_time_copy(1 ^ is_y, bs(key, 4), bs(fallback, 4));
```

The copy writes every byte of `key` either way, so nothing about the choice shows in which memory was touched. Slices of different lengths panic with `subtle: slices have different lengths`, the same text as Go.

`subtle_xor_bytes` XORs two slices into a third and returns how many bytes it wrote, which is the shorter of the two inputs:

<!-- example: ../examples/crypto/subtle.c#xor -->
```c
Byte pad[5] = {0x10, 0x20, 0x30, 0x40, 0x50};
Byte msg[5] = {'h', 'e', 'l', 'l', 'o'};
Byte out[5];
Int n = subtle_xor_bytes(bs(out, 5), bs(msg, 5), bs(pad, 5));
```

The output may be one of the inputs, or apart from both. A destination that is too short, or one that overlaps an input without starting at the same place, panics before anything is written, as in Go.

### How the C stays constant time

Go has its compiler on its side here. It turns `x == y` in these functions into a single compare and set instruction, and nothing else. A C compiler makes no such promise, and is happy to turn a careful mask back into a branch when it can prove the two give the same answer. So burrow writes the comparisons as arithmetic on the bits and passes every mask through an empty asm statement, which tells GCC and Clang that the value could be anything and stops them reasoning past it. MSVC has no such statement, so the optimiser is turned off around these functions there. That makes them slower under MSVC and keeps them correct.

None of this helps code that branches on a secret itself, or uses one to pick which memory to read. The functions only keep their own promise.

### Data independent timing

Some processors have a mode in which the instructions that have it take the same time whatever their operands are. On arm64 it is DIT. `subtle_with_data_independent_timing` turns it on, runs a function, and turns it back off afterwards, including when the function panics:

<!-- example: ../examples/crypto/subtle.c#dit -->
```c
static void sign(void *env) {
    Slice *digest = env;
    /* Arithmetic on the key goes here. On arm64 it runs with DIT on, and so
     * does any goroutine it starts. */
    (void)digest;
}

static void sign_with_dit(Slice digest) {
    subtle_with_data_independent_timing(BURROW_FN(Func, sign, &digest));
}
```

The mode belongs to the thread, and a goroutine can move between threads, so the scheduler keeps a flag on the goroutine and sets the mode on whichever thread runs it, the way Go's runtime does. A goroutine started inside the function has the mode on too. Calls can be nested, and only the outermost one turns the mode off.

Where the processor has no such mode, which is everywhere but arm64 today, the function just runs. burrow looks for DIT on Linux, FreeBSD and Apple systems. Go also looks on OpenBSD, which burrow does not do yet, and neither looks on Windows.

`GODEBUG=dataindependenttiming=1` turns the mode on for every thread the runtime starts and leaves it on, which is Go's setting of the same name.
