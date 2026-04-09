
- Per avviarlo con tutti e 3:
./run.sh -t ./tests \
         --vbe ./build/VeryBusyExpressions.so \
         --cp  ./build/ConstantPropagation.so  \
         --dom ./build/DominatorAnalysis.so

- singolarmente:
    ./run.sh -t ./tests --vbe ./build/VeryBusyExpressions.so

### Cosa cambia rispetto al vecchio script

| Vecchio | Nuovo |
|---|---|
| Un solo `-p <plugin>` globale | `--vbe / --cp / --dom` per plugin distinti |
| Pass scelte a mano come argomenti | Pass fisse per ogni file (nella mappa `PASS_MAP`) |
| Itera su tutti i `.cpp` con lo stesso plugin | Ogni `.cpp` usa solo il suo plugin |
| Errore se pass non in `ALLOWED_PASSES` | Skip silenzioso se nessun plugin associato |

L'unica cosa che dovrai adattare sono i **nomi delle pass** dentro `PASS_MAP` — devono corrispondere esattamente a quello che registri con `registerPipelineParsingCallback` nei tuoi nuovi plugin.