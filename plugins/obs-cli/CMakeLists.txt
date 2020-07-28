project(obs-cli)

set(obs-cli_SOURCES
	obs-cli.c)

set(obs-cli_HEADERS
	obs-cli.h)

add_executable(obs-cli
	${obs-cli_SOURCES}
	${obs-cli_HEADERS})

set(obs-cli_LIBRARIES
	libobs
	)

if(MSVC)
	set(obs-cli_PLATFORM_DEPS
	 	ws2_32
        shcore
		w32-pthreads)
endif()

include_directories(${OBS_JANSSON_INCLUDE_DIRS})

target_link_libraries(obs-cli
	${obs-cli_LIBRARIES}
	${OBS_JANSSON_IMPORT}
	${obs-cli_PLATFORM_DEPS}
	)

set_target_properties(obs-cli PROPERTIES FOLDER "plugins/obs-cli")

install_obs_core(obs-cli)
