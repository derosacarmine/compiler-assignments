# First Assignment
## Purpose
Implement three LLVM passes that achieve the following optimizations:
1. ***Algebraic Identity***
2. ***Strength Reduction***
3. ***Multi-Instruction Optimization***
## How To Test
- To compile, go into the build folder and run the make command
- To test, go into assignment_1 and run:
    ./optimize_tests.sh -t ./tests/ -p ./build/libLocalOpts.so optimization-name

---

## Code Explanation

This is an **LLVM compiler plugin** that implements two optimization passes operating on LLVM's Intermediate Representation (IR).

---

### 1. `AlgebraicIdentity` — Algebraic Identity

Eliminates useless operations based on algebraic identity rules:

#### **Single costant identities**

In these cases, if a constant operand has a specific value (such as 0 or 1), an entire instruction can be substituted a variable or constant.

*   **Zero addition:** `x + 0 = x`, `0 + x = x`.
    
*   **Zero subtraction:** `x - 0 = x`.

*   **Multiplication by 0:** `x * 0 = 0`, `0 * x = 0`.
    
*   **Multiplication by 1:** `x * 1 = x`, `1 * x = x`.
    
*   **Division by one:** `x / 1 = x`.
    
*   **Shift (Left, Logical Right, Arithmetic Right) by zero:** `x << 0 = x`, `x >> 0 = x`.
    
*   **Bitwise AND by -1 and 0:** `x & -1 = x`, `x & 0 = 0`.
    
*   **Bitwise OR/XOR by zero:** `x | 0 = x`, `x ^ 0 = x`.

*   **Modulo by 1:** `x % 1 = 0`.
    

#### **Identities with the same operands**

In these cases, an instruction can be replaced if the two operands are the same.

*   **Identical operands subtraction:** `x - x = 0`.
    
*   **Identical operands division:** `x / x = 1`.
    
*   **Identical operands in bitwise AND/OR:** `x & x = x`, `x | x = x`.
    
*   **Identical operands  XOR:** `x ^ x = 0`.

*   **Identical operands in modulo:** `x % x = 0`.

---

### 2. `StrengthReduction` — Strength Reduction

Replaces **expensive** operations (multiplications and divisions) with **cheaper** ones:

**Multiplication (`Mul`):**
| Case | Example | Replacement |
|---|---|---|
| Power of 2 | `x * 8` | `x << 3` |
| Power of 2 − 1 | `x * 7` | `(x << 3) - x` |
| Power of 2 + 1 | `x * 9` | `(x << 3) + x` |
| Multiplication by -1 | `x * (-1)` | `0 - x` |
| Sum of 2 powers | `x * 40` | `(x << 5) + (x << 3)` |
| Sub of 2 powers | `x * 56` | `(x << 6) - (x << 3)` | 

**Integer Division (`SDiv/Udiv`):**
- If the divisor is a power of 2 → arithmetic right shift (`AShr`)
- Example: `x / 4` → `x >> 2`

**Integer Signed Remainder (`SRem`):**
| Case | Example | Replacement |
|---|---|---|
| Power of 2 | `x % 8` | `x - (((((x >> 32-1) << 32-log2(cst)) + x) >> log2(cst)) << log2(cst))` |
| Negative var | `-x % 8` | `-x - (((((-x >> 32-1) << 32-log2(cst)) - x) >> log2(cst)) << log2(cst))` |

**Integer Unsigned Remainder (`URem`):**
- if the constant is a power of 2 → subtract 1 from the constant and do an (`And`)
- Example: `x % 4` → `x & 3`

---

### 3. `Multi-Instruction` — Multi-Instruction
Instructions can be removed if their value can be obtained from previous instructions:

- `a = b+1, c = a-1 --> c = b`
- `a = b*3, c = a/3 --> c = b`
- `a = b << 3, c = a >> 3 --> c = b`
- `a = b & 5, c = a & 5 --> c = a`
- `a = b | 5, c = a | 5 --> c = a`
- `a = b ^ 5, c = a ^ 5 --> c = b`
- `a = b ^ 3, c = a ^ 7, d = c ^ 4 --> d = b`

---

### Shared Structure

Both passes follow the same three-level traversal:

```
run() → runOnFunction() → runOnBasicBlock()
```

They iterate over every **BasicBlock** in the function, and for each instruction check whether the optimization applies. The `++iter` is done **before** modifying the instruction to avoid iterator invalidation.

---

### Plugin Registration

At the bottom, the two passes are registered as LLVM passes under the names:
- `algebraic-identity`
- `strength-reduction`


## Code Explanation and Similarities with Java


