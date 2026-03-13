# First Assignment
## Purpose
Implement three LLVM passes that achieve the following optimizations:
1. ***Algebraic Identity***
2. ***Strength Reduction***
3. ***Multi-Instruction Optimization***
## Code Explanation

This is an **LLVM compiler plugin** that implements two optimization passes operating on LLVM's Intermediate Representation (IR).

---

### 1. `AlgebraicIdentity` — Algebraic Identity

Eliminates useless operations based on algebraic identity rules:

#### **Single costant identities**

In these cases, if a constant operand has a specific value (such as 0 or 1), an entire instruction can be substituted a variable or constant.

*   **Zero addition:** `x + 0 = x`, `0 + x = x`.
    
*   **Zero subtraction:** `x - 0 = x`.

*   **Multiplication by 0:** `x * 0 = 0` o `0 * x = 0`.
    
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

Replaces **expensive** operations (multiplications and divisions) with **cheaper** ones (bitwise shifts):

**Multiplication (`Mul`):**
| Case | Example | Replacement |
|---|---|---|
| Power of 2 | `x * 8` | `x << 3` |
| Power of 2 − 1 | `x * 7` | `(x << 3) - x` |
| Power of 2 + 1 | `x * 9` | `(x << 3) + x` |

**Integer Division (`SDiv`):**
- If the divisor is a power of 2 → arithmetic right shift (`AShr`)
- Example: `x / 4` → `x >> 2`

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


## Refactoring (Da tenere temporaneamente per Manu perchè sennò impazzisce)

### AlgebraicIdentity — la mappa

```cpp
using Predicate = std::function<bool(const ConstantInt*)>;
```
In Java scriveresti:
```java
interface Predicate { boolean test(ConstantInt c); }
```
`using` è un alias di tipo (come `typedef`). `std::function<bool(const ConstantInt*)>` è un tipo che rappresenta **qualsiasi callable** (lambda, funzione, functor) che prende un `const ConstantInt*` e ritorna `bool`. È l'equivalente di `Function<ConstantInt, Boolean>` in Java.

---

```cpp
using Identity = std::map<unsigned, Predicate>;
```
Equivalente Java:
```java
Map<Integer, Predicate> identityMap;
```
`std::map` è una mappa ordinata (come `TreeMap` in Java). La chiave è `unsigned` (intero senza segno, non esiste in Java), che qui rappresenta il codice operazione dell'istruzione LLVM.

---

```cpp
Identity identityMap = {
    {Instruction::Add, [](const ConstantInt* c) { return c->isZero(); }},
    {Instruction::Mul, [](const ConstantInt* c) { return c->isOne();  }},
};
```
In Java:
```java
Map<Integer, Predicate> identityMap = Map.of(
    Instruction.ADD, c -> c.isZero(),
    Instruction.MUL, c -> c.isOne()
);
```
Le `{}` sono **initializer list**, un modo di inizializzare strutture direttamente inline. `[](const ConstantInt* c) { return c->isZero(); }` è una **lambda**: `[]` è la capture list (ne parliamo dopo), poi ci sono parametri e corpo. `->` in C++ su un puntatore è come `.` in Java — accede a un membro tramite puntatore.

---

### Il loop

```cpp
for (auto& instr : B) {
```
In Java:
```java
for (Instruction instr : B) {
```
`auto&` significa "deduci il tipo automaticamente, e prendilo per **riferimento**". Il riferimento `&` è importante: senza, copieresti l'istruzione, e le modifiche non avrebbero effetto. In Java gli oggetti sono sempre passati per riferimento implicito, in C++ devi essere esplicito.

---

```cpp
auto it = identityMap.find(instr.getOpcode());
```
In Java:
```java
var entry = identityMap.get(instr.getOpcode());
```
`find()` ritorna un **iteratore**, non il valore direttamente. Un iteratore in C++ è simile a un cursore/puntatore a una posizione nella mappa.

---

