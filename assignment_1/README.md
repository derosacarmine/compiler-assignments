# First Assignment
## Purpose
Implement three LLVM passes that achieve the following optimizations:
1. ***Algebraic Identity:***
    - $ 𝑥 + 0 = 0 + 𝑥 ⇒𝑥$
    - $ 𝑥 × 1 = 1 × 𝑥 ⇒𝑥 $
2. ***Strength Reduction (more advanced)***
    - $ 15 × 𝑥 = 𝑥 × 15 ⇒ (𝑥 ≪ 4) – x $
    - $ y = x / 8 ⇒ y = x >> 3 $
3. ***Multi-Instruction Optimization***
    - $ 𝑎 = 𝑏 + 1, 𝑐 = 𝑎 − 1 ⇒𝑎 = 𝑏 + 1, 𝑐 = b $

## Code Explanation

This is an **LLVM compiler plugin** that implements two optimization passes operating on LLVM's Intermediate Representation (IR).

---

### 1. `AlgebraicIdentity` — Algebraic Identity

Eliminates useless operations based on algebraic identity rules:

**Addition with zero:** `x + 0 = x` or `0 + x = x`
- If one operand is the constant `0`, the instruction is replaced directly with the other operand.

**Multiplication by one:** `x * 1 = x` or `1 * x = x`
- Same logic: if one operand is `1`, the other is used directly.

In both cases, the original instruction is removed with `eraseFromParent()`.

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
