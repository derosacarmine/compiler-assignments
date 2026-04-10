#!/bin/bash
#!/bin/bash

# Ricerca automatica di clang++ e opt
find_llvm_bin() {
    # Già nel PATH?
    if command -v clang++ &>/dev/null && command -v opt &>/dev/null; then
        return 0
    fi

    # Cerca in path comuni
    local search_paths=(
        /usr/lib/llvm-*/bin
        /usr/local/lib/llvm-*/bin
        /opt/llvm-*/bin
        /opt/homebrew/opt/llvm/bin        # macOS Homebrew
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

    # Ricerca più profonda con find (lenta ma sicura come fallback)
    echo "Ricerca LLVM nel filesystem (potrebbe richiedere qualche secondo)..."
    local found
    found=$(find /usr /opt "$HOME" -name "clang++" -type f 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        local bin_dir
        bin_dir=$(dirname "$found")
        export PATH="$bin_dir:$PATH"
        echo "LLVM trovato in: $bin_dir"
        return 0
    fi

    echo "Errore: clang++ e opt non trovati. Installa LLVM o aggiungilo al PATH."
    exit 1
}

find_llvm_bin

# Mappa: nome_file_test -> (plugin_path, pass_name)
declare -A PLUGIN_MAP
declare -A PASS_MAP

usage() {
    echo "Usage: $0 -t <test_dir> [options]"
    echo ""
    echo "Options:"
    echo "  -t <dir>        Directory contenente i .cpp di test"
    echo "  --vbe <path>    Plugin .so per very_busy_expressions   (pass: very-busy-expressions)"
    echo "  --cp  <path>    Plugin .so per constant_propagation    (pass: constant-propagation)"
    echo "  --dom <path>    Plugin .so per dominator_analysis      (pass: dominator-analysis)"
    echo ""
    echo "Esempio:"
    echo "  $0 -t ./tests --vbe ./build/VBE.so --cp ./build/CP.so --dom ./build/DOM.so"
    exit 1
}

TEST_DIR=""

# Parsing manuale degli argomenti (getopts non supporta --long-options)
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

# Validazioni
if [ -z "$TEST_DIR" ]; then
    echo "Errore: devi specificare la directory di test con -t."
    usage
fi

if [ ! -d "$TEST_DIR" ]; then
    echo "Errore: directory '$TEST_DIR' non trovata."
    exit 1
fi

if [ ${#PLUGIN_MAP[@]} -eq 0 ]; then
    echo "Errore: devi specificare almeno un plugin (--vbe, --cp, --dom)."
    usage
fi

# Verifica che i plugin specificati esistano
for key in "${!PLUGIN_MAP[@]}"; do
    plugin="${PLUGIN_MAP[$key]}"
    if [ ! -f "$plugin" ]; then
        echo "Errore: plugin non trovato per '$key': $plugin"
        exit 1
    fi
done

LL_DIR="$TEST_DIR/ll"
# OPT_DIR="$TEST_DIR/optimized"
# mkdir -p "$LL_DIR" "$OPT_DIR"

echo "Plugin caricati:"
for key in "${!PLUGIN_MAP[@]}"; do
    echo "  $key -> ${PLUGIN_MAP[$key]} (pass: ${PASS_MAP[$key]})"
done
echo "---------------------------------"

for cpp_file in "$TEST_DIR"/*.cpp; do
    [ -e "$cpp_file" ] || continue
    filename=$(basename "$cpp_file" .cpp)

    # Cerca il plugin corrispondente a questo file
    plugin="${PLUGIN_MAP[$filename]}"
    pass="${PASS_MAP[$filename]}"

    if [ -z "$plugin" ]; then
        echo "Skipping '$filename': nessun plugin associato."
        continue
    fi

    echo "Processing: $filename  (pass: $pass)"
    
    if !  clang++ -S -emit-llvm -O0 -Xclang -disable-O0-optnone -fno-discard-value-names "$cpp_file" -o "$LL_DIR/$filename.ll"; then
        echo "   [ERROR] Compilazione fallita per: $filename"
        continue
    fi

    # mem2reg per promuovere allocas
    opt -load-pass-plugin "$plugin" -passes="mem2reg" \
       "$LL_DIR/$filename.ll" -S -o "$LL_DIR/$filename.ll"

    # Esecuzione della pass specifica
    opt -load-pass-plugin "$plugin" -passes="$pass" \
        "$LL_DIR/$filename.ll" -S -disable-output

    #echo "   [OK] Completato -> $OPT_DIR/${filename}_opt.ll"
done