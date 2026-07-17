add_library(ccc_core STATIC
    Pad.cpp
    Model.cpp
    Bem.cpp
    Connectivity.cpp
    FastCapIo.cpp
)

target_include_directories(ccc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(ccc_core PUBLIC cxx_std_20)
target_compile_definitions(ccc_core PUBLIC CCC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(ccc_core PUBLIC Eigen3::Eigen)
if(OpenMP_CXX_FOUND)
    target_link_libraries(ccc_core PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(ccc_core PUBLIC CCC_HAS_OPENMP=1)
endif()
if(CCC_HAS_CUDA)
    target_link_libraries(ccc_core PUBLIC CUDA::cudart CUDA::cusolver CUDA::cublas)
    target_compile_definitions(ccc_core PUBLIC CCC_HAS_CUDA=1)
endif()
