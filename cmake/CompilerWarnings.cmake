function(ovl_set_compile_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:__cplusplus
            /wd4244  # conversion, possible loss of data (legacy ulong → uint32_t)
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wnon-virtual-dtor -Wold-style-cast
            -Wcast-align -Wunused -Woverloaded-virtual
            -Wno-sign-compare
        )
    endif()
endfunction()
