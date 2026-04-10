#!/bin/bash

# Ricerca automatica di clang++ e opt
find_llvm_bin() {
    if command -v clang++ &>/dev/null && command -v opt &>/dev/null; then
        return 0
    fi

    local search_paths=(
        /usr/lib/llvm-*/bin
        /usr/local/lib/llvm-*/bin
        /opt/llvm-*/bin
        /opt/homebrew/opt/llvm/bin
        "$HOME/llvm-*/bin"
        "$HOME/llvm/bin"
    )

    for pattern in "${search_paths[@]}"; do
        for dir in $pattern; do
            if [ -x "$dir/clang++" ] && [ -x "$dir/opt" ]; then
                export PATH="$dir:$PATH"
                echo "LLVM trovato in: $dir"
                return 0
            fi
        done
    done

    echo "Errore: clang++ e opt non trovati."
    exit 1
}

find_llvm_bin

declare -A PLUGIN_MAP
declare -A PASS_MAP

usage() {
    echo "Usage: $0 -t <test_dir> [options]"
    echo "Options:"
    echo "  --vbe <path>    Plugin .so per very_busy_expressions"
    echo "  --cp  <path>    Plugin .so per constant_propagation"
    echo "  --dom <path>    Plugin .so per dominator_analysis"
    exit 1
}

TEST_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -t)    TEST_DIR="$2";               shift 2 ;;
        --vbe) PLUGIN_MAP["very_busy_expressions"]="$2"
               PASS_MAP["very_busy_expressions"]="very-busy-expressions"
               shift 2 ;;
        --cp)  PLUGIN_MAP["constant_propagation"]="$2"
               PASS_MAP["constant_propagation"]="constant-propagation"
               shift 2 ;;
        --dom) PLUGIN_MAP["dominator_analysis"]="$2"
               PASS_MAP["dominator_analysis"]="dominator-analysis"
               shift 2 ;;
        -h|--help) usage ;;
        *) echo "Argomento non riconosciuto: $1"; usage ;;
    esac
done

if [ -z "$TEST_DIR" ] || [ ${#PLUGIN_MAP[@]} -eq 0 ]; then
    usage
fi

echo "Plugin caricati (Analisi in memoria):"
for key in "${!PLUGIN_MAP[@]}"; do
    echo "  $key -> ${PLUGIN_MAP[$key]}"
done
echo "---------------------------------"

for cpp_file in "$TEST_DIR"/*.cpp; do
    [ -e "$cpp_file" ] || continue
    filename=$(basename "$cpp_file" .cpp)

    plugin="${PLUGIN_MAP[$filename]}"
    pass="${PASS_MAP[$filename]}"

    if [ -z "$plugin" ]; then
        echo "Skipping '$filename': nessun plugin associato."
        continue
    fi

    echo "Processing: $filename (Pass: $pass)"
    
    # Esecuzione combinata:
    # 1. clang++ genera l'IR e lo spara nello stdout (-o -)
    # 2. opt riceve l'IR dallo stdin, carica il plugin ed esegue il pass
    # 3. L'output trasformato di opt viene buttato in /dev/null
    
    clang++ -S -emit-llvm -O0 -Xclang -disable-O0-optnone -fno-discard-value-names -g "$cpp_file" -o - | \
    opt -load-pass-plugin "$plugin" -passes="$pass" -o /dev/null

    # Salviamo subito lo stato della pipe in una variabile locale
    rc_pipe=("${PIPESTATUS[@]}")

    if [ "${rc_pipe[0]}" -ne 0 ]; then
        echo "   [ERROR] Errore in Clang per $filename"
    elif [ "${rc_pipe[1]}" -ne 0 ]; then
        echo "   [ERROR] Errore in Opt (Pass) per $filename"
    else
        echo "   [OK] Analisi completata per $filename"
    fi
    echo "---------------------------------"
done