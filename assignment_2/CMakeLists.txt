cmake_minimum_required(VERSION 3.20)
project(LLVMAnalysisPasses)

#===============================================================================
# 1. LOAD LLVM CONFIGURATION
#===============================================================================
set(LT_LLVM_INSTALL_DIR "" CACHE PATH "LLVM installation directory")

# Assicuriamoci che CMake cerchi nelle cartelle giuste dell'installazione
list(APPEND CMAKE_PREFIX_PATH "${LT_LLVM_INSTALL_DIR}")
list(APPEND CMAKE_PREFIX_PATH "${LT_LLVM_INSTALL_DIR}/lib/cmake/llvm/")

# Rimuovi il "19" specifico se continua a darti errore, 
# oppure usa la firma estesa:
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_VERSION}")
message(STATUS "Using LLVMConfig.cmake from: ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

#===============================================================================
# 2. BUILD CONFIGURATION
#===============================================================================
# LLVM 19 richiede C++17 o C++20
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Consistenza con RTTI di LLVM
if(NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

#===============================================================================
# 3. ADD THE TARGETS
#===============================================================================

# Funzione helper per evitare ripetizioni
function(add_analysis_pass PASS_NAME SOURCE_FILE)
    add_library(${PASS_NAME} MODULE ${SOURCE_FILE})
    
    # Rimuove il prefisso 'lib' (opzionale, per avere nomi puliti come DominatorPass.so)
    set_target_properties(${PASS_NAME} PROPERTIES PREFIX "")

    # Gestione simboli per macOS (Darwin)
    if(APPLE)
        target_link_libraries(${PASS_NAME} PRIVATE "-undefined dynamic_lookup")
    endif()
endfunction()

# Compila i 3 file come moduli separati
add_analysis_pass(DominatorAnalysis src/dominator_analysis.cpp)
add_analysis_pass(ConstantPropagation src/constant_propagation.cpp)
add_analysis_pass(VeryBusyExpressions src/very_busy_expressions.cpp)

# Se preferisci una singola libreria che contenga tutto, commenta sopra e usa:
# add_library(AllAnalysis SHARED 
#     dominator_analysis.cpp 
#     constant_propagation.cpp 
#     very_busy_expression.cpp
# )