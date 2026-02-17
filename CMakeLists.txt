cmake_minimum_required(VERSION 3.21)
project(ButterscotchC VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /WX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

add_library(butterscotch_core
  src/app.c
  src/data/form_reader.c
  src/vm/vm.c
  src/runtime/game_runner.c
  src/builtin/builtin_registry.c
)

target_include_directories(butterscotch_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(butterscotch_cli src/main.c)
target_link_libraries(butterscotch_cli PRIVATE butterscotch_core)

option(BS_BUILD_SDL_FRONTEND "Build SDL frontend executable" ON)
if(BS_BUILD_SDL_FRONTEND)
  set(BS_RUNTIME_DEP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_deps.cmake")
  if(WIN32 AND EXISTS "C:/msys64/ucrt64/lib/cmake/SDL2/SDL2Config.cmake")
    list(APPEND CMAKE_PREFIX_PATH "C:/msys64/ucrt64")
  endif()
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    find_package(SDL2_image QUIET CONFIG)
    find_package(SDL2_mixer QUIET CONFIG NAMES sdl2_mixer SDL2_mixer)
    add_executable(butterscotch_sdl
      src/main_sdl.c
      src/platform/sdl_frontend.c
    )
    target_link_libraries(butterscotch_sdl PRIVATE butterscotch_core SDL2::SDL2)
    if(SDL2_image_FOUND)
      target_link_libraries(butterscotch_sdl PRIVATE SDL2_image::SDL2_image)
      target_compile_definitions(butterscotch_sdl PRIVATE BS_HAVE_SDL_IMAGE=1)
    endif()
    if(SDL2_mixer_FOUND OR sdl2_mixer_FOUND)
      target_link_libraries(butterscotch_sdl PRIVATE SDL2_mixer::SDL2_mixer)
      target_compile_definitions(butterscotch_sdl PRIVATE BS_HAVE_SDL_MIXER=1)
    endif()
    if(WIN32)
      # Ensure the SDL runtime DLLs are placed next to the executable.
      add_custom_command(TARGET butterscotch_sdl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:butterscotch_sdl>
                $<TARGET_FILE_DIR:butterscotch_sdl>
        COMMAND_EXPAND_LISTS
      )

      # MinGW SDL2_image packages depend on extra codec/image DLLs that may
      # not be captured by TARGET_RUNTIME_DLLS. Resolve and copy full runtime
      # dependency closure from the compiler's bin directory.
      if(MINGW AND EXISTS "${BS_RUNTIME_DEP_SCRIPT}")
        get_filename_component(BS_COMPILER_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
        set(BS_RUNTIME_DEP_ARGS
          -DINPUT_EXE=$<TARGET_FILE:butterscotch_sdl>
          -DOUTPUT_DIR=$<TARGET_FILE_DIR:butterscotch_sdl>
          -DSEARCH_DIR_COMPILER=${BS_COMPILER_BIN_DIR}
          -DSEARCH_DIR_SDL2=$<TARGET_FILE_DIR:SDL2::SDL2>
        )
        if(SDL2_image_FOUND)
          list(APPEND BS_RUNTIME_DEP_ARGS
            -DSEARCH_DIR_SDL2_IMAGE=$<TARGET_FILE_DIR:SDL2_image::SDL2_image>
          )
        endif()
        if(SDL2_mixer_FOUND OR sdl2_mixer_FOUND)
          list(APPEND BS_RUNTIME_DEP_ARGS
            -DSEARCH_DIR_SDL2_MIXER=$<TARGET_FILE_DIR:SDL2_mixer::SDL2_mixer>
          )
        endif()
        add_custom_command(TARGET butterscotch_sdl POST_BUILD
          COMMAND ${CMAKE_COMMAND} ${BS_RUNTIME_DEP_ARGS}
                  -P "${BS_RUNTIME_DEP_SCRIPT}"
          COMMAND_EXPAND_LISTS
          VERBATIM
        )
      endif()
    endif()
  else()
    message(WARNING "SDL2 not found: skipping butterscotch_sdl build")
  endif()
endif()