```cpp
if (it == identityMap.end()) continue;
```
In Java scriveresti:
```java
if (entry == null) continue;
```
`end()` ritorna un iteratore sentinella che indica "non trovato". Non puoi usare `null` perché gli iteratori C++ non sono puntatori nullable — devi confrontare con `end()`.

---

```cpp
for (int i : {0, 1}) {
```
In Java:
```java
for (int i : new int[]{0, 1}) {
```
Stessa idea, ma in C++ puoi usare direttamente una `initializer_list` inline senza creare un array esplicitamente.

---

```cpp
if (auto* c = dyn_cast<ConstantInt>(instr.getOperand(i))) {
```
In Java:
```java
if (instr.getOperand(i) instanceof ConstantInt c) {
```
`dyn_cast<T>` è il cast sicuro di LLVM: se l'operando è un `ConstantInt`, ritorna il puntatore castato, altrimenti `nullptr`. La novità C++ è che puoi dichiarare la variabile `c` **dentro la condizione dell'if** — esiste solo dentro quel blocco.

---

```cpp
if (it->second(c)) {
```
`it` è un iteratore a una coppia `{chiave, valore}`. In C++ le coppie hanno `.first` e `.second` invece di `.getKey()` e `.getValue()`. Quindi `it->second` è la `Predicate`, e `(c)` la invoca come se fosse una funzione. In Java:
```java
if (entry.getValue().test(c)) {
```

---

```cpp
instr.replaceAllUsesWith(instr.getOperand(1 - i));
instr.eraseFromParent();
break;
```
Niente di speciale qui rispetto a Java — sono chiamate a metodi LLVM. `1 - i` è il trucco per ottenere l'altro operando: se `i == 0` prendi `1`, se `i == 1` prendi `0`.

---

## StrengthReduction — la capture list nelle lambda

```cpp
[](Value* var, ConstantInt* c) -> std::pair<Instruction*, Instruction*> {
```
In Java:
```java
BiFunction<Value, ConstantInt, Pair<Instruction, Instruction>> builder = (var, c) -> ...
```
Il `-> std::pair<...>` specifica esplicitamente il tipo di ritorno della lambda (necessario quando è complesso). `std::pair` è come una coppia generica — in Java useresti qualcosa come `Map.Entry` o una record class.

La **capture list** `[]` vuota significa che la lambda **non cattura nulla** dall'esterno. Se avessi scritto `[&]` catturerebbe tutto per riferimento, `[=]` tutto per copia. In Java le lambda catturano automaticamente le variabili `effectively final` — in C++ devi essere esplicito.

---

```cpp
std::vector<std::pair<Predicate, Builder>> mulReductions = { ... };
```
In Java:
```java
List<Map.Entry<Predicate, Builder>> mulReductions = List.of(...);
```
`std::vector` è come `ArrayList`. Si usa un `vector` invece di una `map` perché i casi vanno **controllati in ordine** e sono mutuamente esclusivi — una mappa non garantisce ordine di visita ed avrebbe come chiave una funzione (non hashabile facilmente).

---

```cpp
for (auto& [pred, build] : mulReductions) {
```
Questo è **structured binding** (C++17), equivalente a:
```java
for (var entry : mulReductions) {
    var pred  = entry.getKey();
    var build = entry.getValue();
```
Destruttura automaticamente la coppia in due variabili nominative. Molto più leggibile.

---

```cpp
auto [first, second] = build(variable_value, const_value);
```
Stesso concetto: destruttura il `std::pair` ritornato da `build` direttamente in due variabili. In Java dovresti fare:
```java
var result = build.apply(var, c);
var first  = result.getKey();
var second = result.getValue();
```

---

Le differenze principali da tenere a mente rispetto a Java sono: i **riferimenti** (`&`) che devi gestire esplicitamente, gli **iteratori** al posto dei nullable, le **lambda con capture list** esplicita, e gli **structured bindings** per destrutturare coppie/tuple.
