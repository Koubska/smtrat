#find_package(Boost COMPONENTS uCI_PIPELINE_SOURCEnit_test_framework REQUIRED)

# add_definitions(-DBOOST_TEST_DYN_LINK)
# 
# include_directories(${CMAKE_SOURCE_DIR}/src
#                     ${CMAKE_SOURCE_DIR}/src/cli
#                     ${CMAKE_BINARY_DIR}/src/cli
# )
# 
# add_subdirectory(cad)
# add_subdirectory(pseudobool)
# add_subdirectory(onecellcad)
# add_subdirectory(nlsat)
# add_subdirectory(mcsat)
# add_subdirectory(mcsatvs)
# add_subdirectory(fourierMotzkinQE)


add_custom_target(all-tests)

add_library(TestCommon STATIC Common.cpp)
target_link_libraries(TestCommon)
add_dependencies(TestCommon resources)


################################
# Unit Tests
################################

add_subdirectory(smtrat-cadcells)