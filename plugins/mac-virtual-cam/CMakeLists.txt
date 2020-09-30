project(mac-virtual-cam)

find_package(Libobs REQUIRED)

find_library(AVFOUNDATION AVFoundation)
find_library(COCOA Cocoa)
find_library(COREFOUNDATION CoreFoundation)
find_library(COREMEDIA CoreMedia)
find_library(COREVIDEO CoreVideo)
find_library(COCOA Cocoa)
find_library(COREMEDIAIO CoreMediaIO)
find_library(IOSURFACE IOSurface)
find_library(IOKIT IOKit)

configure_file(
	Defines.h.in
	../../../plugins/mac-virtual-cam/Defines.generated.h
)

include_directories(${AVFOUNDATION}
                    ${COCOA}
                    ${COREFOUNDATION}
                    ${COREMEDIA}
                    ${COREVIDEO}
                    ${COREMEDIAIO}
                    ${COCOA}
                    ${IOSURFACE}
					${LIBOBS_INCLUDE_DIR}
					${CMAKE_SOURCE_DIR}/libobs
                    ./
                    ../../common)


set(mac-virtual-cam_SOURCES
	Defines.generated.h
	plugin-main.mm
	MachServer.h
	MachServer.mm
	../../../common/MachProtocol.h)

add_library(mac-virtual-cam MODULE
	${mac-virtual-cam_SOURCES}
	${mac-virtual-cam_HEADERS})

target_link_libraries(mac-virtual-cam
	${AVFOUNDATION}
	${COCOA}
	${COREFOUNDATION}
	${COREMEDIA}
	${COREVIDEO}
	${COREMEDIAIO}
	${IOSURFACE}
	${IOKIT}
	${LIBOBS_LIB}
	)

set_target_properties(mac-virtual-cam PROPERTIES FOLDER "plugins")

set_target_properties(mac-virtual-cam PROPERTIES
	PREFIX ""
	OUTPUT_NAME "adept-mac-virtualcam"
	COMPILE_FLAGS "-std=gnu++14 -stdlib=libc++ -fobjc-arc -fobjc-weak"
)

install_obs_plugin(mac-virtual-cam)