```cpp
std::function<bool(const ConstantInt*)> predicate;
```
In Java:
```java
interface Predicate { boolean test(ConstantInt c); }
```
`using` is a type alias (like `typedef`). `std::function<bool(const ConstantInt*)>` is a type representing **any callable** (lambda, function, functor) that takes a `const ConstantInt*` and returns `bool`. It is the equivalent of `Function<ConstantInt, Boolean>` in Java.

---

In cpp:

```cpp
map<unsigned, function<Value*(ConstantInt*, Value*)constantMap; 
```
In Java:
```java
//Integer --> ConstantInt, Object --> Value, Object --> Value (the return type)
Map<Integer, BiFunction<Integer, Object, Object>> constantMap = new HashMap<>();

// add ad element
constantMap.put(42, (constInt, value) -> {
    // logic here
    return value;
});

// function recall
Object result = constantMap.get(42).apply(10, someValue);
```
The key is `unsigned` which here represents the operation code of the LLVM instruction; the value associated with the key is a fz that returns a Value* and has as parameters (ConstantInt*, Value*) 

---
In Cpp:

```cpp
function<Value*(ConstantInt* c, Value* v)> ifZeroReturnV = [](ConstantInt* c, Value* v) -> Value* { return c->isZero() ? v : nullptr;};
```
In Java:
```Java
BiFunction<ConstantInt, Value, Value> ifZeroReturnV = (c, v) -> c.isZero() ? v : null;
```
In cpp: `lambda [](...) -> returnType {}` in Java(implicit return type): `lambda (...) -> ...`

---

```cpp
using Fn = function<Value*(ConstantInt*, Value*)>;
    
    static Fn firstOf(vector<Fn> fns) {
        return [fns](ConstantInt* c, Value* v) -> Value* {
            for (auto& fn : fns)
                if (auto* r = fn(c, v)) return r;
            return nullptr;
        };
    }
```
The [fns] brackets are the lambda's capture list in C++.
This means that the lambda copies the fns variable (the function vector) from the external context, so it can use it internally. You can choose capture for copy [fns] or for reference [&fns].
In Java the capture is automatic and implicit so:

```Java
static BiFunction<ConstantInt, Value, Value> firstOf(List<BiFunction<ConstantInt, Value, Value>> fns) {
    return (c, v) -> {
        for (var fn : fns)  {
            Value r = fn.apply(c, v);
            if (r != null) return r;
        }
        return null;
    };
}
```
always capture by reference (but the variable must be effectively final)

`auto` in C++ is like `var` in Java — the compiler automatically infers the type. Here `auto*` infers that r is of type `Value*` and `auto&` is a reference to the variable, avoids copying to make any changes permanent.
```cpp
for (auto& fn : fns)  // iterates by reference, does not copy every element
for (auto fn : fns)   // copy each element of the vector
```
---

```cpp
auto it = identityMap.find(instr.getOpcode());
```
In Java:
```java
var entry = identityMap.get(instr.getOpcode());
```
`find()` It returns an iterator, not the value itself. An iterator in C++ is similar to a cursor/pointer to a location in the map.

---

```cpp
if (it == identityMap.end()) continue;
```
In Java:
```java
if (entry == null) continue;
```
`end()` returns a sentinel iterator indicating "not found." You can't use `null` because C++ iterators aren't nullable pointers—you must match with `end()`.

---


```cpp
  std::optional<Instruction::BinaryOps> secondOp; // nullopt = only a shift is needed
```

optional in C++ is a value that may or may not be present (presence or absence of a value).

---

```cpp
[](Value* var, ConstantInt* c) -> std::pair<Instruction*, Instruction*> {
```
In Java:
```java
BiFunction<Value, ConstantInt, Pair<Instruction, Instruction>> builder = (var, c) -> ...
```
The `-> std::pair<...>` explicitly specifies the lambda's return type (necessary when it's complex). `std::pair` is like a generic pair—in Java, you'd use something like `Map.Entry` or a record class.

The empty capture list `[]` means the lambda captures nothing from the outside. If I had written `[&]`, it would capture everything by reference, and `[=]` everything by copy. In Java, lambdas automatically capture `effectively final` variables—in C++, you have to be explicit.

---

```cpp
std::vector<mulRecduction> mulReductions = { ... };
```
In Java:
```java
List<Map.Entry<mulReduction>> mulReductions = List.of(...);
```
`std::vector` is like `ArrayList`. A `vector` is used instead of a `map` because the cases must be **checked in order** and are mutually exclusive—a map does not guarantee order of visit and would have a function as its key (not easily hashable).
