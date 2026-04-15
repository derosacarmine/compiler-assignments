# First Assignment
## Purpose
Implement three LLVM passes that achieve the following optimizations:
1. ***Algebraic Identity***
2. ***Strength Reduction***
3. ***Multi-Instruction Optimization***
## How To Use
### Prepare the environment
- LLVM-19 and clang++ 19 are required (in particular the commands opt and clang++ must be available)
- To prepare the environment, you can run the script init.sh which will ask you to insert the path to your llvm installation
- Alternatively, you can manually prepare the environment and compile the plugin using
```bash
export LLVM_DIR=path/to/llvm
mkdir build
cd build
cmake -DLT_LLVM_INSTALL_DIR=$LLVM_DIR ..
make
 ```

### Running the optimizer
- To automatically run the optimizer for all .cpp tests, it's possible to use optimize_test.sh:
```bash
./optimize_test.sh -t <test_dir_path> -p <plugin_path> <pass1> <pass2> ...
```
The possible passes are **algebraic-identity, strength-reduction** and **multi-instruction**

- Alternatively, it is possibile to run the plugin for a specific .ll file using:
```bash
opt -load-pass-plugin <plugin_path> -passes=<passes_to_execute> <input_file> -S -o <output_file>
```
---

### Generate the documentation
Doxygen is required to create the documentation, run the following command inside the "assignment_1" directory:
```bash
doxygen
```
this will create a "doc" directory with the various files in html and latex

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
- If the divisor is not a power of 2: ottimizzazione della divisione intera tramite il metodo di **Granlund-Montgomery**.

    L'obiettivo è sostituire l'operazione di divisione (`SDIV`), computazionalmente costosa, con una sequenza di operazioni molto più veloci: **Moltiplicazione, Addizione e Shift**.

    ### 1. Il Concetto Matematico
    Dividere per un numero $d$ equivale a moltiplicare per il suo inverso $1/d$. Poiché lavoriamo con numeri interi, approssimiamo $1/d$ usando una frazione con potenza di 2:
    $$\frac{n}{d} \approx \frac{n \times M}{2^k}$$
    Dove **$M$** è la **Magic Constant** e **$k$** è la precisione dello shift. Il teorema garantisce che esista un $M$ tale che il risultato della divisione intera sia identico per ogni $n$ a 32 bit.

    ### 2. L'Algoritmo di Ricerca (Il Loop)
    Il codice cerca iterativamente la precisione minima $k$:
    * **Precisione incrementale:** Si parte da un bit e si aumenta finché l'errore di approssimazione di $M/2^k$ è così piccolo da non influenzare mai il quoziente intero nel range $[-2^{31}, 2^{31}-1]$.
    * **Condizione di stop:** Il loop termina quando la distanza tra l'approssimazione per difetto ($q1$) e quella per eccesso ($q2$) è sufficiente a coprire l'intero range dei possibili dividendi.

    m_low e m_high sono **i limiti (inferiore e superiore) del numeratore della frazione** che stiamo usando per approssimare $1/d$.

    * **`m_low` (Lower Bound):** È il valore intero più piccolo che, diviso per la potenza di 2 ($2^{32+precision}$), si avvicina a $1/d$ "da sinistra". Se usassimo un numero più piccolo di `m_low`, l'errore di approssimazione sarebbe troppo grande e il quoziente risulterebbe sbagliato (troppo basso) per alcuni valori di $n$.
    * **`m_high` (Upper Bound):** È il valore intero più grande accettabile. Rappresenta la soglia oltre la quale l'approssimazione diventa troppo alta, rischiando di far sballare il calcolo del quoziente per eccesso.

    ### L'Intervallo di Confidenza
    L'algoritmo funziona perché cerca un numero intero $M$ (il **Magic Number**) che cada dentro questo intervallo:
    $$m\_low \le M \le m\_high$$

    Finché questo intervallo contiene almeno un numero intero, la divisione approssimata sarà identica alla divisione reale per ogni possibile dividendo a 32 bit.

    ### Perché si usa `m_high`?
    Nel codice, assegniamo `magic_number = (int64_t)m_high;`. Si sceglie il limite superiore perché garantisce che l'approssimazione sia sempre "sufficiente" a coprire il resto della divisione, specialmente quando si lavora con la logica del `ceil` (arrotondamento per eccesso) necessaria per il teorema di Granlund-Montgomery.

    Immagina una retta numerica dove vuoi piazzare $1/d$:
    1.  **`m_low` / PotenzaDi2** è appena a sinistra di $1/d$.
    2.  **`m_high` / PotenzaDi2** è appena a destra di $1/d$.
    3.  Il **loop di riduzione** (il `while`) stringe la "PotenzaDi2" finché l'intervallo tra `m_low` e `m_high` è ancora abbastanza largo da contenere un numero intero. Se l'intervallo si stringe troppo e non ci sono più interi in mezzo, l'algoritmo si ferma alla precisione precedente.

    ### 3. La Sequenza di Istruzioni Generata
    Il risultato finale non è una sola istruzione, ma una "ricetta" di calcolo:

    1.  **MULH (Multiply High):** Si moltiplica il dividendo $n$ per la costante magica $M$ a 64 bit e si estraggono i 32 bit superiori. Questo simula la divisione per $2^{32}$.
    2.  **Correzione Additiva:** Se $M$ è troppo grande (appare come negativo a 32 bit), si somma il dividendo originale al risultato parziale per compensare l'overflow.
    3.  **Shift finale:** Si esegue uno shift a destra (`AShr`) dei bit rimanenti ($k - 32$).
    4.  **Correzione del Segno:** Poiché lo shift aritmetico arrotonda verso $-\infty$ ma la divisione C richiede l'arrotondamento verso $0$, si estrae il bit di segno e lo si somma al risultato (se il numero è negativo, si aggiunge 1).

    ### 4. Esempio Pratico (Divisore 7)
    * **Costante calcolata:** $M = 2454267027$ (che è $> 2^{31}$, quindi richiede correzione).
    * **Shift:** $k = 34$ (ovvero un `MULH` seguito da uno shift di 2).
    * **Vantaggio:** Una `MUL` e due `SHR` richiedono circa 3-5 cicli di clock, contro i 20-80 cicli di una `IDIV` hardware.


    ### Schema Riassuntivo della Logica
    | Componente | Funzione |
    | :--- | :--- |
    | **Magic Number ($M$)** | Approssima $1/d$ come numeratore intero. |
    | **Shift ($k$)** | Gestisce il denominatore $2^k$ della frazione. |
    | **MULH** | Esegue la moltiplicazione e lo shift "gratuito" dei primi 32 bit. |
    | **Sign Correction** | Garantisce che `-5 / 2` faccia `-2` e non `-3`. |


    Esempio: x / 17


    1.  **Estensione a 64 bit (`sext`)**: Estende il valore di input per evitare overflow durante la moltiplicazione con il "numero magico".
    2.  **Moltiplicazione per il reciproco (`mul`)**: Moltiplica per `4042322161`. Questo numero è il moltiplicatore magico calcolato appositamente per il divisore 17.
    3.  **Estrazione della parte alta (`ashr ... 32`)**: Prende i 32 bit più significativi del risultato a 64 bit.
    4.  **Correzione e Shift (`add`, `ashr ... 4`)**: Somma il valore originale e sposta di 4 posizioni. Questo serve a compensare l'approssimazione usata nella moltiplicazione.
    5.  **Gestione del segno (`lshr ... 31`)**: Questa è la parte fondamentale per i numeri interi con segno (`signed`). Serve a garantire che il risultato sia arrotondato correttamente verso lo zero anche per i numeri negativi.

    Se prendi un numero qualsiasi, ad esempio $34$, e segui quei passaggi:
    * $34 \times 4042322161 = 137438953474$
    * Sposta a destra di 32 bit $\approx 31.99$ (troncato a 31)
    * Somma l'originale ($31 + 34 = 65$)
    * Sposta a destra di 4 ($65 / 16 \approx 4$, ma qui la logica binaria darà esattamente 2).
    * Risultato: **2**. ($34 / 17 = 2$).

    | Operazione | Costo in Cicli CPU (approssimativo) |
    | :--- | :--- |
    | `sdiv` (Divisione) | **20 - 80 cicli** |
    | `mul` + `ashr` + `add` | **5-10 cicli totali** |





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
- `a = b & 1, c = a & 2 --> c = 0`
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

At the bottom, the three passes are registered as LLVM passes under the names:
- `algebraic-identity`
- `strength-reduction`
- `multi-instruction` 

