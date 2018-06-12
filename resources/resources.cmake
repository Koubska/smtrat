include(ExternalProject)
set_directory_properties(PROPERTIES EP_PREFIX ${CMAKE_BINARY_DIR}/resources)

add_custom_target(resources)

if("${CMAKE_GENERATOR}" MATCHES "MAKE")
	set(CMAKE_MAKE_PROGRAM "$(MAKE)")
endif()

# Make sure that libraries from /usr/lib et al are found before OSX frameworks
set(CMAKE_FIND_FRAMEWORK "LAST")

###############
##### Load resources
###############

##### CArL
include(resources/carl-required-version.cmake)
if (NOT "${CARL_REQUIRED_VERSION}" MATCHES "[0-9]+\\.[0-9]+(\\.[0-9]+)?")
	message(STATUS "Resetting invalid carl version ${CARL_REQUIRED_VERSION}")
	set(CARL_REQUIRED_VERSION "0.0")
endif()
find_package(carl ${CARL_REQUIRED_VERSION})
if(carl_FOUND)
	message(STATUS "Use system version of carl ${carl_VERSION} (required ${CARL_REQUIRED_VERSION})")
	add_custom_target(doxygen-build DEPENDS Doxygen::doxygen)
else()
	include(resources/carl.cmake)
	message(STATUS "Use shipped version of carl ${carl_VERSION} (required ${CARL_REQUIRED_VERSION})")
	add_custom_target(doxygen-build DEPENDS Doxygen::doxygen CArL-EP-doxygen)
endif()

add_custom_target(carl-required-version
	COMMAND ${CMAKE_COMMAND} -D CARL_DIR=${carl_INCLUDE_DIR} -D DEST_FILE=${CMAKE_SOURCE_DIR}/resources/carl-required-version.cmake -P ${CMAKE_SOURCE_DIR}/resources/carl-write-required-version.cmake
	BYPRODUCTS ${CMAKE_SOURCE_DIR}/resources/carl-required-version.cmake
	WORKING_DIRECTORY ${carl_INCLUDE_DIR}
	VERBATIM
)
