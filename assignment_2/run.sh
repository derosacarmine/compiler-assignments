#!/bin/bash

find_llvm_bin() {
    if command -v clang++ &>/dev/null && command -v opt &>/dev/null; then
        return 0
    fi

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

    echo "Errore: clang++ e opt non trovati. Installa LLVM o aggiungilo al PATH."
    exit 1
}

find_llvm_bin

declare -A PLUGIN_MAP
declare -A PASS_MAP

usage() {
    echo "Usage: $0 -t <test_dir> [options]"
    echo ""
    echo "Options:"
    echo "  -t <dir>        Path to test directory"
    echo "  --vbe <path>    Plugin .so path for very_busy_expressions   (pass: very-busy-expressions)"
    echo "  --cp  <path>    Plugin .so path for constant_propagation    (pass: constant-propagation)"
    echo "  --dom <path>    Plugin .so path for dominator_analysis      (pass: dominator-analysis)"
    echo ""
    echo "Example:"
    echo "  $0 -t ./tests --vbe ./build/VBE.so --cp ./build/CP.so --dom ./build/DOM.so"
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
        *) echo "Unrecognized argument: $1"; usage ;;
    esac
done

if [ -z "$TEST_DIR" ]; then
    echo "Error: you must specify test directory with -t."
    usage
fi

if [ ! -d "$TEST_DIR" ]; then
    echo "Error: directory '$TEST_DIR' not found."
    exit 1
fi

if [ ${#PLUGIN_MAP[@]} -eq 0 ]; then
    echo "Error: you must specify at least one plugin (--vbe, --cp, --dom)."
    usage
fi

for key in "${!PLUGIN_MAP[@]}"; do
    plugin="${PLUGIN_MAP[$key]}"
    if [ ! -f "$plugin" ]; then
        echo "Error: plugin not found for '$key': $plugin"
        exit 1
    fi
done

LL_DIR="$TEST_DIR/ll"
mkdir -p "$LL_DIR"

echo "Plugin loaded:"
for key in "${!PLUGIN_MAP[@]}"; do
    echo "  $key -> ${PLUGIN_MAP[$key]} (pass: ${PASS_MAP[$key]})"
done
echo "---------------------------------"

for cpp_file in "$TEST_DIR"/*.cpp; do
    [ -e "$cpp_file" ] || continue
    filename=$(basename "$cpp_file" .cpp)

    plugin="${PLUGIN_MAP[$filename]}"
    pass="${PASS_MAP[$filename]}"

    if [ -z "$plugin" ]; then
        echo "Skipping '$filename': no plugin associated."
        continue
    fi

    echo "Processing: $filename  (pass: $pass)"
    
    if !  clang++ -S -emit-llvm -O0 -Xclang -disable-O0-optnone -fno-discard-value-names "$cpp_file" -o "$LL_DIR/$filename.ll"; then
        echo "   [ERROR] Failed compilation for: $filename"
        continue
    fi

    # mem2reg per promuovere allocas
    #opt -load-pass-plugin "$plugin" -passes="mem2reg" \
    #    "$LL_DIR/$filename.ll" -S -o "$LL_DIR/$filename.ll"

    # Esecuzione della pass specifica
    opt -load-pass-plugin "$plugin" -passes="$pass" \
        "$LL_DIR/$filename.ll" -S -disable-output

    echo "   [OK] Completed -> $OPT_DIR/${filename}_opt.ll"
done