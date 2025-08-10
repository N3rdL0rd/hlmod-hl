if(FALSE)
    if(MSVC)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /SAFESEH:NO")
    endif()

    find_package(ffmpeg REQUIRED)

    add_library(video.hdll SHARED video.c)

    set_as_hdll(video)

    target_include_directories(video.hdll
        PRIVATE
            ${FFMPEG_INCLUDE_DIRS}
    )

    target_link_libraries(video.hdll
        PRIVATE
            libhl
            ${FFMPEG_LIBRARIES}
    )

    install(
        TARGETS
            video.hdll
        DESTINATION ${HDLL_DESTINATION}
    )
endif()